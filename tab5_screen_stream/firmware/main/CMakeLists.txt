set(sources
    "main.c"
    "scr_frame.c"
    "scr_display.c"
)

set(common_requires
    driver
    esp_driver_jpeg
    esp_lcd
    esp_timer
    m5stack_tab5_noglib
)

# Only the selected transport is compiled; both live in this component so the
# protocol, display and statistics code is shared.
if(CONFIG_APP_LINK_TCP)
    list(APPEND sources "scr_link_tcp.c" "scr_wifi.c")
    idf_component_register(
        SRCS ${sources}
        INCLUDE_DIRS "."
        REQUIRES ${common_requires}
        PRIV_REQUIRES esp_wifi esp_netif nvs_flash
    )

    # On ESP32-P4 there is no local radio: esp_wifi_remote replaces the esp_wifi
    # implementation with one that talks to the C6, and swaps in API headers
    # generated for the co-processor target ("injected").  With ESP-IDF 5.5.6 +
    # esp_hosted 2.13 those headers (and nvs_flash's) do not reach this
    # component's compile line through the normal requirement propagation, so
    # they are added explicitly - injected first, so it wins over the stock one.
    idf_component_get_property(wifi_remote_dir espressif__esp_wifi_remote COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${wifi_remote_dir}/idf_v${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}/include/injected"
    )
    foreach(dep esp_wifi nvs_flash esp_netif)
        idf_component_get_property(dep_dir ${dep} COMPONENT_DIR)
        idf_component_get_property(dep_lib ${dep} COMPONENT_LIB)
        target_include_directories(${COMPONENT_LIB} PRIVATE "${dep_dir}/include")
        target_link_libraries(${COMPONENT_LIB} PRIVATE ${dep_lib})
    endforeach()
else()
    list(APPEND sources "scr_link_usb.c")
    idf_component_register(
        SRCS ${sources}
        INCLUDE_DIRS "."
        REQUIRES ${common_requires}
        PRIV_REQUIRES esp_driver_usb_serial_jtag
    )
endif()
