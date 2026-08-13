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
    "audio/eq_events.c"
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
# CONFIG_AUDIO_INPUT_USB_DAC selects CONFIG_USB_DEVICE_UAC_AS_PART, which
# makes usb_device_uac leave its own tusb_config.h/usb_descriptors.c
# unbuilt, expecting the project to supply them (see main/usb/). That also
# means the include paths it would normally add to the tinyusb component
# have to be added here instead — same mechanism, just triggered by our
# config instead of "not AS_PART".
if(CONFIG_AUDIO_INPUT_USB_DAC)
    list(APPEND SRC_FILES "audio/usb_dac_input.c" "usb/usb_descriptors.c")
endif()

# Bluetooth A2DP sink (only available on ESP32 with Classic BT)
if(CONFIG_BT_A2DP_ENABLE)
    list(APPEND SRC_FILES "audio/a2dp_sink.c")
    list(APPEND SRC_FILES "bt_coex.c")
endif()

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
    dac
    dac_tas58xx
    dac_tas57xx
    display
    audio-resampler
    esp_eth
    spiffs_storage
)

# Bluetooth component only needed when A2DP is enabled
#if(CONFIG_BT_A2DP_ENABLE)
    list(APPEND DEPS bt)
#endif()

# Ethernet component only needed when W5500 is enabled
#if(CONFIG_ETH_W5500_ENABLED)
    list(APPEND DEPS esp_eth)
#endif()

# USB-DAC input needs direct TinyUSB header access (tud_cdc_*, the
# tud_*_cb lifecycle callbacks) that the existing AUDIO_OUTPUT_USB variant
# never required.
if(CONFIG_AUDIO_INPUT_USB_DAC)
    idf_build_get_property(build_components BUILD_COMPONENTS)
    if(tinyusb IN_LIST build_components)
        set(usb_dac_tusb_name tinyusb)
    else()
        set(usb_dac_tusb_name espressif__tinyusb)
    endif()
    list(APPEND DEPS usb_device_uac ${usb_dac_tusb_name})
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
if(CONFIG_AUDIO_INPUT_USB_DAC)
    idf_component_get_property(usb_dac_tusb_lib ${usb_dac_tusb_name} COMPONENT_LIB)
    idf_component_get_property(usb_dac_uac_dir usb_device_uac COMPONENT_DIR)
    target_include_directories(${usb_dac_tusb_lib} PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/usb"
        "${usb_dac_uac_dir}/tusb_uac"
    )
endif()
