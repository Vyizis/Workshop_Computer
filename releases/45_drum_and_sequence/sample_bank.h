#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shared_state.h"

// Flash-resident drum sample bank.
//
// Layout (Drum&Sequence sample bank, magic "DSbk"):
//   [0 .. 4095]     DrumSeqSampleBank (one flash sector)
//   [4096 .. ]      packed 48 kHz mono signed 8-bit PCM for up to 8 slots
//
// Bank starts at DS_FIRMWARE_RESERVE (256 KB) so UF2 firmware never overlaps.
// Web encoder (web/index.html) must keep DS_BANK_* constants in lockstep with
// the structs and static_asserts below.

static constexpr uint32_t DS_BANK_MAGIC = 0x6b625344u;  // "DSbk" little-endian
static constexpr uint32_t DS_BANK_VERSION = 1u;
static constexpr uint32_t DS_BANK_HEADER_SIZE = 4096u;
static constexpr uint32_t DS_BANK_SAMPLE_RATE = 48000u;
static constexpr uint32_t DS_BANK_MAX_SAMPLES = DS_DRUM_TRACKS;
static constexpr uint32_t DS_FLASH_SECTOR_SIZE = 4096u;

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16u * 1024u * 1024u)
#endif

#ifndef DS_FIRMWARE_RESERVE
#define DS_FIRMWARE_RESERVE (256u * 1024u)
#endif

static constexpr uint32_t DS_COMPILED_FLASH_TOTAL_BYTES =
    static_cast<uint32_t>(PICO_FLASH_SIZE_BYTES);
static constexpr uint32_t DS_AUDIO_FLASH_OFFSET = DS_FIRMWARE_RESERVE;
static constexpr uint32_t DS_AUDIO_CAPACITY_BYTES =
    DS_COMPILED_FLASH_TOTAL_BYTES - DS_AUDIO_FLASH_OFFSET - DS_BANK_HEADER_SIZE;

// Runtime view of one slot (copied out of the flash header on rescan/apply).
struct DrumSeqSample {
    uint32_t offset;       // byte offset into PCM region
    uint32_t frame_count;  // 8-bit frames (= bytes)
    uint16_t source_bpm;
    uint8_t peak;
    uint8_t flags;
    char name[48];
};

// On-flash record — must stay 60 bytes (matches web DS_BANK_RECORD_SIZE).
struct DrumSeqSampleRecord {
    uint32_t offset;
    uint32_t frame_count;
    uint16_t source_bpm;
    uint8_t peak;
    uint8_t flags;
    char name[48];
};

struct DrumSeqSampleBank {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t sample_rate;
    uint32_t sample_count;
    uint32_t audio_bytes;
    uint32_t capacity_bytes;
    uint32_t reserved0;
    DrumSeqSampleRecord samples[DS_BANK_MAX_SAMPLES];  // starts at byte 32
};

static_assert(sizeof(DrumSeqSampleRecord) == 60,
              "Sample record must match web DS_BANK_RECORD_SIZE");
static_assert(sizeof(DrumSeqSampleBank) <= DS_BANK_HEADER_SIZE,
              "Drum&Sequence bank header must fit in one flash sector");
static_assert(offsetof(DrumSeqSampleBank, samples) == 32,
              "Sample records must start at byte 32");

void drum_seq_sample_bank_init(void);
void drum_seq_sample_bank_rescan(void);
bool drum_seq_sample_bank_apply_header(const DrumSeqSampleBank& header);
bool drum_seq_sample_bank_valid(void);
bool drum_seq_sample_bank_mutating(void);
void drum_seq_sample_bank_set_mutating(bool mutating);
uint32_t drum_seq_audio_sample_count(void);
uint32_t drum_seq_audio_audio_bytes(void);
uint32_t drum_seq_audio_capacity_bytes(void);
uint32_t drum_seq_flash_total_bytes(void);
uint32_t drum_seq_audio_flash_offset(void);
const DrumSeqSample& drum_seq_sample(uint32_t index);
uint8_t drum_seq_read_byte(uint32_t offset);
