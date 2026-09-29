/******************************************************************************
 * @file           : alerts.h
 * @brief          : Buzzer patterns and LED2 blink rate.
 *
 * Buzzer is on PB1 (Grove D6), PWM from TIM3 CH4. Nothing here blocks, the
 * pattern moves one step per call to alerts_tick().
 ******************************************************************************/
#ifndef ALERTS_H
#define ALERTS_H

#include <stdint.h>

/* One mode per incident status, plus quiet and the SOS-hold tick */
typedef enum
{
    ALERT_QUIET = 0,    /* slow blink, silent */
    ALERT_FALL,         /* response window */
    ALERT_WAITING,      /* help requested */
    ALERT_ESCALATED,
    ALERT_SOS_HOLD,     /* button being held */
    ALERT_CANCELLED,
    ALERT_RECOVERED
} alert_mode_t;

void alerts_init(void);

/* Safe to call every loop, only restarts the pattern if the mode changes */
void alerts_set(alert_mode_t mode);

/* Call every loop */
void alerts_tick(void);

#endif /* ALERTS_H */
