#include "sample_bank.h"

#include <string.h>

#include "hardware/flash.h"

#ifndef XIP_BASE
#define XIP_BASE 0x10000000u
#endif

// Flash bank metadata and XIP helpers.
// Audio PCM starts at DS_AUDIO_FLASH_OFFSET + DS_BANK_HEADER_SIZE.
// Playback must not call drum_seq_read_byte from the audio ISR while
// sample_bank_mutating is set — use the RAM voice cache instead.

namespace {

DrumSeqSample samples[DS_BANK_MAX_SAMPLES];
DrumSeqSample empty_sample = {0, 0, 120, 0, 0, "empty"};
uint32_t sample_count = 0;
uint32_t audio_bytes = 0;
uint32_t flash_total_bytes = DS_COMPILED_FLASH_TOTAL_BYTES;
uint32_t audio_capacity_bytes = 0;
bool sample_bank_valid = false;
volatile bool sample_bank_mutating = false;   // Core 0 polls this to mute during upload

const DrumSeqSampleBank* flash_header() {
    return reinterpret_cast<const DrumSeqSampleBank*>(XIP_BASE + DS_AUDIO_FLASH_OFFSET);
}

uint32_t capacity_from_flash_size(uint32_t flash_bytes) {
    if (flash_bytes <= DS_AUDIO_FLASH_OFFSET + DS_BANK_HEADER_SIZE) {
        return 0u;
    }
    return flash_bytes - DS_AUDIO_FLASH_OFFSET - DS_BANK_HEADER_SIZE;
}

// JEDEC RDID (0x9F) → capacity code in rx[3]. Falls back to compiled size.
uint32_t detect_flash_total_bytes() {
    uint8_t txbuf[4] = {0x9fu, 0u, 0u, 0u};
    uint8_t rxbuf[4] = {0u, 0u, 0u, 0u};
    flash_do_cmd(txbuf, rxbuf, sizeof(txbuf));

    const uint8_t capacity_code = rxbuf[3];
    if (rxbuf[1] == 0 || rxbuf[1] == 0xff || capacity_code < 16u ||
        capacity_code > 31u) {
        return DS_COMPILED_FLASH_TOTAL_BYTES;
    }
    return 1u << capacity_code;
}

bool record_valid(const DrumSeqSampleRecord& record, uint32_t total_audio_bytes) {
    if (record.frame_count == 0) {
        return true;
    }
    if (record.source_bpm == 0) {
        return false;
    }
    if (record.offset > total_audio_bytes) {
        return false;
    }
    return record.frame_count <= total_audio_bytes - record.offset;
}

void sanitize_name(char* name, uint32_t len) {
    name[len - 1u] = '\0';
    for (uint32_t j = 0; j < len - 1u; ++j) {
        unsigned char value = static_cast<unsigned char>(name[j]);
        if (value == 0 || value == 0xff) {
            name[j] = '\0';
            break;
        }
        if (value < 0x20 || value > 0x7e) {
            name[j] = '_';
        }
    }
}

bool header_fields_ok(const DrumSeqSampleBank& header) {
    return header.magic == DS_BANK_MAGIC &&
           header.version == DS_BANK_VERSION &&
           header.header_size == DS_BANK_HEADER_SIZE &&
           header.sample_rate == DS_BANK_SAMPLE_RATE &&
           header.sample_count == DS_BANK_MAX_SAMPLES &&
           header.audio_bytes <= audio_capacity_bytes &&
           header.capacity_bytes <= audio_capacity_bytes;
}

bool load_samples_from_header(const DrumSeqSampleBank& header) {
    if (!header_fields_ok(header)) {
        return false;
    }

    DrumSeqSample loaded[DS_BANK_MAX_SAMPLES];
    memset(loaded, 0, sizeof(loaded));

    for (uint32_t i = 0; i < DS_BANK_MAX_SAMPLES; ++i) {
        const DrumSeqSampleRecord& record = header.samples[i];
        if (!record_valid(record, header.audio_bytes)) {
            return false;
        }

        loaded[i].offset = record.offset;
        loaded[i].frame_count = record.frame_count;
        loaded[i].source_bpm = record.source_bpm;
        loaded[i].peak = record.peak;
        loaded[i].flags = record.flags;
        memcpy(loaded[i].name, record.name, sizeof(loaded[i].name));
        sanitize_name(loaded[i].name, sizeof(loaded[i].name));
    }

    memcpy(samples, loaded, sizeof(samples));
    sample_count = DS_BANK_MAX_SAMPLES;
    audio_bytes = header.audio_bytes;
    sample_bank_valid = true;
    return true;
}

}  // namespace

void drum_seq_sample_bank_init(void) {
    flash_total_bytes = detect_flash_total_bytes();
    audio_capacity_bytes = capacity_from_flash_size(flash_total_bytes);
    drum_seq_sample_bank_rescan();
}

void drum_seq_sample_bank_rescan(void) {
    // Re-read the on-flash header (boot, erase, or fallback after apply fails).
    sample_bank_valid = false;
    sample_count = 0;
    audio_bytes = 0;
    memset(samples, 0, sizeof(samples));
    flash_flush_cache();
    load_samples_from_header(*flash_header());
}

bool drum_seq_sample_bank_apply_header(const DrumSeqSampleBank& header) {
    // Prefer this after a successful upload: metadata comes from the RAM
    // staging buffer we already validated, not a fresh XIP read.
    sample_bank_valid = false;
    sample_count = 0;
    audio_bytes = 0;
    memset(samples, 0, sizeof(samples));
    return load_samples_from_header(header);
}

bool drum_seq_sample_bank_valid(void) {
    return sample_bank_valid;
}

bool drum_seq_sample_bank_mutating(void) {
    return sample_bank_mutating;
}

void drum_seq_sample_bank_set_mutating(bool mutating) {
    sample_bank_mutating = mutating;
    __asm volatile("dmb" ::: "memory");
}

uint32_t drum_seq_audio_sample_count(void) {
    return sample_bank_valid ? sample_count : 0u;
}

uint32_t drum_seq_audio_audio_bytes(void) {
    return sample_bank_valid ? audio_bytes : 0u;
}

uint32_t drum_seq_audio_capacity_bytes(void) {
    return audio_capacity_bytes;
}

uint32_t drum_seq_flash_total_bytes(void) {
    return flash_total_bytes;
}

uint32_t drum_seq_audio_flash_offset(void) {
    return DS_AUDIO_FLASH_OFFSET;
}

const DrumSeqSample& drum_seq_sample(uint32_t index) {
    if (!sample_bank_valid || index >= DS_BANK_MAX_SAMPLES) {
        return empty_sample;
    }
    return samples[index];
}

uint8_t drum_seq_read_byte(uint32_t offset) {
    // Byte within the audio PCM region (not the header). Safe only when
    // sample_bank_mutating is false — intended for voice_cache_rebuild on Core 1.
    const uint8_t* data = reinterpret_cast<const uint8_t*>(
        XIP_BASE + DS_AUDIO_FLASH_OFFSET + DS_BANK_HEADER_SIZE);
    return data[offset];
}
