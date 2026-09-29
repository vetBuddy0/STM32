/******************************************************************************
 * @file           : screens.c
 * @brief          : OLED screens for each incident state.
 *
 * Layout used by every screen:
 *     row 0      status bar (DEMO/OPER, marker, incident number)
 *     rows 1-2   big headline
 *     rows 3-6   details / what to do
 *     row 7      link or message status
 *
 * Only redraws when something visible changed.
 ******************************************************************************/

/*--------------------------- Includes ---------------------------------------*/
#include "main.h"
#include "screens.h"
#include "incident.h"
#include "button.h"
#include "net.h"
#include "oled.h"

#include <stdio.h>

/*--------------------------- Configuration ----------------------------------*/
#define BEAT_MS         1000U   /* heartbeat marker blink */
#define BOOT_MS         1500U   /* min time on the boot screen */

/*--------------------------- State ------------------------------------------*/

/* what was drawn last time */
static incident_status_t last_status;
static incident_type_t   last_type;
static delivery_t        last_delivery;
static uint32_t last_id;
static uint32_t last_seconds;   /* countdown or incident age */
static uint8_t  last_beat;
static uint8_t  last_hold;      /* SOS hold in 0.1 s */
static uint8_t  last_link;      /* bit 7 = online, rest = queue depth */
static uint8_t  drawn_once;

static uint32_t boot_started_ms;

/*--------------------------- Helpers ----------------------------------------*/

/* "45s" or "5m 00s" */
static void format_age(char *out, int size, uint32_t ms)
{
    uint32_t seconds = ms / 1000U;

    if (seconds < 60U)
    {
        snprintf(out, size, "%lus", (unsigned long)seconds);
    }
    else
    {
        snprintf(out, size, "%lum %02lus",
                 (unsigned long)(seconds / 60U), (unsigned long)(seconds % 60U));
    }
}

/* row 7 text for the caregiver message */
static const char *delivery_text(delivery_t state)
{
    switch (state)
    {
    case MSG_PENDING:   return net_is_online() ? "Sending..." : "Waiting: no link";
    case MSG_DELIVERED: return "Delivered";
    case MSG_ACKED:     return "Caregiver saw it";
    default:            return "";
    }
}

/* link status + number of queued messages */
static void draw_link_line(uint8_t row)
{
    char line[OLED_COLS + 1];
    uint8_t waiting = net_queue_depth();

    if (net_is_online())
    {
        if (waiting > 0U)
        {
            snprintf(line, sizeof(line), "Link ok, %u waiting", (unsigned)waiting);
        }
        else
        {
            snprintf(line, sizeof(line), "Link ok");
        }
    }
    else if (waiting > 0U)
    {
        snprintf(line, sizeof(line), "No link, %u waiting", (unsigned)waiting);
    }
    else
    {
        snprintf(line, sizeof(line), "No link");
    }

    oled_text(row, 0, line);
}

static void draw_status_bar(uint8_t beat)
{
    char line[OLED_COLS + 1];
    const char *mark;

    switch (incident_status())
    {
    case INC_ESCALATED:
        mark = "!!";
        break;
    case INC_RESPONSE:
    case INC_ASSIST:
        mark = "!";
        break;
    default:
        mark = beat ? "*" : " ";
        break;
    }

    snprintf(line, sizeof(line), "%-6s %-2s   Inc:%lu",
             incident_demo_timers() ? "DEMO" : "OPER",
             mark, (unsigned long)incident_id());
    oled_text(0, 0, line);
}

/*--------------------------- The screens ------------------------------------*/

static void draw_boot(void)
{
    oled_big(1, "STARTING");
    oled_text(3, 0, "Sensor OK");
    oled_text(4, 0, incident_demo_timers() ? "Timers: DEMO" : "Timers: normal");
    oled_text(5, 0, "Keep still");
    oled_text(6, 0, "while warming up");
    oled_text(7, 0, "");
}

static void draw_monitoring(void)
{
    oled_big(1, "MONITORING");
    oled_text(3, 0, incident_wearer_still() ? "Upright, still"
                                            : "Upright, moving");
    oled_text(4, 0, "");
    oled_text(5, 0, "Hold button 3s");
    oled_text(6, 0, "to call for help");
    draw_link_line(7);
}

/* progress bar while the button is held for SOS */
static void draw_sos_hold(uint32_t held_ms)
{
    char label[12];
    uint32_t percent;

    /* cap at the hold time so the label doesn't overflow the row */
    if (held_ms > BUTTON_HOLD_MS)
    {
        held_ms = BUTTON_HOLD_MS;
    }
    percent = (held_ms * 100U) / BUTTON_HOLD_MS;

    snprintf(label, sizeof(label), "%lu.%lus",
             (unsigned long)(held_ms / 1000U),
             (unsigned long)((held_ms % 1000U) / 100U));

    oled_big(1, "SOS");
    oled_text(3, 0, "Keep holding");
    oled_bar(4, (uint8_t)percent, label);
    oled_text(5, 0, "Let go to");
    oled_text(6, 0, "cancel");
    draw_link_line(7);
}

static void draw_response(uint32_t left_ms)
{
    char label[12];
    uint32_t total = left_ms + incident_phase_ms();
    uint32_t percent = (total > 0U) ? ((left_ms * 100U) / total) : 0U;

    snprintf(label, sizeof(label), "%lus", (unsigned long)((left_ms + 999U) / 1000U));

    oled_big(1, "FALL!");
    oled_text(3, 0, "Fall detected");
    oled_bar(4, (uint8_t)percent, label);
    oled_text(5, 0, "Press button");
    oled_text(6, 0, "if you are OK");
    draw_link_line(7);
}

static void draw_assist(void)
{
    char age[12];
    char line[OLED_COLS + 1];
    uint8_t is_sos = (incident_type() == INC_SOS);

    format_age(age, sizeof(age), incident_age_ms());
    snprintf(line, sizeof(line), "%s %s", is_sos ? "Waiting" : "Down for", age);

    oled_big(1, is_sos ? "SOS SENT" : "HELP SENT");
    oled_text(3, 0, is_sos ? "You asked for help" : "Help requested");
    oled_text(4, 0, line);
    oled_text(5, 0, "Hold button 3s");
    oled_text(6, 0, "to cancel");
    oled_text(7, 0, delivery_text(incident_delivery_state()));
}

static void draw_escalated(void)
{
    char age[12];
    char line[OLED_COLS + 1];

    format_age(age, sizeof(age), incident_age_ms());
    snprintf(line, sizeof(line), "Down for %s", age);

    oled_big(1, "STILL DOWN");
    oled_text(3, 0, "No recovery yet");
    oled_text(4, 0, line);
    oled_text(5, 0, "Stay still");
    oled_text(6, 0, "Help is on the way");
    oled_text(7, 0, delivery_text(incident_delivery_state()));
}

static void draw_cancelled(void)
{
    char line[OLED_COLS + 1];

    snprintf(line, sizeof(line), "incident %lu", (unsigned long)incident_id());

    oled_big(1, "CANCELLED");
    oled_text(3, 0, "You cancelled");
    oled_text(4, 0, line);
    oled_text(5, 0, "");
    oled_text(6, 0, "");
    oled_text(7, 0, delivery_text(incident_delivery_state()));
}

static void draw_resolved(void)
{
    char line[OLED_COLS + 1];

    snprintf(line, sizeof(line), "incident %lu closed", (unsigned long)incident_id());

    oled_big(1, "RECOVERED");
    oled_text(3, 0, "Upright again");
    oled_text(4, 0, line);
    oled_text(5, 0, "Back to monitoring");
    oled_text(6, 0, "");
    oled_text(7, 0, delivery_text(incident_delivery_state()));
}

/*--------------------------- Choosing ---------------------------------------*/

void screens_init(void)
{
    boot_started_ms = HAL_GetTick();
    drawn_once = 0;
    last_beat = 0xFFU;      /* force a redraw */

    oled_clear();
    draw_boot();
    draw_status_bar(1);
    oled_flush();
}

void screens_boot_note(const char *text)
{
    oled_text(6, 0, text);
    oled_flush();

    /* restart the boot timer so the note stays readable */
    boot_started_ms = HAL_GetTick();
}

void screens_update(void)
{
    uint32_t now = HAL_GetTick();
    uint32_t held_ms;
    uint32_t seconds;
    uint8_t beat;
    uint8_t hold_tenths;
    uint8_t link;
    incident_status_t status = incident_status();

    if ((now - boot_started_ms) < BOOT_MS)
    {
        return;
    }

    /* whole seconds only, so the countdown redraws once a second */
    if (status == INC_RESPONSE)
    {
        seconds = (incident_response_left_ms() + 999U) / 1000U;
    }
    else if ((status == INC_ASSIST) || (status == INC_ESCALATED))
    {
        seconds = incident_age_ms() / 1000U;
    }
    else
    {
        seconds = 0U;
    }

    link = (uint8_t)((net_is_online() ? 0x80U : 0x00U) | (net_queue_depth() & 0x7FU));
    held_ms = button_held_ms(now);
    hold_tenths = (uint8_t)((held_ms / 100U) & 0xFFU);
    beat = (uint8_t)(((now / BEAT_MS) & 1U) == 0U);

    /* nothing changed */
    if (drawn_once &&
        (status == last_status) &&
        (incident_type() == last_type) &&
        (incident_delivery_state() == last_delivery) &&
        (incident_id() == last_id) &&
        (seconds == last_seconds) &&
        (beat == last_beat) &&
        (hold_tenths == last_hold) &&
        (link == last_link))
    {
        return;
    }

    switch (status)
    {
    case INC_RESPONSE:  draw_response(incident_response_left_ms()); break;
    case INC_ASSIST:    draw_assist();                              break;
    case INC_ESCALATED: draw_escalated();                           break;
    case INC_CANCELLED: draw_cancelled();                           break;
    case INC_RESOLVED:  draw_resolved();                            break;

    case INC_IDLE:
    default:
        /* idle, or SOS hold in progress */
        if (held_ms > 0U)
        {
            draw_sos_hold(held_ms);
        }
        else
        {
            draw_monitoring();
        }
        break;
    }

    draw_status_bar(beat);

    last_status = status;
    last_type = incident_type();
    last_delivery = incident_delivery_state();
    last_id = incident_id();
    last_seconds = seconds;
    last_beat = beat;
    last_hold = hold_tenths;
    last_link = link;
    drawn_once = 1;
}
