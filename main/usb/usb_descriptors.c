/**
 * Composite USB descriptors: UAC 2.0 speaker (host -> arply audio, for
 * USB-DAC mode) + CDC-ACM serial (keeps a console available since the
 * fixed-function USB Serial/JTAG peripheral can't run at the same time
 * as this custom TinyUSB device stack — see main/usb/tusb_config.h).
 *
 * Adapted from managed_components/espressif__usb_device_uac's own
 * tusb/usb_descriptors.c (its UAC-only default build), with a CDC
 * interface added via the standard TinyUSB Interface Association
 * Descriptor pattern. The UAC macros (TUD_AUDIO_DESCRIPTOR and friends)
 * come from that component's tusb_uac/uac_descriptors.h — unchanged,
 * just invoked with an interface layout this file owns instead of the
 * component's own ITF_NUM_* enum (which only exists when
 * CONFIG_USB_DEVICE_UAC_AS_PART is unset).
 */

#include "tusb.h"
#include "uac_descriptors.h"

//--------------------------------------------------------------------+
// Interface & endpoint layout
//--------------------------------------------------------------------+

enum {
  ITF_NUM_CDC = 0,
  ITF_NUM_CDC_DATA,
  ITF_NUM_AUDIO_CONTROL,
  ITF_NUM_AUDIO_STREAMING_SPK, // = ITF_NUM_AUDIO_CONTROL + 1, required by
                               // TUD_AUDIO_DESCRIPTOR's internal layout
  ITF_NUM_TOTAL
};

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_AUDIO_OUT 0x03
#define EPNUM_AUDIO_FB  0x83

// Exposed so usb_dac_input.c can pass the right streaming interface number
// to uac_device_config_t.spk_itf_num without duplicating this enum.
uint8_t const usb_dac_spk_itf_num = ITF_NUM_AUDIO_STREAMING_SPK;

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,

    // Interface Association Descriptors (one for CDC, one for UAC) require
    // this device-level class/subclass/protocol combination.
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = CONFIG_UAC_TUSB_VID,
    .idProduct = CONFIG_UAC_TUSB_PID,
    .bcdDevice = 0x0100,

    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,

    .bNumConfigurations = 0x01};

uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
#define CONFIG_TOTAL_LEN \
  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_AUDIO_DEVICE_DESC_LEN)

uint8_t const desc_configuration[] = {
    // Config number, interface count, string index, total length,
    // attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // CDC: control interface, string index, notif EP, notif size, data OUT,
    // data IN, data EP size
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT,
                       EPNUM_CDC_IN, 64),

    // UAC speaker: control interface number, string index, EP out, EP in
    // (unused in the speaker-only build — TUD_AUDIO_DESCRIPTOR ignores it),
    // feedback EP
    TUD_AUDIO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, 5, EPNUM_AUDIO_OUT, 0,
                        EPNUM_AUDIO_FB),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, // 0: English (0x0409)
    CONFIG_UAC_TUSB_MANUFACTURER, // 1
    CONFIG_UAC_TUSB_PRODUCT,      // 2
    CONFIG_UAC_TUSB_SERIAL_NUM,   // 3
    "arply console",              // 4: CDC interface
    "arply audio",                // 5: UAC control interface (streaming
                                   //    interface uses index 6, i.e. +1)
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;

  uint8_t chr_count;

  if (index == 0) {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  } else {
    if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
      return NULL;
    }

    const char *str = string_desc_arr[index];
    chr_count = (uint8_t)strlen(str);
    if (chr_count > 31) {
      chr_count = 31;
    }

    for (uint8_t i = 0; i < chr_count; i++) {
      _desc_str[1 + i] = str[i];
    }
  }

  _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

  return _desc_str;
}
