/*
 * Link abstraction: one byte stream between the PC and the Tab5.
 *
 * Two implementations exist, chosen with Kconfig (APP_LINK_USB / APP_LINK_TCP):
 *  - USB  : the ESP32-P4 USB-Serial/JTAG CDC-ACM endpoint, i.e. the same USB-C
 *           cable used for flashing.  No setup, ~1 MB/s.
 *  - TCP  : the on-board ESP32-C6 runs the esp-hosted coprocessor firmware, the
 *           P4 joins your access point and listens on a TCP port.  Faster, but
 *           needs working ESP-Hosted firmware on the C6.
 *
 * Both directions use the same connection: screen frames travel PC -> Tab5 and
 * `LOG`/`STAT` text lines travel Tab5 -> PC.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief Install the driver / start the server. */
esp_err_t scr_link_init(void);

/**
 * @brief Read up to @p len bytes.
 *
 * @return number of bytes read, 0 when nothing arrived before @p timeout_ms,
 *         or a negative value when the link failed (the caller resynchronises).
 */
int scr_link_read(uint8_t *buf, size_t len, uint32_t timeout_ms);

/** @brief Write bytes, best effort (used for status lines). */
int scr_link_write(const void *buf, size_t len);

/** @brief True when a PC is connected. */
bool scr_link_ready(void);
