/******************************************************************************
 * @file           : incident.c
 * @brief          : Incident handling after a confirmed fall, plus manual SOS.
 *
 * Fall confirmed -> RESPONSE (alarm, wearer can press "I'm OK")
 *               -> ASSIST (message to caregiver) -> ESCALATED (long lie)
 *               -> RESOLVED once the wearer is back up for RECOVERY_MS,
 *                  or CANCELLED by the button.
 * SOS goes straight to ASSIST and can only be closed by the wearer.
 * A caregiver ACK is recorded but never closes anything.
 ******************************************************************************/

/*--------------------------- Includes ---------------------------------------*/
#include "main.h"
#include "incident.h"
#include "alerts.h"
#include "button.h"
#include "sampler.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

/*--------------------------- Configuration ----------------------------------*/

/* 1 = short timers for the demo, 0 = normal timers. The status bar shows
 * which set is in use. */
#define DEMO_TIMERS         1

#if DEMO_TIMERS
#define RESPONSE_MS     10000U      /* time to press "I'm OK" */
#define ESCALATE_MS     60000U      /* long lie, counted from the start of the incident */
#define RECOVERY_MS     10000U      /* upright this long in a row = recovered */
#else
#define RESPONSE_MS     30000U
#define ESCALATE_MS    300000U
#define RECOVERY_MS     60000U
#endif

#define CLOSING_MS       5000U      /* how long cancelled/resolved stays on screen */

/*--------------------------- State ------------------------------------------*/

/* Only one incident at a time */
static uint32_t          inc_id;            /* 0 before the first incident */
static incident_type_t   inc_type;
static incident_status_t inc_status;
static delivery_t        inc_delivery;

static uint32_t inc_started_ms;
static uint32_t phase_started_ms;   /* last status change */
static uint32_t upright_since_ms;   /* 0 = not upright */
static uint8_t  down_seen;          /* wearer was down at some point */
static uint8_t  upright_now;        /* from incident_tick() */
static uint8_t  still_now;

/*--------------------------- Helpers ----------------------------------------*/

/* runs in the ISR, so queue the line instead of printing */
static void incident_say(const char *text)
{
    log_line(text);
}

/* net.c overrides this */
__attribute__((weak)) void on_incident_update(void) { }

static const char *status_name(incident_status_t status)
{
    switch (status)
    {
    case INC_RESPONSE:  return "response window";
    case INC_ASSIST:    return "assistance requested";
    case INC_ESCALATED: return "escalated, long lie";
    case INC_CANCELLED: return "cancelled by wearer";
    case INC_RESOLVED:  return "resolved, recovered";
    default:            return "idle";
    }
}

/* change status, log it and queue a message for the caregiver */
static void set_status(incident_status_t next, uint32_t now_ms)
{
    char line[64];

    inc_status = next;
    phase_started_ms = now_ms;

    snprintf(line, sizeof(line), "## incident %lu %s: %s\r\n",
             (unsigned long)inc_id,
             (inc_type == INC_SOS) ? "SOS" : "fall",
             status_name(next));
    incident_say(line);

    on_incident_update();
}

/* Ignored if an incident is already open - a second fall is the same emergency */
static void raise_incident(incident_type_t type, uint32_t now_ms)
{
    if (incident_is_active())
    {
        return;
    }

    inc_id++;
    inc_type = type;
    inc_delivery = MSG_NONE;
    inc_started_ms = now_ms;
    upright_since_ms = 0;
    down_seen = 0;

    /* SOS was deliberate, so it skips the response window */
    if (type == INC_FALL)
    {
        set_status(INC_RESPONSE, now_ms);
    }
    else
    {
        inc_delivery = MSG_PENDING;
        set_status(INC_ASSIST, now_ms);
    }
}

/*--------------------------- Events from the wearer -------------------------*/

void incident_request_sos(void)
{
    raise_incident(INC_SOS, HAL_GetTick());
}

void incident_wearer_ok(void)
{
    uint32_t now = HAL_GetTick();

    if (inc_status == INC_RESPONSE)
    {
        /* also clear the detector's FALL latch */
        fall_cancel_request = 1;
        set_status(INC_CANCELLED, now);
    }
}

void incident_cancel(void)
{
    uint32_t now = HAL_GetTick();

    if (incident_is_active())
    {
        fall_cancel_request = 1;
        inc_delivery = MSG_PENDING;     /* caregiver needs to know it was cancelled */
        set_status(INC_CANCELLED, now);
    }
}

/*--------------------------- Events from the link ---------------------------*/

void incident_delivery(uint32_t id, delivery_t state)
{
    /* ignore replies about an older incident */
    if ((id != inc_id) || !incident_is_active())
    {
        return;
    }

    inc_delivery = state;

    /* ACK doesn't change the status or stop escalation on purpose */
}

/*--------------------------- Events from the detector -----------------------*/

void on_fall_confirmed(const incident_t *incident)
{
    (void)incident;
    raise_incident(INC_FALL, HAL_GetTick());
}

void on_recovered(const incident_t *incident)
{
    (void)incident;

    /* Empty on purpose - the detector leaving FALL isn't proof of recovery,
     * recovery_seen() handles that. */
}

/*--------------------------- The tick ---------------------------------------*/

/* buzzer + LED mode for the current status */
static alert_mode_t alert_for_status(void)
{
    switch (inc_status)
    {
    case INC_RESPONSE:  return ALERT_FALL;
    case INC_ASSIST:    return ALERT_WAITING;
    case INC_ESCALATED: return ALERT_ESCALATED;
    case INC_CANCELLED: return ALERT_CANCELLED;
    case INC_RESOLVED:  return ALERT_RECOVERED;

    case INC_IDLE:
    default:
        return button_is_down() ? ALERT_SOS_HOLD : ALERT_QUIET;
    }
}

/* Recovered = was down at some point, then upright for RECOVERY_MS in a row.
 * Any sample that isn't upright restarts the timer. */
static uint8_t recovery_seen(uint32_t now_ms)
{
    if (!upright_now)
    {
        down_seen = 1;
        upright_since_ms = 0;
        return 0;
    }

    if (!down_seen)
    {
        return 0;                   /* never went down */
    }

    if (upright_since_ms == 0U)
    {
        upright_since_ms = now_ms;
    }

    return ((now_ms - upright_since_ms) >= RECOVERY_MS) ? 1U : 0U;
}

/* press: "I'm OK" during the response window
 * hold:  cancel if an incident is open, otherwise SOS */
static void handle_button(button_event_t event)
{
    switch (event)
    {
    case BUTTON_PRESS:
        if (inc_status == INC_RESPONSE)
        {
            incident_wearer_ok();
        }
        break;

    case BUTTON_HOLD:
        if (incident_is_active())
        {
            incident_cancel();
        }
        else
        {
            incident_request_sos();
        }
        break;

    default:
        break;
    }
}

void incident_tick(uint8_t upright, uint8_t still)
{
    uint32_t now = HAL_GetTick();

    upright_now = upright;
    still_now = still;

    handle_button(button_poll(now));

    switch (inc_status)
    {
    case INC_RESPONSE:
        /* no answer from the wearer -> ask for help */
        if ((now - phase_started_ms) >= RESPONSE_MS)
        {
            inc_delivery = MSG_PENDING;
            set_status(INC_ASSIST, now);
        }
        break;

    case INC_ASSIST:
        /* falls can resolve by recovery, SOS only by cancel */
        if ((inc_type == INC_FALL) && recovery_seen(now))
        {
            inc_delivery = MSG_PENDING;
            set_status(INC_RESOLVED, now);
        }
        else if ((now - inc_started_ms) >= ESCALATE_MS)
        {
            inc_delivery = MSG_PENDING;
            set_status(INC_ESCALATED, now);
        }
        break;

    case INC_ESCALATED:
        /* can still recover after escalating */
        if ((inc_type == INC_FALL) && recovery_seen(now))
        {
            inc_delivery = MSG_PENDING;
            set_status(INC_RESOLVED, now);
        }
        break;

    case INC_CANCELLED:
    case INC_RESOLVED:
        /* show the result for a few seconds then go back to idle */
        if ((now - phase_started_ms) >= CLOSING_MS)
        {
            inc_type = INC_NONE;
            inc_delivery = MSG_NONE;
            inc_status = INC_IDLE;
            phase_started_ms = now;
        }
        break;

    case INC_IDLE:
    default:
        break;
    }

    /* buzzer and LED here; the screen is drawn from the main loop since
     * I2C is too slow for the ISR */
    alerts_set(alert_for_status());
    alerts_tick();
}

/*--------------------------- Read-only access -------------------------------*/

uint32_t incident_id(void)                  { return inc_id; }
incident_type_t incident_type(void)         { return inc_type; }
incident_status_t incident_status(void)     { return inc_status; }
delivery_t incident_delivery_state(void)    { return inc_delivery; }
uint8_t incident_wearer_still(void)         { return still_now; }
uint8_t incident_demo_timers(void)          { return (uint8_t)DEMO_TIMERS; }

uint8_t incident_is_active(void)
{
    return ((inc_status == INC_RESPONSE) ||
            (inc_status == INC_ASSIST) ||
            (inc_status == INC_ESCALATED)) ? 1U : 0U;
}

uint32_t incident_age_ms(void)
{
    return (inc_status == INC_IDLE) ? 0U : (HAL_GetTick() - inc_started_ms);
}

uint32_t incident_phase_ms(void)
{
    return HAL_GetTick() - phase_started_ms;
}

uint32_t incident_response_left_ms(void)
{
    uint32_t spent;

    if (inc_status != INC_RESPONSE)
    {
        return 0U;
    }

    spent = HAL_GetTick() - phase_started_ms;
    return (spent < RESPONSE_MS) ? (RESPONSE_MS - spent) : 0U;
}

/*--------------------------- Start-up ---------------------------------------*/

void incident_init(void)
{
    inc_id = 0;
    inc_type = INC_NONE;
    inc_status = INC_IDLE;
    inc_delivery = MSG_NONE;
    inc_started_ms = 0;
    phase_started_ms = HAL_GetTick();
    upright_since_ms = 0;
    down_seen = 0;
    upright_now = 1;
    still_now = 1;

    button_init();
    alerts_init();

    incident_say(DEMO_TIMERS ? "Incident timers: DEMO (shortened)\r\n"
                             : "Incident timers: operational\r\n");
    incident_say("Button: press = I am OK, hold 3s = SOS or cancel\r\n");
}
