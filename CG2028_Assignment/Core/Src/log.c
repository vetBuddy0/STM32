/******************************************************************************
 * @file           : log.c
 * @brief          : Ring buffer of UART lines.
 *
 * Both the TIM7 ISR and the main loop can add lines, so claiming a slot is
 * done with interrupts off (only a few instructions). Only the main loop
 * sends them.
 ******************************************************************************/

/*--------------------------- Includes ---------------------------------------*/
#include "main.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

/*--------------------------- State ------------------------------------------*/

static char lines[LOG_LINES][LOG_LEN];
static volatile uint8_t next_write;
static volatile uint8_t next_read;
static volatile uint8_t waiting;
static volatile uint16_t dropped;       /* lines lost because the buffer was full */

extern UART_HandleTypeDef huart1;       /* in main.c */

/*--------------------------- Implementation ---------------------------------*/

void log_line(const char *text)
{
    uint8_t slot;

    /* only the slot claim needs protecting, the copy is into our own slot */
    __disable_irq();

    if (waiting >= LOG_LINES)
    {
        dropped++;
        __enable_irq();
        return;
    }

    slot = next_write;
    next_write = (uint8_t)((next_write + 1U) % LOG_LINES);
    waiting++;

    __enable_irq();

    strncpy(lines[slot], text, LOG_LEN - 1U);
    lines[slot][LOG_LEN - 1U] = '\0';
}

void log_drain(void)
{
    while (waiting > 0U)
    {
        uint8_t slot = next_read;

        HAL_UART_Transmit(&huart1, (uint8_t *)lines[slot],
                          strlen(lines[slot]), HAL_MAX_DELAY);

        __disable_irq();
        next_read = (uint8_t)((next_read + 1U) % LOG_LINES);
        waiting--;
        __enable_irq();
    }

    /* report dropped lines so we know the log has gaps */
    if (dropped > 0U)
    {
        char note[48];
        uint16_t lost;

        __disable_irq();
        lost = dropped;
        dropped = 0;
        __enable_irq();

        snprintf(note, sizeof(note), "log: %u line(s) dropped\r\n", (unsigned)lost);
        HAL_UART_Transmit(&huart1, (uint8_t *)note, strlen(note), HAL_MAX_DELAY);
    }
}
