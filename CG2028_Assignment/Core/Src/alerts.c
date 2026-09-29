/******************************************************************************
 * @file           : alerts.c
 * @brief          : Buzzer patterns and LED2.
 *
 * Buzzer: Grove active buzzer on A1 (PC4), plain GPIO high = sound.
 * (BUZZER_SELF_TONE 0 = old passive-piezo option on D6/PB1 with TIM3 PWM.)
 *
 * Patterns are short beeps with gaps, never a continuous tone - a buzzing
 * case shakes the gyro and the board would never look "still".
 ******************************************************************************/

/*--------------------------- Includes ---------------------------------------*/
#include "main.h"
#include "alerts.h"
#include "../../Drivers/BSP/B-L4S5I-IOT01/stm32l4s5i_iot01.h"

/*--------------------------- Configuration ----------------------------------*/

/* 0 = mute (useful while tuning the detector) */
#define BUZZER_ENABLED      1

/* 1 for an active buzzer that makes its own tone (plain GPIO on/off).
 * The Grove buzzer is active, so no timer is needed. */
#define BUZZER_SELF_TONE    1

#if BUZZER_SELF_TONE
#define BUZZER_PIN          ARD_A1_Pin          /* PC4, Grove A1 */
#define BUZZER_PORT         ARD_A1_GPIO_Port
#else
#define BUZZER_PIN          ARD_D6_Pin          /* PB1, only pin here with TIM3 */
#define BUZZER_PORT         ARD_D6_GPIO_Port
#endif
#define BUZZER_AF           GPIO_AF2_TIM3
#define BUZZER_TIM          TIM3
#define BUZZER_CHANNEL      TIM_CHANNEL_4

/* LED2 toggle interval. Checked every 20 ms so 150 ends up as 160 in practice */
#define LED_SLOW_MS      1000U
#define LED_FAST_MS       150U      /* incident active */

/*--------------------------- Patterns ---------------------------------------*/

/* on time, then gap after it */
typedef struct
{
    uint16_t on_ms;
    uint16_t off_ms;
} beep_t;

/* repeat_ms = pause after the last beep before starting again, 0 = play once */
typedef struct
{
    const beep_t *beeps;
    uint8_t       count;
    uint16_t      hz;
    uint16_t      repeat_ms;
} pattern_t;

/* response window: 2 beeps, repeats every ~1.4 s */
static const beep_t fall_beeps[] = { {120, 120}, {120, 0} };

/* help requested: 1 beep every ~3 s */
static const beep_t waiting_beeps[] = { {100, 0} };

/* escalated: 3 beeps at a lower pitch every ~5.5 s */
static const beep_t escalated_beeps[] = { {100, 100}, {100, 100}, {100, 0} };

/* short tick while the SOS button is being held */
static const beep_t hold_beeps[] = { {20, 0} };

/* one-off sounds */
static const beep_t cancelled_beeps[] = { {250, 0} };
static const beep_t recovered_beeps[] = { {80, 120}, {80, 0} };

static const pattern_t patterns[] = {
    /* ALERT_QUIET     */ { NULL,             0, 0,    0 },
    /* ALERT_FALL      */ { fall_beeps,       2, 2700, 1000 },
    /* ALERT_WAITING   */ { waiting_beeps,    1, 2700, 3000 },
    /* ALERT_ESCALATED */ { escalated_beeps,  3, 2100, 5000 },
    /* ALERT_SOS_HOLD  */ { hold_beeps,       1, 3200, 250 },
    /* ALERT_CANCELLED */ { cancelled_beeps,  1, 1800, 0 },
    /* ALERT_RECOVERED */ { recovered_beeps,  2, 3200, 0 }
};

/*--------------------------- State ------------------------------------------*/

#if !BUZZER_SELF_TONE
static TIM_HandleTypeDef htim_buzzer;
#endif
static uint8_t timer_ready;

static alert_mode_t mode;
static uint8_t  step;
static uint8_t  sounding;
static uint8_t  finished;       /* one-shot pattern done */
static uint32_t step_due_ms;

static uint32_t led_last_ms;
static uint8_t  led_on;

/*--------------------------- The buzzer -------------------------------------*/

/* set ARR/CCR for the requested pitch and start PWM */
static void buzzer_on(uint16_t hz)
{
#if BUZZER_ENABLED
#if BUZZER_SELF_TONE
    (void)hz;
    HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_SET);
#else
    uint32_t timer_clock;
    uint32_t period;

    if (!timer_ready || (hz == 0U))
    {
        return;
    }

    /* APB1 timer clock is doubled if the APB1 prescaler isn't 1 */
    timer_clock = HAL_RCC_GetPCLK1Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0U)
    {
        timer_clock *= 2U;
    }

    period = timer_clock / hz;
    if (period < 2U)
    {
        period = 2U;
    }

    __HAL_TIM_SET_AUTORELOAD(&htim_buzzer, period - 1U);
    __HAL_TIM_SET_COMPARE(&htim_buzzer, BUZZER_CHANNEL, period / 2U);
#endif
#else
    (void)hz;
#endif
}

static void buzzer_off(void)
{
#if BUZZER_ENABLED
#if BUZZER_SELF_TONE
    HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
#else
    /* CCR = 0 keeps PB1 driven low. Stopping the channel instead would
     * leave the pin floating, and a floating Grove buzzer input keeps
     * sounding between beeps. */
    if (timer_ready)
    {
        __HAL_TIM_SET_COMPARE(&htim_buzzer, BUZZER_CHANNEL, 0U);
    }
#endif
#endif
}

/* buzzer pin: plain output, or PB1 as TIM3_CH4 with PWM */
static void buzzer_init(void)
{
    GPIO_InitTypeDef gpio = {0};

#if BUZZER_SELF_TONE
    __HAL_RCC_GPIOC_CLK_ENABLE();

    gpio.Pin = BUZZER_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BUZZER_PORT, &gpio);
    HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
#else
    TIM_OC_InitTypeDef channel = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();

    /* PB1 as alternate function (TIM3) */
    gpio.Pin = BUZZER_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLDOWN;      /* held low even before the timer drives it */
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = BUZZER_AF;
    HAL_GPIO_Init(BUZZER_PORT, &gpio);

    htim_buzzer.Instance = BUZZER_TIM;
    htim_buzzer.Init.Prescaler = 0;             /* count the clock directly */
    htim_buzzer.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim_buzzer.Init.Period = 1000;             /* replaced per note */
    htim_buzzer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim_buzzer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_PWM_Init(&htim_buzzer) != HAL_OK)
    {
        return;         /* no buzzer, everything else still works */
    }

    channel.OCMode = TIM_OCMODE_PWM1;
    channel.Pulse = 0;              /* silent until buzzer_on() */
    channel.OCPolarity = TIM_OCPOLARITY_HIGH;
    channel.OCFastMode = TIM_OCFAST_DISABLE;

    if (HAL_TIM_PWM_ConfigChannel(&htim_buzzer, &channel, BUZZER_CHANNEL) != HAL_OK)
    {
        return;
    }

    /* channel stays running from here on; on/off only changes CCR */
    if (HAL_TIM_PWM_Start(&htim_buzzer, BUZZER_CHANNEL) != HAL_OK)
    {
        return;
    }

    timer_ready = 1;
#endif
}

/*--------------------------- Patterns ---------------------------------------*/

void alerts_set(alert_mode_t next)
{
    if (next == mode)
    {
        return;
    }

    mode = next;
    step = 0;
    finished = 0;
    sounding = 0;
    buzzer_off();
    step_due_ms = HAL_GetTick();    /* start now */
}

void alerts_tick(void)
{
    uint32_t now = HAL_GetTick();
    const pattern_t *pattern = &patterns[mode];

    /* LED2: slow when idle, fast otherwise */
    {
        uint32_t period = (mode == ALERT_QUIET) ? LED_SLOW_MS : LED_FAST_MS;

        if ((now - led_last_ms) >= period)
        {
            led_last_ms = now;
            led_on = !led_on;

            if (led_on)
            {
                BSP_LED_On(LED2);
            }
            else
            {
                BSP_LED_Off(LED2);
            }
        }
    }

    /* buzzer pattern */
    if ((pattern->beeps == NULL) || finished)
    {
        return;
    }

    if ((int32_t)(now - step_due_ms) < 0)
    {
        return;             /* current beep/gap not over yet */
    }

    if (sounding)
    {
        /* beep finished -> gap */
        buzzer_off();
        sounding = 0;

        if (pattern->beeps[step].off_ms > 0U)
        {
            step_due_ms = now + pattern->beeps[step].off_ms;
            step++;
            return;
        }

        /* last beep: wait repeat_ms, or stop if one-shot */
        step++;
        if (step >= pattern->count)
        {
            if (pattern->repeat_ms == 0U)
            {
                finished = 1;
                return;
            }
            step = 0;
            step_due_ms = now + pattern->repeat_ms;
        }
        return;
    }

    /* next beep */
    if (step < pattern->count)
    {
        buzzer_on(pattern->hz);
        sounding = 1;
        step_due_ms = now + pattern->beeps[step].on_ms;
    }
}

/*--------------------------- Start-up ---------------------------------------*/

void alerts_init(void)
{
    mode = ALERT_QUIET;
    step = 0;
    sounding = 0;
    finished = 0;
    step_due_ms = 0;
    led_last_ms = 0;
    led_on = 0;
    timer_ready = 0;

    BSP_LED_Init(LED2);
    BSP_LED_Off(LED2);
    buzzer_init();
}
