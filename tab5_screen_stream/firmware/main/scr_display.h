/*
 * Display sink: MIPI-DSI panel bring-up plus JPEG/RGB565 blitting.
 *
 * The Tab5 panel is 720x1280 in its native orientation (portrait), driven by
 * the ESP32-P4 DSI DPI peripheral with two frame buffers allocated by the LCD
 * driver.  Frames are decoded with the P4 hardware JPEG decoder into a PSRAM
 * scratch buffer, letterboxed into the back frame buffer and then handed to
 * the panel, which flushes the cache and switches scan-out to that buffer.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "driver/jpeg_decode.h"

typedef struct {
    esp_lcd_panel_handle_t panel;
    uint16_t width;   /* panel native size, from the BSP */
    uint16_t height;
    uint8_t *fb[2];
    int fb_count;
    int fb_next;      /* frame buffer to draw into next */

    uint8_t *scratch;      /* JPEG decode target, padded dimensions */
    size_t scratch_size;

    jpeg_decoder_handle_t jpeg;
} scr_disp_t;

/** @brief Bring up the panel (panel + PHY + backlight) and grab its frame buffers. */
esp_err_t scr_disp_init(scr_disp_t *disp);

/** @brief Fill every frame buffer with black. */
void scr_disp_clear(scr_disp_t *disp);

/**
 * @brief Decode a JPEG into the panel and show it.
 *
 * @param dec_us  optional, decode time in microseconds
 * @param draw_us optional, blit + panel update time in microseconds
 */
esp_err_t scr_disp_draw_jpeg(scr_disp_t *disp, const uint8_t *jpeg, size_t len,
                             uint16_t width, uint16_t height,
                             uint32_t *dec_us, uint32_t *draw_us);

/** @brief Show a raw little-endian RGB565 buffer (no decoder involved). */
esp_err_t scr_disp_draw_rgb565(scr_disp_t *disp, const uint8_t *pixels, size_t len,
                               uint16_t width, uint16_t height, uint32_t *draw_us);
