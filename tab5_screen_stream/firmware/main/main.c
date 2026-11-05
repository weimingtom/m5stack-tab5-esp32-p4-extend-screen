/*
 * M5Stack Tab5 (ESP32-P4) PC screen receiver.
 *
 * python/screen_stream.py captures the desktop and sends length-prefixed
 * frames; this firmware receives them into PSRAM, checks the CRC, decodes JPEG
 * with the P4 hardware decoder and paints the result on the 5" 720x1280
 * MIPI-DSI panel.  Status lines go back to the PC over the same link.
 *
 * Task layout:
 *   reader  : link -> PSRAM payload slot -> ready queue (two slots, ping-pong)
 *   decoder : ready queue -> hardware JPEG decode -> frame buffer -> panel
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "scr_display.h"
#include "scr_frame.h"
#include "scr_link.h"

static const char *TAG = "screen";

#define SLOT_COUNT 2

typedef struct {
    uint8_t slot;
    scr_hdr_t hdr;
} frame_msg_t;

static scr_disp_t s_disp;
static uint8_t *s_slot[SLOT_COUNT];
static size_t s_slot_size;
static QueueHandle_t s_free_slots;
static QueueHandle_t s_ready_frames;

static struct {
    uint32_t frames_ok;
    uint32_t dropped;
    uint32_t bad_crc;
    uint32_t bad_header;
    uint32_t decode_errors;
    uint64_t bytes;
    int64_t window_start_us;
    uint32_t window_frames;
    uint64_t window_bytes;
    uint32_t last_decode_us;
    uint32_t last_draw_us;
} s_stats;

/* ------------------------------------------------------------------------- */
/* status lines (the USB console is owned by the driver, so we speak to the PC
 * through the link instead)                                                  */
/* ------------------------------------------------------------------------- */

static void link_line(const char *kind, const char *fmt, va_list args)
{
    char body[192];
    vsnprintf(body, sizeof(body), fmt, args);

    char line[224];
    const int len = snprintf(line, sizeof(line), "%s %s\n", kind, body);
    if (len > 0) {
        scr_link_write(line, (size_t)len);
    }
}

static void link_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    link_line("LOG", fmt, args);
    va_end(args);
}

/* ------------------------------------------------------------------------- */
/* reader                                                                     */
/* ------------------------------------------------------------------------- */

static bool link_read_exact(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t got = 0;
    int64_t last_progress_us = esp_timer_get_time();

    while (got < len) {
        const int n = scr_link_read(buf + got, len - got, 200);
        if (n < 0) {
            return false;
        }
        if (n > 0) {
            got += (size_t)n;
            last_progress_us = esp_timer_get_time();
            continue;
        }
        if (esp_timer_get_time() - last_progress_us > (int64_t)timeout_ms * 1000) {
            return false;
        }
    }
    return true;
}

/* Consume and drop @p len bytes, keeping the stream in sync after a drop. */
static bool link_discard(uint32_t len)
{
    uint8_t scratch[1024];
    while (len > 0) {
        const size_t chunk = len > sizeof(scratch) ? sizeof(scratch) : len;
        if (!link_read_exact(scratch, chunk, 2000)) {
            return false;
        }
        len -= (uint32_t)chunk;
    }
    return true;
}

/* Byte-wise scan for the frame magic - only runs at a frame boundary or after
 * a resynchronisation, so the cost is negligible. */
static bool sync_magic(void)
{
    static const char magic[SCR_MAGIC_LEN] = { 'S', 'C', 'R', 'F' };
    int matched = 0;

    while (matched < SCR_MAGIC_LEN) {
        uint8_t byte;
        const int n = scr_link_read(&byte, 1, 200);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            continue;
        }
        if (byte == (uint8_t)magic[matched]) {
            matched++;
        } else {
            matched = (byte == (uint8_t)magic[0]) ? 1 : 0;
        }
    }
    return true;
}

static void reader_task(void *arg)
{
    uint8_t hdr_raw[SCR_HEADER_SIZE];

    for (;;) {
        if (!sync_magic()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        memcpy(hdr_raw, "SCRF", SCR_MAGIC_LEN);
        if (!link_read_exact(hdr_raw + SCR_MAGIC_LEN, SCR_HEADER_SIZE - SCR_MAGIC_LEN, 2000)) {
            continue;
        }

        scr_hdr_t hdr;
        if (!scr_hdr_parse(hdr_raw, &hdr)) {
            s_stats.bad_header++;
            if (s_stats.bad_header <= 5 || (s_stats.bad_header % 50) == 0) {
                link_log("bad header (version %u, flags 0x%02x), resyncing",
                         hdr_raw[4], hdr_raw[5]);
            }
            continue;
        }

        if (hdr.payload_len > s_slot_size) {
            /* A raw RGB565 frame of the full panel is ~1.8 MB, which does not
             * fit the default 512 KB slot: refuse it instead of overflowing. */
            s_stats.bad_header++;
            if (s_stats.bad_header <= 5 || (s_stats.bad_header % 50) == 0) {
                link_log("frame %" PRIu32 " is %" PRIu32 " bytes, larger than the %u KB slot "
                         "(raise CONFIG_APP_PAYLOAD_BUFFER_KB), dropped",
                         hdr.seq, hdr.payload_len, (unsigned)(s_slot_size / 1024));
            }
            link_discard(hdr.payload_len);
            continue;
        }

        /* The ping-pong slots follow the sender: if it runs ahead we stall the
         * link instead of dropping, which the USB CDC flow control handles. */
        uint8_t slot = 0;
        if (xQueueReceive(s_free_slots, &slot, pdMS_TO_TICKS(1500)) != pdTRUE) {
            s_stats.dropped++;
            link_discard(hdr.payload_len);
            continue;
        }

        if (!link_read_exact(s_slot[slot], hdr.payload_len, 5000)) {
            xQueueSend(s_free_slots, &slot, 0);
            continue;
        }

        if (scr_crc32(0, s_slot[slot], hdr.payload_len) != hdr.crc32) {
            s_stats.bad_crc++;
            xQueueSend(s_free_slots, &slot, 0);
            if (s_stats.bad_crc <= 5 || (s_stats.bad_crc % 50) == 0) {
                link_log("crc mismatch on frame %" PRIu32 ", dropped", hdr.seq);
            }
            continue;
        }

        const frame_msg_t msg = { .slot = slot, .hdr = hdr };
        if (xQueueSend(s_ready_frames, &msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
            s_stats.dropped++;
            xQueueSend(s_free_slots, &slot, 0);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* decoder                                                                    */
/* ------------------------------------------------------------------------- */

static void report_stats(const frame_msg_t *msg, esp_err_t err)
{
    const int64_t now = esp_timer_get_time();
    s_stats.window_frames++;
    s_stats.window_bytes += msg->hdr.payload_len;

    const int64_t elapsed_us = now - s_stats.window_start_us;
    if (elapsed_us < (int64_t)CONFIG_APP_STATS_INTERVAL_MS * 1000) {
        return;
    }
    const double seconds = (double)elapsed_us / 1e6;
    const size_t free_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;

    char body[192];
    int len = snprintf(body, sizeof(body),
                       "seq=%" PRIu32 " w=%u h=%u bytes=%" PRIu32 " dec_us=%" PRIu32
                       " draw_us=%" PRIu32 " fps=%.1f kbps=%.0f drop=%" PRIu32
                       " badcrc=%" PRIu32 " free_kb=%u ok=%d",
                       msg->hdr.seq, msg->hdr.width, msg->hdr.height, msg->hdr.payload_len,
                       s_stats.last_decode_us, s_stats.last_draw_us,
                       (double)s_stats.window_frames / seconds,
                       (double)s_stats.window_bytes * 8.0 / seconds / 1000.0,
                       s_stats.dropped, s_stats.bad_crc, (unsigned)free_kb, (int)err);
    if (len > 0) {
        scr_link_write("STAT ", 5);
        scr_link_write(body, (size_t)len);
        scr_link_write("\n", 1);
    }

    s_stats.window_start_us = now;
    s_stats.window_frames = 0;
    s_stats.window_bytes = 0;
}

static void decoder_task(void *arg)
{
    frame_msg_t msg;

    for (;;) {
        if (xQueueReceive(s_ready_frames, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint32_t decode_us = 0;
        uint32_t draw_us = 0;
        esp_err_t err;

        if (msg.hdr.flags & SCR_FLAG_JPEG) {
            err = scr_disp_draw_jpeg(&s_disp, s_slot[msg.slot], msg.hdr.payload_len,
                                     msg.hdr.width, msg.hdr.height, &decode_us, &draw_us);
        } else {
            err = scr_disp_draw_rgb565(&s_disp, s_slot[msg.slot], msg.hdr.payload_len,
                                       msg.hdr.width, msg.hdr.height, &draw_us);
        }

        s_stats.last_decode_us = decode_us;
        s_stats.last_draw_us = draw_us;

        if (err == ESP_OK) {
            s_stats.frames_ok++;
            s_stats.bytes += msg.hdr.payload_len;
        } else {
            s_stats.decode_errors++;
            if (s_stats.decode_errors <= 5 || (s_stats.decode_errors % 50) == 0) {
                link_log("frame %" PRIu32 " (%ux%u) failed: %s", msg.hdr.seq, msg.hdr.width,
                         msg.hdr.height, esp_err_to_name(err));
            }
        }

        report_stats(&msg, err);
        xQueueSend(s_free_slots, &msg.slot, 0);
    }
}

/* ------------------------------------------------------------------------- */

static void alloc_slots(void)
{
    s_slot_size = (size_t)CONFIG_APP_PAYLOAD_BUFFER_KB * 1024;
    for (int i = 0; i < SLOT_COUNT; i++) {
        s_slot[i] = (uint8_t *)heap_caps_malloc(s_slot_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_slot[i] == NULL) {
            ESP_LOGE(TAG, "no PSRAM for frame slot %d (%u bytes)", i, (unsigned)s_slot_size);
        }
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);

    if (scr_disp_init(&s_disp) != ESP_OK) {
        ESP_LOGE(TAG, "display init failed, nothing to draw on");
    }

    alloc_slots();
    if (s_slot[0] == NULL || s_slot[1] == NULL) {
        ESP_LOGE(TAG, "out of PSRAM, aborting");
        return;
    }

    s_free_slots = xQueueCreate(SLOT_COUNT, sizeof(uint8_t));
    s_ready_frames = xQueueCreate(SLOT_COUNT, sizeof(frame_msg_t));
    if (s_free_slots == NULL || s_ready_frames == NULL) {
        ESP_LOGE(TAG, "queue creation failed");
        return;
    }
    for (uint8_t i = 0; i < SLOT_COUNT; i++) {
        xQueueSend(s_free_slots, &i, 0);
    }

    const esp_err_t link_err = scr_link_init();
    if (link_err != ESP_OK) {
        ESP_LOGE(TAG, "link init failed: %s", esp_err_to_name(link_err));
    }

    s_stats.window_start_us = esp_timer_get_time();

    xTaskCreatePinnedToCore(reader_task, "scr_reader", 6144, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(decoder_task, "scr_decoder", 6144, NULL, 9, NULL, 1);

    link_log("Tab5 screen receiver up: panel %ux%u, slots %dx%d KB, PSRAM free %u KB",
             s_disp.width, s_disp.height, SLOT_COUNT, CONFIG_APP_PAYLOAD_BUFFER_KB,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
#if CONFIG_APP_LINK_USB
    link_log("waiting for frames on USB-Serial/JTAG - run: python screen_stream.py --serial <PORT>");
#else
    link_log("waiting for frames on tcp/%d - run: python screen_stream.py --tcp <TAB5 IP>",
             CONFIG_APP_TCP_PORT);
#endif
}
