#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// SysEx protocol for the Pattern tab (WebMIDI).
// Envelope: F0 7D <cmd> <payload...> F7
// Manufacturer 0x7D is the Workshop Computer educational/DIY ID.

#define DS_SYSEX_MFR               0x7D

// ── Browser → card ────────────────────────────────────────────
#define DS_SYSEX_SET_DRUM_STEP     0x01  // track, step, velocity
#define DS_SYSEX_SET_SEQ_STEP      0x02  // seq(0=A,1=B), step, on, note
#define DS_SYSEX_SET_LENGTH        0x03  // target(0=bar,1=A,2=B), length 1..16
#define DS_SYSEX_SET_PLAYING       0x04  // playing 0/1
#define DS_SYSEX_REQUEST_DUMP      0x05  // empty → card replies FULL_DUMP
#define DS_SYSEX_INTERFACE_VERSION 0x06  // major, minor, patch → FW ver + dump
#define DS_SYSEX_SET_TEMPO         0x07  // tempoHi (bpm>>7), tempoLo (bpm&0x7F)
#define DS_SYSEX_SET_SWING         0x08  // swing 0..127 (64 = straight)
#define DS_SYSEX_SAVE_TO_FLASH     0x20  // persist pattern → SAVE_OK / SAVE_ERR

// ── Card → browser ────────────────────────────────────────────
#define DS_SYSEX_FULL_DUMP         0x10  // see send_full_dump() for layout
#define DS_SYSEX_TICK              0x11  // drumStep, seqAStep, seqBStep
#define DS_SYSEX_PARAM_UPDATE      0x12  // then a SET_* cmd + same payload
#define DS_SYSEX_FIRMWARE_VERSION  0x13  // major, minor, patch
#define DS_SYSEX_SAVE_OK           0x21  // pattern written to flash
#define DS_SYSEX_SAVE_ERR          0x22  // pattern save failed

// Poll from Core 1: dump on connect, ticks, param mirror, inbound SysEx.
void midi_device_task(void);

#ifdef __cplusplus
}
#endif
