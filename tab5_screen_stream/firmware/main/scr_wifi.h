#pragma once

#include "esp_err.h"

/** @brief Power up the C6, join the configured AP and wait for an IP address. */
esp_err_t scr_wifi_start(void);
