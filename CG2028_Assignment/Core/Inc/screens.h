/******************************************************************************
 * @file           : screens.h
 * @brief          : What goes on the OLED for each incident state.
 *
 * Only reads the incident state, never changes it.
 ******************************************************************************/
#ifndef SCREENS_H
#define SCREENS_H

void screens_init(void);

/* Status line on the boot screen (e.g. while joining Wi-Fi) */
void screens_boot_note(const char *text);

/* Main loop. Only redraws when something visible changed. */
void screens_update(void);

#endif /* SCREENS_H */
