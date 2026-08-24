/**
 * TinyUSB configuration for arply's USB-DAC composite device.
 *
 * Built with CONFIG_USB_DEVICE_UAC_AS_PART=y, which makes the
 * usb_device_uac component leave descriptor ownership entirely to the
 * project instead of building its own UAC-only device. This file and
 * usb_descriptors.c combine a UAC 2.0 speaker (host -> arply audio) with
 * a CDC-ACM serial port, so the console stays available over USB even
 * though the fixed-function USB Serial/JTAG peripheral is not usable at
 * the same time as this custom TinyUSB stack (same physical USB PHY).
 *
 * Layout mirrors
 * managed_components/espressif__usb_device_uac/tusb/tusb_config.h (the
 * component's own default, UAC-only build) with a CDC interface added.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "sdkconfig.h"
#include "uac_config.h"
#include "uac_descriptors.h"
#include "tusb_config_uac.h"

//--------------------------------------------------------------------+
// Board Specific Configuration
//--------------------------------------------------------------------+

#ifdef CONFIG_TINYUSB_RHPORT_HS
#if CONFIG_IDF_TARGET_ESP32P4
#define CFG_TUSB_RHPORT1_MODE (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#else
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#endif
#define CONFIG_USB_HS 1
#else
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CONFIG_USB_HS         0
#endif

//--------------------------------------------------------------------
// Common Configuration
//--------------------------------------------------------------------

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_FREERTOS
#endif

#ifndef ESP_PLATFORM
#define ESP_PLATFORM 1
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#if TU_CHECK_MCU(OPT_MCU_ESP32S2, OPT_MCU_ESP32S3, OPT_MCU_ESP32P4, \
                 OPT_MCU_ESP32S31, OPT_MCU_ESP32H4)
#define CFG_TUSB_OS_INC_PATH freertos /
#endif

#define CFG_TUD_ENABLED 1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

// CDC console (serial log output), in addition to CFG_TUD_AUDIO=1
// (already defined by tusb_config_uac.h).
#define CFG_TUD_CDC            1
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 512

#ifdef __cplusplus
}
#endif
