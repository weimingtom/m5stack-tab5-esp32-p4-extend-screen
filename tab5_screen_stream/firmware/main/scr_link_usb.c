/*
 * USB link: the ESP32-P4 USB-Serial/JTAG peripheral in CDC-ACM mode.
 *
 * This is the very port esptool uses to flash the Tab5, so no extra cable,
 * driver or configuration is needed.  Because the driver takes the peripheral
 * over, the IDF console must not also own it - CONFIG_ESP_CONSOLE_NONE is set
 * in sdkconfig.defaults and logs are forwarded as "LOG ..." lines instead.
 */

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "scr_link.h"

static const char *TAG = "link_usb";

esp_err_t scr_link_init(void)
{
    const usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 4096,
        .rx_buffer_size = CONFIG_APP_USB_RX_BUFFER_KB * 1024,
    };

    const esp_err_t err = usb_serial_jtag_driver_install((usb_serial_jtag_driver_config_t *)&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "USB-Serial/JTAG ready (%d KB receive buffer)", CONFIG_APP_USB_RX_BUFFER_KB);
    return ESP_OK;
}

int scr_link_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    return usb_serial_jtag_read_bytes(buf, (uint32_t)len, pdMS_TO_TICKS(timeout_ms));
}

int scr_link_write(const void *buf, size_t len)
{
    /* never block the producer for long: the host may not be reading */
    return usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(100));
}

bool scr_link_ready(void)
{
    return usb_serial_jtag_is_connected();
}
