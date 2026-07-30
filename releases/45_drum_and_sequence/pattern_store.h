#pragma once

#include <stdbool.h>
#include <stdint.h>

// Pattern persistence — one flash sector just below the sample bank.
// Offset: DS_FIRMWARE_RESERVE - 4096 (= 0x3F000 with 256 KB reserve).

#ifndef DS_FIRMWARE_RESERVE
#define DS_FIRMWARE_RESERVE (256u * 1024u)
#endif

static constexpr uint32_t DS_PATTERN_FLASH_OFFSET = DS_FIRMWARE_RESERVE - 4096u;
static constexpr uint32_t DS_PATTERN_MAGIC = 0x74715344u;  // "DSpt" LE
static constexpr uint32_t DS_PATTERN_VERSION = 1u;

// Load pattern from flash into gState. Returns true if a valid store was applied.
// On failure, leaves gState unchanged (caller should init defaults first).
bool drum_seq_pattern_load(void);

// Write current gState pattern/transport fields to flash. Call from Core 1 only.
bool drum_seq_pattern_save(void);
