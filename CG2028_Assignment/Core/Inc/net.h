/******************************************************************************
 * @file           : net.h
 * @brief          : Caregiver link - sends incident updates to the laptop
 *                   receiver (tools/receiver.py) over Wi-Fi.
 *
 * Joining is slow, so it only happens in net_init(). After that net_tick()
 * does one short step per call and returns.
 ******************************************************************************/
#ifndef NET_H
#define NET_H

#include <stdint.h>

/* If this fills up the oldest update is dropped - every message has the full
 * current state, so the newest one is what matters. */
#define NET_QUEUE_LEN   8

/* Slow (several seconds). Called once at start-up; runs offline if it fails. */
void net_init(void);

/* Main loop, once per pass */
void net_tick(void);

uint8_t net_is_online(void);
uint8_t net_queue_depth(void);

/* Short status for the boot screen, e.g. "Join refused" */
const char *net_status_text(void);

#endif /* NET_H */
