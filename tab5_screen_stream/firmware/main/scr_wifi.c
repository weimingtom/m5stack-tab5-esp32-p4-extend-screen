/*
 * Wi-Fi station bring-up for the Tab5.
 *
 * ESP32-P4 has no radio: on this board an ESP32-C6 module is wired to SDIO and
 * runs the ESP-Hosted coprocessor firmware.  With the esp_wifi_remote component
 * installed, the plain esp_wifi API below is transparently forwarded to the C6
 * (RPC for control, SDIO frames for data), so nothing here is Tab5 specific
 * apart from powering the C6 through the IO expander first.
 */

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#include "bsp/m5stack_tab5.h"

#include "scr_wifi.h"

static const char *TAG = "scr_wifi";

static EventGroupHandle_t s_wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAILED_BIT = BIT1;
static int s_retries;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries++ < CONFIG_APP_WIFI_MAX_RETRY) {
            ESP_LOGW(TAG, "disconnected, retrying (%d/%d)", s_retries, CONFIG_APP_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t scr_wifi_start(void)
{
    /* The C6 module is powered through the second IO expander. */
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_WIFI, true), TAG, "C6 power enable failed");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "could not create the station netif");
        return ESP_FAIL;
    }

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            on_wifi_event, NULL, NULL), TAG,
                        "wifi event handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            on_wifi_event, NULL, NULL), TAG,
                        "ip event handler failed");

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    ESP_LOGI(TAG, "connecting to \"%s\" ...", CONFIG_APP_WIFI_SSID);
    const EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                                 pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(CONFIG_APP_WIFI_TIMEOUT_S * 1000));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "could not join \"%s\" (check the credentials and the C6 ESP-Hosted firmware)",
             CONFIG_APP_WIFI_SSID);
    return ESP_ERR_TIMEOUT;
}
