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

    // CONFIG_UAC_TUSB_VID/PID (usb_device_uac's own Kconfig) only exist
    // when !USB_DEVICE_UAC_AS_PART, which this build isn't — literal here
    // for the same reason the string descriptors below are, and matching
    // usb_device_uac's own defaults (shared Espressif VID; no unique PID
    // registered for this project).
    .idVendor = 0x303A,
    .idProduct = 0x8000,
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

    // UAC speaker, inlined from uac_descriptors.h's own
    // TUD_AUDIO_SPEAK_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, 5, EPNUM_AUDIO_OUT,
    // EPNUM_AUDIO_FB) with one change: that macro hardcodes the IAD's own
    // string index to 0x00 (no string), which is what macOS actually reads
    // to name this composite USB audio function — with no string there, it
    // falls back to showing the raw driver class name ("IOUSBHostInterface")
    // instead of a friendly device name. Point it at string index 6 instead.
    /* Standard Interface Association Descriptor (IAD) */
    TUD_AUDIO_DESC_IAD(/*_firstitfs*/ ITF_NUM_AUDIO_CONTROL,
                       /*_nitfs*/ NUM_INTERFACES, /*_stridx*/ 6),
    /* Standard AC Interface Descriptor(4.7.1) */
    TUD_AUDIO_DESC_STD_AC(/*_itfnum*/ ITF_NUM_AUDIO_CONTROL, /*_nEPs*/ 0x00,
                          /*_stridx*/ 5),
    /* Class-Specific AC Interface Header Descriptor(4.7.2) */
    TUD_AUDIO_DESC_CS_AC(/*_bcdADC*/ 0x0200,
                         /*_category*/ AUDIO_FUNC_DESKTOP_SPEAKER,
                         /*_totallen*/ TUD_AUDIO_DESC_CS_AC_TOTAL_LEN,
                         /*_ctrl*/ AUDIO_CS_AS_INTERFACE_CTRL_LATENCY_POS),
    /* Clock Source Descriptor(4.7.2.1) */
    TUD_AUDIO_DESC_CLK_SRC(/*_clkid*/ UAC2_ENTITY_CLOCK, /*_attr*/ 3,
                           /*_ctrl*/ 7, /*_assocTerm*/ 0x00, /*_stridx*/ 0x00),
    /* Input Terminal Descriptor(4.7.2.4) */
    TUD_AUDIO_DESC_INPUT_TERM(
        /*_termid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL,
        /*_termtype*/ AUDIO_TERM_TYPE_USB_STREAMING, /*_assocTerm*/ 0x00,
        /*_clkid*/ UAC2_ENTITY_CLOCK, /*_nchannelslogical*/ SPEAK_CHANNEL_NUM,
        /*_channelcfg*/ AUDIO_CHANNEL_CONFIG_NON_PREDEFINED,
        /*_idxchannelnames*/ 0x00,
        /*_ctrl*/ (AUDIO_CTRL_R << AUDIO_IN_TERM_CTRL_CONNECTOR_POS),
        /*_stridx*/ 0x00),
    /* Feature Unit Descriptor(4.7.2.8) */
    TUD_AUDIO_DESC_FEATURE_UNIT_N_CHANNEL(
        /*_length*/ TUD_AUDIO_DESC_SPK_FEATURE_UNIT_N_CHANNEL_LEN,
        /*_unitid*/ UAC2_ENTITY_SPK_FEATURE_UNIT,
        /*_srcid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL, /*_stridx*/ 0x00,
        INPUT_CTRL),
    /* Output Terminal Descriptor(4.7.2.5) */
    TUD_AUDIO_DESC_OUTPUT_TERM(
        /*_termid*/ UAC2_ENTITY_SPK_OUTPUT_TERMINAL,
        /*_termtype*/ AUDIO_TERM_TYPE_OUT_GENERIC_SPEAKER,
        /*_assocTerm*/ 0x00, /*_srcid*/ UAC2_ENTITY_SPK_FEATURE_UNIT,
        /*_clkid*/ UAC2_ENTITY_CLOCK, /*_ctrl*/ 0x0000, /*_stridx*/ 0x00),
    /* Interface 1, Alternate 0 - default alternate setting, 0 bandwidth */
    TUD_AUDIO_DESC_STD_AS_INT(/*_itfnum*/ ITF_NUM_AUDIO_CONTROL + 1,
                              /*_altset*/ 0x00, /*_nEPs*/ 0x00,
                              /*_stridx*/ 6),
    /* Interface 1, Alternate 1 - alternate interface for data streaming */
    TUD_AUDIO_DESC_STD_AS_INT(/*_itfnum*/ ITF_NUM_AUDIO_CONTROL + 1,
                              /*_altset*/ 0x01, /*_nEPs*/ 0x02,
                              /*_stridx*/ 6),
    /* Class-Specific AS Interface Descriptor(4.9.2) */
    TUD_AUDIO_DESC_CS_AS_INT(
        /*_termid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL, /*_ctrl*/ AUDIO_CTRL_NONE,
        /*_formattype*/ AUDIO_FORMAT_TYPE_I,
        /*_formats*/ AUDIO_DATA_FORMAT_TYPE_I_PCM,
        /*_nchannelsphysical*/ SPEAK_CHANNEL_NUM,
        /*_channelcfg*/ AUDIO_CHANNEL_CONFIG_NON_PREDEFINED,
        /*_stridx*/ 0x00),
    /* Type I Format Type Descriptor(2.3.1.6 - Audio Formats) */
    TUD_AUDIO_DESC_TYPE_I_FORMAT(
        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX,
        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX),
    /* Standard AS Isochronous Audio Data Endpoint Descriptor(4.10.1.1) */
    TUD_AUDIO_DESC_STD_AS_ISO_EP(
        /*_ep*/ EPNUM_AUDIO_OUT,
        /*_attr*/
        (TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS |
         TUSB_ISO_EP_ATT_DATA),
        /*_maxEPsize*/ CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_OUT,
        /*_interval*/ 1),
    /* Class-Specific AS Isochronous Audio Data Endpoint Descriptor(4.10.1.2) */
    TUD_AUDIO_DESC_CS_AS_ISO_EP(
        /*_attr*/ AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK,
        /*_ctrl*/ AUDIO_CTRL_NONE,
        /*_lockdelayunit*/ AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC,
        /*_lockdelay*/ 0x0001),
    /* Standard AS Isochronous Audio Data Endpoint Descriptor(4.10.1.1) */
    TUD_AUDIO_DESC_STD_AS_ISO_FB_EP(/*_ep*/ EPNUM_AUDIO_FB, /*_epsize*/ 4,
                                    /*_interval*/ 1),
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
    "arply",                    // 1: manufacturer
    "arply USB-DAC",            // 2: product
    "1",                        // 3: serial number
    "arply console",            // 4: CDC interface
    "arply audio",              // 5: UAC control interface
    "Arply USB DAC",            // 6: IAD (the audio function's name, and
                                //    what macOS shows in Sound/Audio MIDI
                                //    Setup instead of "IOUSBHostInterface")
                                //    + the streaming interface, reusing it
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
