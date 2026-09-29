/******************************************************************************
 * @file           : log.h
 * @brief          : Ring buffer of text lines for the UART.
 *
 * HAL_UART_Transmit blocks until every character is out, which we can't do
 * inside the detection interrupt. So the ISR adds lines here and the main
 * loop sends them.
 ******************************************************************************/
#ifndef LOG_H
#define LOG_H

#define LOG_LINES   16
#define LOG_LEN     80      /* including the terminating 0 */

/* Never blocks. If the buffer is full the line is dropped (and counted). */
void log_line(const char *text);

/* Main loop only */
void log_drain(void);

#endif /* LOG_H */
