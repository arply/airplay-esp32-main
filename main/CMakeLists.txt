set( SRC_FILES
    "main.c"
    "alac_magic_cookie.c"
    "settings.c"
    "audio/audio_receiver.c"
    "audio/audio_stream.c"
    "audio/audio_stream_realtime.c"
    "audio/audio_stream_buffered.c"
    "audio/audio_decoder.c"
    "audio/audio_buffer.c"
    "audio/audio_timing.c"
    "audio/audio_crypto.c"
    "audio/audio_resample.c"
    "rtsp/rtsp_server.c"
    "rtsp/rtsp_conn.c"
    "rtsp/rtsp_message.c"
    "rtsp/rtsp_handlers.c"
    "rtsp/rtsp_fairplay.c"
    "rtsp/rtsp_crypto.c"
    "rtsp/rtsp_rsa.c"
    "rtsp/rtsp_events.c"
    "plist/plist_xml.c"
    "plist/bplist_parser.c"
    "plist/bplist_builder.c"
    "plist/base64.c"
    "hap/hap.c"
    "hap/hap_pair_verify.c"
    "hap/hap_pair_setup.c"
    "hap/hap_crypto.c"
    "hap/srp.c"
    "hap/tlv8.c"
    "network/wifi.c"
    "network/mdns_airplay.c"
    "network/ptp_clock.c"
    "network/ntp_clock.c"
    "network/socket_utils.c"
    "network/web_server.c"
    "network/ota.c"
    "network/dns_server.c"
    "network/log_stream.c"
    "led.c"
    "buttons.c"
    "playback_control.c"
    "dacp_client.c"
)

# Select audio output implementation at compile time
if(CONFIG_AUDIO_OUTPUT_SPDIF)
    list(APPEND SRC_FILES "audio/audio_output_spdif.c")
elseif(CONFIG_AUDIO_OUTPUT_USB)
    list(APPEND SRC_FILES "audio/audio_output_usb.c")
else()
    list(APPEND SRC_FILES "audio/audio_output.c")
endif()

# USB-DAC alternate input source (composite UAC speaker + CDC console).
#
# usb_dac_input.c and usb_descriptors.c both need CFG_TUSB_MCU and
# tinyusb's own private FreeRTOS glue include path (freertos/FreeRTOS.h
# resolved via CFG_TUSB_OS_INC_PATH), neither of which PRIV_REQUIRES makes
# visible: ESP-IDF's REQUIRES/PRIV_REQUIRES only propagate a required
# component's *public include dirs*, not its target_compile_options() or
# private include dirs. tinyusb sets CFG_TUSB_MCU on itself via
# target_compile_options(... PUBLIC ...), which only reaches its own
# sources — not ours, even as a "requiring" component. usb_device_uac's
# own CMakeLists.txt hits the same wall for its (AS_PART-only) reference
# usb_descriptors.c and works around it the same way: attach the sources
# directly to the tinyusb component's target below via target_sources(),
# so they compile as tinyusb's own sources and inherit its flags for real.
# They are deliberately NOT added to SRC_FILES/DEPS here.

# W5500 SPI Ethernet
if(CONFIG_ETH_W5500_ENABLED)
    list(APPEND SRC_FILES "network/ethernet.c")
endif()

set( INC_DIRS
    "."
    "audio"
    "rtsp"
    "plist"
    "hap"
    "network"
)

if(CONFIG_AUDIO_INPUT_USB_DAC)
    list(APPEND INC_DIRS "usb")
endif()

set( DEPS
    driver
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_ringbuf
    esp_driver_i2s
    esp_timer
    esp_http_server
    esp_http_client
    app_update
    json
    mbedtls
    esp_driver_ledc
    boards
    audio-resampler
    esp_eth
    spiffs_storage
)

# Ethernet component only needed when W5500 is enabled
#if(CONFIG_ETH_W5500_ENABLED)
    list(APPEND DEPS esp_eth)
#endif()

# usb_device_uac and tinyusb are pulled into the build unconditionally via
# main/idf_component.yml (target-gated, not CONFIG_AUDIO_INPUT_USB_DAC-gated),
# so they're resolvable below regardless of whether main itself REQUIRES
# them — and main no longer needs to: usb_dac_input.c/usb_descriptors.c are
# compiled as tinyusb's own sources (see below), and main.c only sees
# usb_dac_input.h, which is tinyusb-free.
if(CONFIG_AUDIO_INPUT_USB_DAC)
    idf_build_get_property(build_components BUILD_COMPONENTS)
    if(tinyusb IN_LIST build_components)
        set(usb_dac_tusb_name tinyusb)
    else()
        set(usb_dac_tusb_name espressif__tinyusb)
    endif()
    if(usb_device_uac IN_LIST build_components)
        set(usb_dac_uac_name usb_device_uac)
    else()
        set(usb_dac_uac_name espressif__usb_device_uac)
    endif()
endif()

idf_component_register(
    SRCS ${SRC_FILES}
    INCLUDE_DIRS ${INC_DIRS}
    PRIV_REQUIRES ${DEPS}
)

# CONFIG_USB_DEVICE_UAC_AS_PART leaves usb_device_uac's own
# tusb_config.h/usb_descriptors.c unbuilt (see main/usb/ for this
# project's replacements), which also means IT normally adds
# uac_descriptors.h/uac_config.h/tusb_config_uac.h to the tinyusb
# component's public include path — and skips doing so in AS_PART mode.
# Restore that here, the same way its own CMakeLists.txt does it, so both
# usb_device_uac.c and this project's own USB sources can still reach
# those headers.
#
# usb_dac_input.c and usb_descriptors.c are attached directly to tinyusb's
# own target (target_sources), not main's — see the comment above SRC_FILES
# for why. They still need main's own headers (audio_output.h, live in
# main/audio/, resolved via quote-include relative to usb_dac_input.c
# itself; playback_control.h/led.h live at main/ root and rtsp_events.h in
# main/rtsp/ need it added explicitly), so that's added here as a private
# include dir on tinyusb.
if(CONFIG_AUDIO_INPUT_USB_DAC)
    idf_component_get_property(usb_dac_tusb_lib ${usb_dac_tusb_name} COMPONENT_LIB)
    idf_component_get_property(usb_dac_uac_dir ${usb_dac_uac_name} COMPONENT_DIR)
    # esp_timer.h / freertos/ringbuf.h: tinyusb itself only REQUIRES
    # freertos, so its own public include path doesn't carry these along —
    # usb_dac_input.c needs esp_timer directly (esp_timer_get_time() for the
    # idle-timeout check) and esp_ringbuf for its ring buffer, so add both
    # here the same way as the other headers this file wouldn't otherwise see.
    idf_component_get_property(usb_dac_esp_timer_dir esp_timer COMPONENT_DIR)
    idf_component_get_property(usb_dac_esp_ringbuf_dir esp_ringbuf COMPONENT_DIR)
    target_include_directories(${usb_dac_tusb_lib} PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/usb"
        "${usb_dac_uac_dir}/tusb_uac"
        "${usb_dac_uac_dir}/include"
    )
    target_include_directories(${usb_dac_tusb_lib} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/rtsp"
        "${usb_dac_esp_timer_dir}/include"
        "${usb_dac_esp_ringbuf_dir}/include"
    )
    target_sources(${usb_dac_tusb_lib} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/audio/usb_dac_input.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/usb/usb_descriptors.c"
    )
endif()
