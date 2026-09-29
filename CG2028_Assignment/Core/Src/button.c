/******************************************************************************
 * @file           : button.c
 * @brief          : Blue user button, polled every 20 ms from incident_tick().
 *
 * No interrupt needed - 50 reads a second is plenty for a button.
 ******************************************************************************/

/*--------------------------- Includes ---------------------------------------*/
#include "main.h"
#include "button.h"
#include "../../Drivers/BSP/B-L4S5I-IOT01/stm32l4s5i_iot01.h"

/*--------------------------- Configuration ----------------------------------*/
#define BUTTON_DEBOUNCE_MS    30U    /* level must be stable this long */

/*--------------------------- State ------------------------------------------*/

/* Pin level when not pressed - read at start-up rather than assuming the polarity */
static uint32_t released_level;

static uint8_t raw_down;
static uint8_t stable_down;     /* debounced */
static uint32_t raw_since_ms;
static uint32_t down_since_ms;
static uint8_t hold_reported;   /* hold already sent for this press */

/*--------------------------- Implementation ---------------------------------*/

void button_init(void)
{
    uint8_t high = 0;
    uint8_t sample;

    BSP_PB_Init(BUTTON_USER, BUTTON_MODE_GPIO);

    /* let the pull-up settle, then majority of 5 reads = released level */
    HAL_Delay(10);
    for (sample = 0; sample < 5U; sample++)
    {
        if (BSP_PB_GetState(BUTTON_USER) != 0U)
        {
            high++;
        }
        HAL_Delay(2);
    }
    released_level = (high >= 3U) ? 1U : 0U;

    raw_down = 0;
    stable_down = 0;
    raw_since_ms = 0;
    down_since_ms = 0;
    hold_reported = 0;
}

button_event_t button_poll(uint32_t now_ms)
{
    uint8_t level_down = (BSP_PB_GetState(BUTTON_USER) != released_level) ? 1U : 0U;

    /* debounce */
    if (level_down != raw_down)
    {
        raw_down = level_down;
        raw_since_ms = now_ms;
    }
    else if ((raw_down != stable_down) &&
             ((now_ms - raw_since_ms) >= BUTTON_DEBOUNCE_MS))
    {
        stable_down = raw_down;

        if (stable_down)
        {
            down_since_ms = now_ms;
            hold_reported = 0;
        }
        else if (!hold_reported)
        {
            /* released before the hold time. Releasing after a hold gives
             * nothing, so one press = one action. */
            return BUTTON_PRESS;
        }
    }

    if (stable_down && !hold_reported &&
        ((now_ms - down_since_ms) >= BUTTON_HOLD_MS))
    {
        hold_reported = 1;
        return BUTTON_HOLD;
    }

    return BUTTON_NONE;
}

uint32_t button_held_ms(uint32_t now_ms)
{
    return stable_down ? (now_ms - down_since_ms) : 0U;
}

uint8_t button_is_down(void)
{
    return stable_down;
}
