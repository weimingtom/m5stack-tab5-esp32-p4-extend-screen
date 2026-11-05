#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"

#include "scr_display.h"

static const char *TAG = "scr_disp";

/* The P4 hardware JPEG decoder may pad the decoded picture up to 16 pixels. */
static inline uint32_t align16(uint32_t value)
{
    return (value + 15u) & ~15u;
}

static esp_err_t create_decoder(scr_disp_t *disp)
{
    if (disp->jpeg) {
        return ESP_OK;
    }

    const jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 200,
    };
    return jpeg_new_decoder_engine(&engine_cfg, &disp->jpeg);
}

esp_err_t scr_disp_init(scr_disp_t *disp)
{
    memset(disp, 0, sizeof(*disp));

    const bsp_display_config_t bsp_cfg = {
        .dsi_bus = {
            .phy_clk_src = 0, /* let the driver pick the default clock source */
            .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        },
    };
    esp_lcd_panel_io_handle_t io = NULL;
    esp_err_t err = bsp_display_new(&bsp_cfg, &disp->panel, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_display_new failed: %s", esp_err_to_name(err));
        return err;
    }
    disp->width = BSP_LCD_H_RES;
    disp->height = BSP_LCD_V_RES;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(disp->panel, true), TAG, "panel on failed");

    /* Two frame buffers avoid drawing into the buffer being scanned out. */
    void *fb0 = NULL;
    void *fb1 = NULL;
    err = esp_lcd_dpi_panel_get_frame_buffer(disp->panel, 2, &fb0, &fb1);
    if (err == ESP_OK) {
        disp->fb[0] = (uint8_t *)fb0;
        disp->fb[1] = (uint8_t *)fb1;
        disp->fb_count = 2;
    } else {
        ESP_LOGW(TAG, "two frame buffers unavailable (%s), falling back to one", esp_err_to_name(err));
        fb1 = NULL;
        ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(disp->panel, 1, &fb0), TAG,
                            "no frame buffer");
        disp->fb[0] = (uint8_t *)fb0;
        disp->fb_count = 1;
    }

    const uint32_t padded_w = align16(disp->width);
    const uint32_t padded_h = align16(disp->height);
    const jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    disp->scratch = (uint8_t *)jpeg_alloc_decoder_mem((size_t)padded_w * padded_h * 2, &mem_cfg,
                                                      &disp->scratch_size);
    if (disp->scratch == NULL) {
        ESP_LOGE(TAG, "could not allocate a %" PRIu32 "x%" PRIu32 " decode buffer", padded_w, padded_h);
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(create_decoder(disp), TAG, "jpeg decoder init failed");
    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "backlight failed");

    scr_disp_clear(disp);
    ESP_LOGI(TAG, "panel ready: %ux%u, %d frame buffer(s), scratch %u bytes",
             disp->width, disp->height, disp->fb_count, (unsigned)disp->scratch_size);
    return ESP_OK;
}

void scr_disp_clear(scr_disp_t *disp)
{
    const size_t fb_size = (size_t)disp->width * disp->height * 2;
    for (int i = 0; i < disp->fb_count; i++) {
        if (disp->fb[i]) {
            memset(disp->fb[i], 0, fb_size);
        }
    }
    if (disp->fb_count > 0) {
        esp_lcd_panel_draw_bitmap(disp->panel, 0, 0, disp->width, disp->height, disp->fb[0]);
        disp->fb_next = disp->fb_count > 1 ? 1 : 0;
    }
}

/* Copy @p height rows of @p width pixels into the back frame buffer, centred,
 * and hand the buffer to the panel. */
static esp_err_t present(scr_disp_t *disp, const uint8_t *src, size_t src_stride,
                         uint16_t width, uint16_t height, uint32_t *draw_us)
{
    if (width == 0 || height == 0 || width > disp->width || height > disp->height) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int64_t t0 = esp_timer_get_time();
    uint8_t *fb = disp->fb[disp->fb_next % disp->fb_count];
    const size_t panel_stride = (size_t)disp->width * 2;

    if (width != disp->width || height != disp->height) {
        /* letterbox: clear the bars, then drop the image in the middle */
        memset(fb, 0, panel_stride * disp->height);
    }

    const uint16_t off_x = (uint16_t)((disp->width - width) / 2);
    const uint16_t off_y = (uint16_t)((disp->height - height) / 2);
    uint8_t *dst = fb + (size_t)off_y * panel_stride + (size_t)off_x * 2;
    const size_t row_bytes = (size_t)width * 2;

    for (uint16_t y = 0; y < height; y++) {
        memcpy(dst, src, row_bytes);
        src += src_stride;
        dst += panel_stride;
    }

    /* Passing a driver frame buffer makes the panel driver flush the cache and
     * switch scan-out to it instead of copying the pixels again. */
    const esp_err_t err = esp_lcd_panel_draw_bitmap(disp->panel, 0, 0, disp->width, disp->height, fb);
    if (err == ESP_OK && disp->fb_count > 1) {
        disp->fb_next = (disp->fb_next + 1) % disp->fb_count;
    }
    if (draw_us) {
        *draw_us = (uint32_t)(esp_timer_get_time() - t0);
    }
    return err;
}

esp_err_t scr_disp_draw_jpeg(scr_disp_t *disp, const uint8_t *jpeg, size_t len,
                             uint16_t width, uint16_t height,
                             uint32_t *dec_us, uint32_t *draw_us)
{
    if (disp->jpeg == NULL) {
        ESP_RETURN_ON_ERROR(create_decoder(disp), TAG, "jpeg decoder init failed");
    }

    const size_t padded_w = align16(width);
    const size_t padded_h = align16(height);
    if (padded_w * padded_h * 2 > disp->scratch_size) {
        ESP_LOGW(TAG, "frame %ux%u does not fit the %u byte decode buffer",
                 width, height, (unsigned)disp->scratch_size);
        return ESP_ERR_INVALID_SIZE;
    }

    const jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
#if CONFIG_APP_JPEG_RGB_ORDER_RGB
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
#else
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
#endif
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    const int64_t t0 = esp_timer_get_time();
    uint32_t out_size = 0;
    const esp_err_t err = jpeg_decoder_process(disp->jpeg, &decode_cfg, jpeg, (uint32_t)len,
                                               disp->scratch, (uint32_t)disp->scratch_size, &out_size);
    if (dec_us) {
        *dec_us = (uint32_t)(esp_timer_get_time() - t0);
    }
    if (err != ESP_OK) {
        return err;
    }
    if (out_size < padded_w * height * 2) {
        ESP_LOGW(TAG, "decoder returned %" PRIu32 " bytes for %ux%u", out_size, width, height);
        return ESP_ERR_INVALID_SIZE;
    }

    return present(disp, disp->scratch, padded_w * 2, width, height, draw_us);
}

esp_err_t scr_disp_draw_rgb565(scr_disp_t *disp, const uint8_t *pixels, size_t len,
                               uint16_t width, uint16_t height, uint32_t *draw_us)
{
    if (len < (size_t)width * height * 2) {
        return ESP_ERR_INVALID_SIZE;
    }
    return present(disp, pixels, (size_t)width * 2, width, height, draw_us);
}
