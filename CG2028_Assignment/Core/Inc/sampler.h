/******************************************************************************
 * @file           : sampler.h
 * @brief          : TIM7 interrupt that runs detection every 20 ms.
 *
 * Detection used to be in the main loop, but the Wi-Fi calls can block for
 * seconds. Running it from a timer interrupt means sampling, the state
 * machine, the incident timers and the buzzer/LED keep going regardless.
 ******************************************************************************/
#ifndef SAMPLER_H
#define SAMPLER_H

#include <stdint.h>

#include "incident.h"

/* main.c uses this as LOOP_MS */
#define SAMPLER_PERIOD_MS   20U

/* In main.c. Runs inside the ISR, so no printing or OLED access. */
void detection_step(void);

/* Latest values for the CSV output and the screens */
typedef struct
{
    float        acc_g;
    float        gyro_dps;
    float        pose_deg;  /* angle from the pre-fall pose */
    uint32_t     t_ms;
    fall_state_t state;
} detection_t;

/* Copies everything with interrupts off so the values belong to the same step */
void detection_snapshot(detection_t *out);

void sampler_start(void);

/* overruns = steps skipped because the previous one was still running */
uint32_t sampler_steps(void);
uint32_t sampler_overruns(void);

#endif /* SAMPLER_H */
