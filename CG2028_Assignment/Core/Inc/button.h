/******************************************************************************
 * @file           : button.h
 * @brief          : Blue user button - short press and 3 s hold.
 *
 * Debounced. A hold only fires once and the button has to be released before
 * the next action, so one long press can't raise and then cancel an SOS.
 ******************************************************************************/
#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>

#define BUTTON_HOLD_MS      3000U   /* also used by screens.c for the hold bar */

typedef enum
{
    BUTTON_NONE = 0,
    BUTTON_PRESS,       /* released before BUTTON_HOLD_MS */
    BUTTON_HOLD         /* held for BUTTON_HOLD_MS */
} button_event_t;

void button_init(void);

/* Call every loop */
button_event_t button_poll(uint32_t now_ms);

/* ms the button has been held, 0 if released */
uint32_t button_held_ms(uint32_t now_ms);

uint8_t button_is_down(void);

#endif /* BUTTON_H */
