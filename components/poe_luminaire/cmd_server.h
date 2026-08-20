/*
 * Minimal TCP server for text commands (one client at a time).
 *
 * Protocol: one text line per command, terminated by '\n' (an optional
 * '\r' right before the LF is ignored). Response: one or more lines
 * terminated by '\n', starting with "OK" or "ERR".
 *
 * Supported commands:
 *   PING                 -> "OK PONG"
 *   HELP                 -> list of commands
 *   STATUS                -> one line "OK STATUS key=value key=value ..."
 *   ON                    -> enables the HV9910 driver, resuming the last brightness
 *                             (refused unless PoE or AUX power is confirmed)
 *   OFF                   -> disables the HV9910 driver
 *   DIM <0-100>            -> sets the brightness; DIM 0 also disables the driver if it
 *                             was on, DIM >0 also enables it if it was off (same PoE/AUX
 *                             gate as ON)
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the TCP server task on the given port (accepts connections one
 * at a time, processed sequentially). */
void cmd_server_start(uint16_t port);

#ifdef __cplusplus
}
#endif
