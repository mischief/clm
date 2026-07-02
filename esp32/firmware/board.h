// SPDX-License-Identifier: ISC
/*
 * Board bring-up interface. Each board (board_tdeck.c, board_cardputer.c)
 * implements these; exactly one is active per build (guarded by the BOARD_*
 * macro). Shared firmware (main.c) calls board_init() before display_init(),
 * then polls keyboard_read().
 */
#ifndef CLM_BOARD_H
#define CLM_BOARD_H

/* Power rail, SPI bus for the panel, SD card (if any), and keyboard. Called
 * once at startup before display_init(). */
void board_init(void);

/* Next pressed ASCII character, or 0 if none / no keyboard. */
int keyboard_read(void);

#endif /* CLM_BOARD_H */
