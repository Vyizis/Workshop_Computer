#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_RHPORT0_MODE       OPT_MODE_DEVICE
#define CFG_TUSB_OS                 OPT_OS_PICO
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG              0
#endif

#define CFG_TUD_ENDPOINT0_SIZE      64

// Composite device: CDC sample loader + WebMIDI editor
#define CFG_TUD_CDC                 1
#define CFG_TUD_CDC_RX_BUFSIZE      2048
#define CFG_TUD_CDC_TX_BUFSIZE      1024

// Composite device: CDC sample loader + WebMIDI pattern editor.
// MIDI TX must be large enough for a FULL_DUMP (~200 stream bytes ≈ many
// USB MIDI packets); 256 was too small and silently truncated dumps.
#define CFG_TUD_MIDI                1
#define CFG_TUD_MIDI_RX_BUFSIZE     1024
#define CFG_TUD_MIDI_TX_BUFSIZE     1024
#define CFG_TUD_MIDI_EP_BUFSIZE     64

#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_VENDOR              0

#ifdef __cplusplus
}
#endif
