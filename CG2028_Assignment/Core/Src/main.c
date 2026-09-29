/******************************************************************************
 * @file           : main.c
 * @brief          : CG2028 Assignment - ElderCare Wearable Safety Companion
 * @author         : Hou Linxin
 * (c) CG2028 Teaching Team
 *
 * Wrist-worn fall detector.
 * Pipeline: sensor -> EWMA per axis (mov_avg.s) -> features (acc_g, gyro_dps,
 * pose_deg) -> state machine -> incident module (LED, buzzer, OLED, Wi-Fi).
 * Detection runs from the TIM7 interrupt (sampler.c); the main loop only does
 * the slow stuff (screen, UART, network).
 ******************************************************************************/

/*--------------------------- Includes ---------------------------------------*/
#include "main.h"
#include "incident.h"
#include "log.h"
#include "net.h"
#include "oled.h"
#include "sampler.h"
#include "screens.h"
#include "../../Drivers/BSP/B-L4S5I-IOT01/stm32l4s5i_iot01_accelero.h"
#include "../../Drivers/BSP/B-L4S5I-IOT01/stm32l4s5i_iot01_gyro.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/*--------------------------- Configuration ----------------------------------*/
/* All times are in ms and converted to loop counts with MS_TO_LOOPS, so the
 * loop period can change without retuning anything. */

/* Timing */
#define LOOP_MS             SAMPLER_PERIOD_MS   /* 20 ms, taken from sampler.h so the timer and thresholds match */
#define WARMUP_MS           1000    /* ms, let the filters settle before detecting */
#define LED_NORMAL_HALF_MS  1000    /* ms, LED half-period when not in FALL */
#define LED_FALL_HALF_MS    150     /* ms, LED half-period in FALL */

/* ms -> loops, rounded to nearest */
#define MS_TO_LOOPS(ms)     (((ms) + LOOP_MS / 2) / LOOP_MS)

/* EWMA alpha (%) - higher follows faster but lets more noise through */
#define ALPHA_ACC           50
#define ALPHA_GYRO          50
#define ALPHA_POSE          10      /* slow pose tracker on top of the filtered accel */

/* Thresholds (tuned from our drop logs) */
#define FREEFALL_G          0.6f    /* g, below this counts as free fall */
#define FREEFALL_MIN_MS     60      /* ms (3 loops), arm swings dip for less than this */
#define FREEFALL_SURE_LOOPS 6       /* 120 ms of dip = definitely free fall (drops gave 10-15, hands 0-3) */
#define ROT_DPS             200.0f  /* dps, tumbling; normal wrist gestures stay under ~150 */
#define IMPACT_G            1.7f    /* g, gentle handling peaked at 1.6, drops at 1.8+ */
#define POSE_CHANGE_DEG     30.0f   /* deg from pre-fall pose needed to call it a new pose */
#define POSE_RETURN_DEG     30.0f   /* deg, closer than this = back in the old pose */
#define STILL_DPS           20.0f   /* dps, lying still reads 2-10, held in hand ~18 */
#define MOVE_DPS            40.0f   /* dps, counts as movement when recovering from FALL */

/* Time windows */
#define SUSPECT_WINDOW_MS   1000    /* ms to wait for the impact after the dip */
#define IMPACT_SETTLE_MS    300     /* ms ignored after impact (bouncing) */
#define IMPACT_WINDOW_MS    8000    /* ms to see the wearer lie still before giving up */
#define STILL_HOLD_MS       3500    /* ms still in a row to confirm; a seated hand moves every 2-3 s */
#define RECOVER_MOVE_MS     3000    /* ms of net movement to leave FALL */
#define RECOVER_POSE_MS     2000    /* ms back in the old pose to leave FALL */

/* Pose tracker */
#define POSE_MIN_G          0.8f    /* only learn the pose when |a| is near 1 g, */
#define POSE_MAX_G          1.2f    /* i.e. the wrist is roughly at rest */
#define POSE_REF_MIN_MG     700     /* mg, shorter reference = not set / accel dead */

/* Sensor start-up */
#define INIT_ATTEMPTS       5       /* BSP init sometimes fails right after reset */
#define INIT_RETRY_MS       10
#define ACC_OK_MIN_G        0.7f    /* a working accel at rest reads about 1 g */
#define ACC_OK_MAX_G        1.3f

/* Loop-count versions of the above */
#define WARMUP_LOOPS        MS_TO_LOOPS(WARMUP_MS)
#define LED_NORMAL_LOOPS    MS_TO_LOOPS(LED_NORMAL_HALF_MS)
#define LED_FALL_LOOPS      MS_TO_LOOPS(LED_FALL_HALF_MS)
#define FREEFALL_MIN_LOOPS  MS_TO_LOOPS(FREEFALL_MIN_MS)
#define SUSPECT_LOOPS       MS_TO_LOOPS(SUSPECT_WINDOW_MS)
#define IMPACT_SETTLE_LOOPS MS_TO_LOOPS(IMPACT_SETTLE_MS)
#define IMPACT_LOOPS        MS_TO_LOOPS(IMPACT_WINDOW_MS)
#define STILL_LOOPS         MS_TO_LOOPS(STILL_HOLD_MS)
#define RECOVER_MOVE_LOOPS  MS_TO_LOOPS(RECOVER_MOVE_MS)
#define RECOVER_POSE_LOOPS  MS_TO_LOOPS(RECOVER_POSE_MS)

/* LSM6DSL register values written after the BSP init.
 * CTRL1_XL / CTRL2_G: bits 7:4 = ODR, bits 3:2 = full scale. */
#define LSM6DSL_ODR_FS_MASK 0xFCu
#define ACC_CTRL1_XL        (LSM6DSL_ODR_104Hz | LSM6DSL_ACC_FULLSCALE_8G)  /* 104 Hz, +-8 g */
#define GYRO_CTRL2_G        (LSM6DSL_ODR_104Hz | LSM6DSL_GYRO_FS_2000)      /* 104 Hz, +-2000 dps */

#define RAD_TO_DEG          57.29578f

#define HEALTH_EVERY_LOOPS  MS_TO_LOOPS(10000)  /* print a health line every 10 s */

/*--------------------------- Declarations -----------------------------------*/
static void UART1_Init(void);
static void UART_Send(const char *text);
static void detect_say(const char *text);
static float magnitude(const int v[3]);

/* Called from the ISR, so queue the text instead of printing it (log.c). */
static void detect_say(const char *text)
{
    log_line(text);
}

/* ewma_filter is the assembly version (mov_avg.s) used for detection.
 * ewma_filter_C is only used to cross-check it. */
extern int ewma_filter(int new_data, int old_output, int alpha_percent);
int ewma_filter_C(int new_data, int old_output, int alpha_percent);

UART_HandleTypeDef huart1;

static const char *const STATE_NAMES[] = { "NORMAL", "SUSPECT", "IMPACT", "FALL" };

/* Detector state */
static incident_t incident = { 0, STATE_NORMAL, 0 };
static uint32_t state_loops;        /* loops in the current state */
static uint32_t streak_loops;       /* current streak (still / back in pose) */
static uint32_t dip_loops;          /* current run of loops below FREEFALL_G */
static uint32_t dip_max_loops;      /* longest run seen in this SUSPECT */
static uint32_t move_loops;         /* leaky movement count while in FALL */
static uint32_t max_still_loops;    /* longest still streak in this IMPACT (for the log) */
static float    min_gyro_dps;       /* lowest gyro after settling (for the log) */
static float    suspect_peak_g;     /* peak acc_g in this SUSPECT */

/* pose_mg is a slow EWMA of the accel, only updated in NORMAL near 1 g. It lags
 * by ~200 ms, so when SUSPECT starts it still has the pose from before the
 * fall. That copy is pre_fall_ref, and pose_deg is measured against it. */
static int  pose_mg[3];
static int  pre_fall_ref[3];
static int  pose_ref_valid;

/* Set by incident.c when the wearer cancels; checked in STATE_FALL. */
volatile uint8_t fall_cancel_request = 0;

/* Default (empty) hooks, overridden by incident.c */
__attribute__((weak)) void on_fall_confirmed(const incident_t *inc) { (void)inc; }
__attribute__((weak)) void on_recovered(const incident_t *inc) { (void)inc; }

/*--------------------------- Sensor setup -----------------------------------*/

/* Init both sensors with retries - after a reset the first I2C transfer
 * sometimes fails and the accel then reads 0 on every axis. */
static void sensors_init(void)
{
    ACCELERO_StatusTypeDef acc_status = ACCELERO_ERROR;
    uint8_t gyro_status = GYRO_ERROR;

    for (int attempt = 0; (attempt < INIT_ATTEMPTS) && (acc_status != ACCELERO_OK); attempt++)
    {
        if (attempt > 0)
        {
            HAL_Delay(INIT_RETRY_MS);
        }
        acc_status = BSP_ACCELERO_Init();
    }
    detect_say((acc_status == ACCELERO_OK) ? ">> ACCEL INIT OK\r\n" : ">> ACCEL INIT FAILED\r\n");

    for (int attempt = 0; (attempt < INIT_ATTEMPTS) && (gyro_status != GYRO_OK); attempt++)
    {
        if (attempt > 0)
        {
            HAL_Delay(INIT_RETRY_MS);
        }
        gyro_status = BSP_GYRO_Init();
    }
    detect_say((gyro_status == GYRO_OK) ? ">> GYRO INIT OK\r\n" : ">> GYRO INIT FAILED\r\n");
}

/* BSP default is +-2 g, which clipped every impact. Set +-8 g and 104 Hz on
 * both sensors. The BSP read functions check these registers for the
 * sensitivity, so the readings are still in mg / mdps. */
static void sensor_configure(void)
{
    uint8_t reg;

    reg = SENSOR_IO_Read(LSM6DSL_ACC_GYRO_I2C_ADDRESS_LOW, LSM6DSL_ACC_GYRO_CTRL1_XL);
    reg = (uint8_t)((reg & ~LSM6DSL_ODR_FS_MASK) | ACC_CTRL1_XL);
    SENSOR_IO_Write(LSM6DSL_ACC_GYRO_I2C_ADDRESS_LOW, LSM6DSL_ACC_GYRO_CTRL1_XL, reg);

    reg = SENSOR_IO_Read(LSM6DSL_ACC_GYRO_I2C_ADDRESS_LOW, LSM6DSL_ACC_GYRO_CTRL2_G);
    reg = (uint8_t)((reg & ~LSM6DSL_ODR_FS_MASK) | GYRO_CTRL2_G);
    SENSOR_IO_Write(LSM6DSL_ACC_GYRO_I2C_ADDRESS_LOW, LSM6DSL_ACC_GYRO_CTRL2_G, reg);
}

/*--------------------------- State machine ----------------------------------*/

/* Every transition goes through here: log it, reset the counters, fire hooks. */
static void change_state(fall_state_t next, uint32_t now_ms, const char *note)
{
    fall_state_t prev = incident.state;
    char line[48];

    snprintf(line, sizeof(line), ">> %s -> %s %s\r\n", STATE_NAMES[prev], STATE_NAMES[next], note);
    detect_say(line);

    incident.state = next;
    state_loops = 0;
    streak_loops = 0;
    dip_loops = 0;
    move_loops = 0;
    max_still_loops = 0;
    min_gyro_dps = FLT_MAX;
    suspect_peak_g = 0.0f;

    /* Clear any old cancel request so a press before the fall can't cancel it */
    if (next == STATE_FALL)
    {
        fall_cancel_request = 0;
        incident.id++;
        incident.t_start_ms = now_ms;
        on_fall_confirmed(&incident);
    }
    else if (prev == STATE_FALL)
    {
        on_recovered(&incident);
    }
}

/* Freeze the pre-fall pose and go to SUSPECT. If the reference is too short
 * to trust we skip the pose check later instead of blocking the fall. */
static void enter_suspect(const char *cause, uint32_t now_ms)
{
    char note[16];
    uint32_t run = dip_loops;

    memcpy(pre_fall_ref, pose_mg, sizeof(pre_fall_ref));
    pose_ref_valid = (magnitude(pre_fall_ref) >= (float)POSE_REF_MIN_MG);

    snprintf(note, sizeof(note), "%s=%lu", cause, run);
    change_state(STATE_SUSPECT, now_ms, note);

    /* change_state zeroed dip_loops but the dip is still going, so restore it */
    dip_loops = run;
    dip_max_loops = run;

    if (!pose_ref_valid)
    {
        detect_say(">> POSE REF INVALID\r\n");
    }
}

/* Print the peak and dip length before leaving SUSPECT (used for tuning) */
static void end_suspect(fall_state_t next, uint32_t now_ms)
{
    char line[40];

    snprintf(line, sizeof(line), ">> PEAK %.2f g dip=%lu\r\n", suspect_peak_g, dip_max_loops);
    detect_say(line);
    change_state(next, now_ms, "");
}

/* A long enough dip is a real drop however the board lands, so we don't need
 * the pose change as well. Impact and still hold are still required. */
static int pose_gate_waived(void)
{
    return dip_max_loops >= FREEFALL_SURE_LOOPS;
}

/* Print pose / longest still streak / min gyro before leaving IMPACT, so a
 * rejected drop shows which check it failed. */
static void end_impact(fall_state_t next, float pose_deg, uint32_t now_ms)
{
    char line[96];
    char waived[24] = "";

    if (pose_gate_waived())
    {
        snprintf(waived, sizeof(waived), " (waived, dip=%lu)", dip_max_loops);
    }
    snprintf(line, sizeof(line),
             ">> IMPACT END pose=%.0f%s max_still_ms=%lu min_gyro=%.1f\r\n",
             pose_deg, waived, max_still_loops * LOOP_MS, min_gyro_dps);
    detect_say(line);
    change_state(next, now_ms, "");
}

/* NORMAL -> SUSPECT -> IMPACT -> FALL, one step per loop */
static void update_state(float acc_g, float gyro_dps, float pose_deg, uint32_t now_ms)
{
    switch (incident.state)
    {
    case STATE_NORMAL:
        /* dip has to last FREEFALL_MIN_LOOPS in a row; fast rotation enters straight away */
        dip_loops = (acc_g < FREEFALL_G) ? dip_loops + 1 : 0;
        if (dip_loops >= FREEFALL_MIN_LOOPS)
        {
            enter_suspect("dip", now_ms);
        }
        else if (gyro_dps > ROT_DPS)
        {
            enter_suspect("rot", now_ms);
        }
        break;

    case STATE_SUSPECT:
        /* keep tracking the dip length (for the waiver) and the peak */
        dip_loops = (acc_g < FREEFALL_G) ? dip_loops + 1 : 0;
        if (dip_loops > dip_max_loops)
        {
            dip_max_loops = dip_loops;
        }
        if (acc_g > suspect_peak_g)
        {
            suspect_peak_g = acc_g;
        }
        if (suspect_peak_g > IMPACT_G)
        {
            end_suspect(STATE_IMPACT, now_ms);
        }
        else if (++state_loops >= SUSPECT_LOOPS)
        {
            end_suspect(STATE_NORMAL, now_ms);
        }
        break;

    case STATE_IMPACT:
        /* skip the bounce right after the hit */
        if (++state_loops <= IMPACT_SETTLE_LOOPS)
        {
            break;
        }

        if (gyro_dps < min_gyro_dps)
        {
            min_gyro_dps = gyro_dps;
        }

        /* stillness is counted from the gyro only; pose is checked at the end */
        streak_loops = (gyro_dps < STILL_DPS) ? streak_loops + 1 : 0;
        if (streak_loops > max_still_loops)
        {
            max_still_loops = streak_loops;
        }

        /* FALL = still long enough AND in a new pose (unless waived / bad ref) */
        int new_pose = pose_gate_waived() || !pose_ref_valid || (pose_deg > POSE_CHANGE_DEG);
        if ((streak_loops >= STILL_LOOPS) && new_pose)
        {
            end_impact(STATE_FALL, pose_deg, now_ms);
        }
        else if (state_loops >= IMPACT_LOOPS)
        {
            end_impact(STATE_NORMAL, pose_deg, now_ms);
        }
        break;

    case STATE_FALL:
        /* Latched. Exits: cancel button, ~3 s of net movement (leaky count so
         * a twitch doesn't clear it), or back in the old pose for 2 s. */
        if (fall_cancel_request)
        {
            fall_cancel_request = 0;
            change_state(STATE_NORMAL, now_ms, "cancel");
            break;
        }

        if (gyro_dps > MOVE_DPS)
        {
            move_loops++;
        }
        else if (move_loops > 0)
        {
            move_loops--;
        }
        streak_loops = (pose_ref_valid && (pose_deg < POSE_RETURN_DEG)) ? streak_loops + 1 : 0;

        if (move_loops >= RECOVER_MOVE_LOOPS)
        {
            change_state(STATE_NORMAL, now_ms, "moving");
        }
        else if (streak_loops >= RECOVER_POSE_LOOPS)
        {
            change_state(STATE_NORMAL, now_ms, "pose");
        }
        break;
    }
}

/*--------------------------- Features ---------------------------------------*/

/* sqrt(x^2 + y^2 + z^2). Float because mdps squared overflows an int. */
static float magnitude(const int v[3])
{
    return sqrtf((float)v[0] * v[0] + (float)v[1] * v[1] + (float)v[2] * v[2]);
}

/* Angle between two vectors in degrees (dot product). 0 if either is zero. */
static float angle_deg(const int a[3], const int b[3])
{
    float mag_product = magnitude(a) * magnitude(b);
    if (mag_product == 0.0f)
    {
        return 0.0f;
    }

    /* clamp - rounding can give slightly more than 1 */
    float dot = (float)a[0] * b[0] + (float)a[1] * b[1] + (float)a[2] * b[2];
    float cos_angle = dot / mag_product;
    if (cos_angle > 1.0f)  cos_angle = 1.0f;
    if (cos_angle < -1.0f) cos_angle = -1.0f;
    return acosf(cos_angle) * RAD_TO_DEG;
}

/*--------------------------- Detection --------------------------------------*/

/* Filter memories, one per axis. Static because each ISR call needs the
 * previous output. The _c ones are for the C cross-check. */
static int acc_mg[3]       = {0, 0, 0};
static int gyro_mdps[3]    = {0, 0, 0};
static int acc_mg_c[3]     = {0, 0, 0};
static int gyro_mdps_c[3]  = {0, 0, 0};
static uint32_t loop_count = 0;         /* for the warm-up */

/* Latest values for the CSV and screens. Written in the ISR, so the main loop
 * reads them through detection_snapshot(). */
static volatile float    last_acc_g;
static volatile float    last_gyro_dps;
static volatile float    last_pose_deg;
static volatile uint32_t last_step_ms;
static volatile uint8_t  filters_disagree;

/* Copy with interrupts off so we don't get values from two different steps */
void detection_snapshot(detection_t *out)
{
    __disable_irq();
    out->acc_g = last_acc_g;
    out->gyro_dps = last_gyro_dps;
    out->pose_deg = last_pose_deg;
    out->t_ms = last_step_ms;
    out->state = incident.state;
    __enable_irq();
}

/* Called from the TIM7 interrupt every LOOP_MS. No printing or drawing here. */
void detection_step(void)
{
    uint32_t now = HAL_GetTick();

    /* accel in mg, gyro in mdps */
    int16_t acc_raw[3] = {0, 0, 0};
    float gyro_raw[3]  = {0.0f, 0.0f, 0.0f};
    BSP_ACCELERO_AccGetXYZ(acc_raw);
    BSP_GYRO_GetXYZ(gyro_raw);

    /* EWMA on all 6 axes (asm), plus the C version to compare against.
     * Gyro goes in as int mdps since the asm is integer only. */
    for (int axis = 0; axis < 3; axis++)
    {
        int gyro_in = (int)gyro_raw[axis];

        acc_mg[axis]      = ewma_filter(acc_raw[axis], acc_mg[axis], ALPHA_ACC);
        gyro_mdps[axis]   = ewma_filter(gyro_in, gyro_mdps[axis], ALPHA_GYRO);
        acc_mg_c[axis]    = ewma_filter_C(acc_raw[axis], acc_mg_c[axis], ALPHA_ACC);
        gyro_mdps_c[axis] = ewma_filter_C(gyro_in, gyro_mdps_c[axis], ALPHA_GYRO);
    }

    /* flag a mismatch, the main loop prints the warning */
    if ((memcmp(acc_mg, acc_mg_c, sizeof(acc_mg)) != 0) ||
        (memcmp(gyro_mdps, gyro_mdps_c, sizeof(gyro_mdps)) != 0))
    {
        filters_disagree = 1;
    }

    /* features, from filtered values only */
    float acc_g    = magnitude(acc_mg) / 1000.0f;
    float gyro_dps = magnitude(gyro_mdps) / 1000.0f;

    /* update the slow pose tracker (same asm filter) only while at rest in NORMAL */
    if ((incident.state == STATE_NORMAL) && (acc_g >= POSE_MIN_G) && (acc_g <= POSE_MAX_G))
    {
        for (int axis = 0; axis < 3; axis++)
        {
            pose_mg[axis] = ewma_filter(acc_mg[axis], pose_mg[axis], ALPHA_POSE);
        }
    }

    float pose_deg = angle_deg(acc_mg, pre_fall_ref);

    /* Warm-up, then a sanity check that the accel reads ~1 g at rest. If it
     * doesn't (failed init), re-init and warm up again. The re-init is slow
     * so a few overruns get counted while it happens. */
    if (loop_count < WARMUP_LOOPS)
    {
        loop_count++;
    }
    else if (loop_count == WARMUP_LOOPS)
    {
        if ((acc_g >= ACC_OK_MIN_G) && (acc_g <= ACC_OK_MAX_G))
        {
            detect_say(">> WARMUP DONE\r\n");
            loop_count++;
        }
        else
        {
            detect_say(">> BAD ACCEL, retrying\r\n");
            sensors_init();
            sensor_configure();
            loop_count = 0;
        }
    }
    else
    {
        update_state(acc_g, gyro_dps, pose_deg, now);
    }

    /* incident timers, button, buzzer, LED. "upright" = back in the pre-fall pose */
    incident_tick((uint8_t)(pose_ref_valid && (pose_deg < POSE_RETURN_DEG)),
                  (uint8_t)(gyro_dps < STILL_DPS));

    last_acc_g = acc_g;
    last_gyro_dps = gyro_dps;
    last_pose_deg = pose_deg;
    last_step_ms = now;
}

/*--------------------------- Main -------------------------------------------*/

int main(void)
{
    uint32_t health_loops = 0;

    HAL_Init();
    UART1_Init();

    BSP_LED_Init(LED2);
    sensors_init();
    sensor_configure();
    BSP_LED_Off(LED2);

    /* OLED is optional - if it isn't connected the oled_* calls do nothing */
    oled_init();
    screens_init();

    incident_init();

    /* Joining Wi-Fi takes a few seconds, so do it once here. If it fails we
     * just run offline. */
    screens_boot_note("Joining network...");
    net_init();
    screens_boot_note(net_status_text());

    /* CSV header (pose = angle from pre-fall pose, 0 until the first SUSPECT) */
    UART_Send("t_ms,state,acc_g,gyro_dps,pose\r\n");

    /* start detection in the timer interrupt */
    sampler_start();

    /* Main loop: screen, network and UART only. These can block, which is
     * fine because detection doesn't depend on this loop any more. */
    while (1)
    {
        uint32_t loop_start = HAL_GetTick();
        detection_t now;
        char line[72];

        screens_update();
        oled_service();     /* sends one changed row */

        net_tick();         /* one short network step */

        log_drain();        /* print lines queued by the ISR */

        detection_snapshot(&now);
        snprintf(line, sizeof(line), "%lu,%s,%.2f,%.2f,%.2f\r\n",
                 (unsigned long)now.t_ms, STATE_NAMES[now.state],
                 now.acc_g, now.gyro_dps, now.pose_deg);
        UART_Send(line);

        if (filters_disagree)
        {
            filters_disagree = 0;
            UART_Send("WARNING: Assembly and C EWMA outputs do not match.\r\n");
        }

        /* overruns = detection steps skipped because the last one hadn't finished */
        if (++health_loops >= HEALTH_EVERY_LOOPS)
        {
            health_loops = 0;
            snprintf(line, sizeof(line),
                     "## health steps=%lu overruns=%lu queued=%u link=%s\r\n",
                     (unsigned long)sampler_steps(), (unsigned long)sampler_overruns(),
                     (unsigned)net_queue_depth(), net_is_online() ? "up" : "down");
            UART_Send(line);
        }

        /* one CSV row per 20 ms */
        while ((HAL_GetTick() - loop_start) < LOOP_MS) { }
    }
}

int ewma_filter_C(int new_data, int old_output, int alpha_percent)
{
    /* Reference implementation for verification only. The assembly routine
     * must be used in the actual sensor-processing and detection pipeline. */
    int numerator = alpha_percent * new_data
                  + (100 - alpha_percent) * old_output;
    return numerator / 100;
}

/*--------------------------- UART -------------------------------------------*/

/* blocking send */
static void UART_Send(const char *text)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)text, strlen(text), HAL_MAX_DELAY);
}

/* USART1 on PB6/PB7 (ST-LINK VCP), 115200 8N1 */
static void UART1_Init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    GPIO_InitStruct.Pin = GPIO_PIN_7 | GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

    if (HAL_UART_Init(&huart1) != HAL_OK)
    {
        while (1) { }
    }
}

/* Do not modify these lines. They suppress UART-related warnings. */
int _write(int file, char *ptr, int len)
{
    (void)file;
    (void)ptr;
    return len;
}
int _read(int file, char *ptr, int len) { (void)file; (void)ptr; (void)len; return 0; }
int _fstat(int file, struct stat *st) { (void)file; (void)st; return 0; }
int _lseek(int file, int ptr, int dir) { (void)file; (void)ptr; (void)dir; return 0; }
int _isatty(int file) { (void)file; return 1; }
int _close(int file) { (void)file; return -1; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; return -1; }
