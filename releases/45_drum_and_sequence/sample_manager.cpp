// Web Serial sample bank loader (Core 1).
//
// CDC sample protocol:
//   X  → "SYNC\n"                        handshake
//   I  → u32 len + ASCII info string     flash size / capacity / used
//   R  → stream full bank (no ACKs)      legacy / diagnostics
//   S  → ASCII '0'..'7' → u32 frames + PCM in 512-byte chunks (host ACKs 'A') + "DONE\n"
//   W  → u32 total_len, "OK\n", then header+audio pages, final "OK\n"
//   E  → erase bank header sector, "OK\n"
//
// Write order: validate header in RAM → erase/program audio → program header
// last (so a failed upload does not leave a half-valid magic). After write,
// apply the RAM header and rebuild the voice cache before clearing
// sample_bank_mutating so Core 0 never plays from flash during the transfer.

#include "sample_manager.h"

#include "sample_bank.h"
#include "voice_cache.h"
#include "shared_state.h"

#include <string.h>
#include <stdio.h>

#include "hardware/flash.h"
#include "pico/time.h"
#include "tusb.h"

#ifndef XIP_BASE
#define XIP_BASE 0x10000000u
#endif

static constexpr uint8_t kSampleCdcItf = 0u;
static constexpr uint32_t kFlashSectorSize = 4096u;
static constexpr uint32_t kFlashPageSize = 256u;
static constexpr uint32_t kReadChunkSize = 1024u;
static constexpr uint32_t kSlotChunkSize = 512u;
static constexpr uint32_t kWriteTimeoutMs = 5000u;

// Header is staged in RAM so we can validate before touching flash, then
// program it after the audio payload.
static uint8_t header_staging[DS_BANK_HEADER_SIZE] __attribute__((aligned(4)));
static uint8_t page_buf[kFlashPageSize] __attribute__((aligned(4)));

// ── CDC helpers ───────────────────────────────────────────────

static bool cdc_connected(void) {
    return tud_cdc_n_connected(kSampleCdcItf);
}

static bool cdc_write_all(const void* data, uint32_t len) {
    if (!cdc_connected()) return false;

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t sent = 0;
    absolute_time_t deadline = make_timeout_time_ms(3000);

    while (sent < len && cdc_connected() && !time_reached(deadline)) {
        uint32_t avail = tud_cdc_n_write_available(kSampleCdcItf);
        if (avail == 0) {
            tud_cdc_n_write_flush(kSampleCdcItf);
            tud_task();
            continue;
        }
        uint32_t chunk = len - sent;
        if (chunk > avail) chunk = avail;
        uint32_t written = tud_cdc_n_write(kSampleCdcItf, bytes + sent, chunk);
        if (written == 0) {
            tud_cdc_n_write_flush(kSampleCdcItf);
            tud_task();
            continue;
        }
        sent += written;
        // Progress resets the stall timer so paced transfers can take longer.
        deadline = make_timeout_time_ms(3000);
        tud_cdc_n_write_flush(kSampleCdcItf);
        tud_task();
    }
    tud_cdc_n_write_flush(kSampleCdcItf);
    return sent == len;
}

static bool cdc_write_str(const char* text) {
    return cdc_write_all(text, (uint32_t)strlen(text));
}

static bool cdc_read_exact(uint8_t* dst, uint32_t len, uint32_t timeout_ms) {
    uint32_t got = 0;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (got < len) {
        if (time_reached(deadline)) return false;
        uint32_t avail = tud_cdc_n_available(kSampleCdcItf);
        if (avail == 0) {
            tud_task();
            continue;
        }
        uint32_t chunk = len - got;
        if (chunk > avail) chunk = avail;
        uint32_t n = tud_cdc_n_read(kSampleCdcItf, dst + got, chunk);
        if (n == 0) {
            tud_task();
            continue;
        }
        got += n;
        // Refresh deadline while data is flowing.
        deadline = make_timeout_time_ms(timeout_ms);
    }
    return true;
}

static void send_sync(void) {
    cdc_write_str("SYNC\n");
}

static bool validate_header(const DrumSeqSampleBank& header, uint32_t total_len) {
    if (total_len < DS_BANK_HEADER_SIZE) return false;
    const uint32_t audio_len = total_len - DS_BANK_HEADER_SIZE;
    if (header.magic != DS_BANK_MAGIC ||
        header.version != DS_BANK_VERSION ||
        header.header_size != DS_BANK_HEADER_SIZE ||
        header.sample_rate != DS_BANK_SAMPLE_RATE ||
        header.sample_count != DS_BANK_MAX_SAMPLES ||
        header.audio_bytes != audio_len ||
        header.audio_bytes > drum_seq_audio_capacity_bytes() ||
        header.capacity_bytes > drum_seq_audio_capacity_bytes()) {
        return false;
    }
    for (uint32_t i = 0; i < header.sample_count && i < DS_BANK_MAX_SAMPLES; ++i) {
        const DrumSeqSampleRecord& record = header.samples[i];
        if (record.frame_count == 0) {
            continue;
        }
        if (record.source_bpm == 0 ||
            record.offset > header.audio_bytes ||
            record.frame_count > header.audio_bytes - record.offset) {
            return false;
        }
    }
    return header.sample_count <= DS_BANK_MAX_SAMPLES;
}

static void handle_info(void) {
    // Length-prefixed ASCII metadata for the Samples tab.
    // F=flash, R=reserve, A=audio offset, C=capacity, U=used, SR=rate, N=slots
    // SLOTS f0 f1 … f7 = per-voice frame counts (0 = empty)
    char info[384];
    int n = snprintf(info, sizeof(info),
        "DRUMSEQ1 FW 1.0 F %lu R %lu A %lu C %lu U %lu SR %lu N %lu\nSLOTS",
        static_cast<unsigned long>(drum_seq_flash_total_bytes()),
        static_cast<unsigned long>(DS_FIRMWARE_RESERVE),
        static_cast<unsigned long>(drum_seq_audio_flash_offset()),
        static_cast<unsigned long>(drum_seq_audio_capacity_bytes()),
        static_cast<unsigned long>(drum_seq_audio_audio_bytes()),
        static_cast<unsigned long>(DS_BANK_SAMPLE_RATE),
        static_cast<unsigned long>(drum_seq_audio_sample_count()));
    if (n <= 0 || n >= (int)sizeof(info)) return;

    for (uint32_t i = 0; i < DS_BANK_MAX_SAMPLES; ++i) {
        const int added = snprintf(
            info + n, sizeof(info) - static_cast<size_t>(n), " %lu",
            static_cast<unsigned long>(drum_seq_voice_cache_length(i)));
        if (added <= 0 || n + added >= (int)sizeof(info)) return;
        n += added;
    }
    const int tail = snprintf(info + n, sizeof(info) - static_cast<size_t>(n), "\nEND\n");
    if (tail <= 0 || n + tail >= (int)sizeof(info)) return;
    n += tail;

    uint32_t used = static_cast<uint32_t>(n);
    cdc_write_all(&used, sizeof(used));
    cdc_write_all(info, used);
}

static void handle_read(void) {
    if (!drum_seq_sample_bank_valid()) {
        uint32_t zero = 0;
        cdc_write_all(&zero, sizeof(zero));
        return;
    }

    // Stream without per-chunk ACKs. Drain any inbound CDC bytes while
    // sending so leftover host ACKs cannot fill the RX FIFO and stall USB.
    const uint32_t total_len = DS_BANK_HEADER_SIZE + drum_seq_audio_audio_bytes();
    drum_seq_sample_bank_set_mutating(true);
    gState.playing = 0;
    if (!cdc_write_all(&total_len, sizeof(total_len))) {
        drum_seq_sample_bank_set_mutating(false);
        return;
    }

    const uint8_t* src = reinterpret_cast<const uint8_t*>(XIP_BASE + DS_AUDIO_FLASH_OFFSET);
    uint32_t sent = 0;
    while (sent < total_len) {
        while (tud_cdc_n_available(kSampleCdcItf)) {
            uint8_t discard;
            tud_cdc_n_read(kSampleCdcItf, &discard, 1);
        }
        uint32_t chunk = total_len - sent;
        // Small chunks keep the CDC TX FIFO from staying full for long.
        if (chunk > 256u) chunk = 256u;
        if (!cdc_write_all(src + sent, chunk)) {
            drum_seq_sample_bank_set_mutating(false);
            send_sync();
            return;
        }
        sent += chunk;
        tud_task();
    }
    drum_seq_sample_bank_set_mutating(false);
    cdc_write_str("DONE\n");
}

static void handle_slot_read(void) {
    // Host: 'S' + ASCII slot '0'..'7' (avoid a raw 0x00 byte — some serial
    // paths drop NULs). Also accept binary 0..7. Reply from RAM voice cache
    // in 512-byte chunks with host 'A' ACKs so we never fill the 1 KB CDC TX
    // buffer (blind streaming stalls around exactly that size).
    uint8_t slot_byte = 0;
    if (!cdc_read_exact(&slot_byte, 1, kWriteTimeoutMs)) {
        cdc_write_str("TIMEOUT\n");
        return;
    }

    uint8_t slot = 0xff;
    if (slot_byte >= '0' && slot_byte <= '7') {
        slot = static_cast<uint8_t>(slot_byte - '0');
    } else if (slot_byte < DS_BANK_MAX_SAMPLES) {
        slot = slot_byte;
    }

    uint32_t frames = 0;
    const int8_t* pcm = nullptr;
    if (slot < DS_BANK_MAX_SAMPLES) {
        frames = drum_seq_voice_cache_length(slot);
        pcm = drum_seq_voice_cache_pcm(slot);
    }

    if (!cdc_write_all(&frames, sizeof(frames))) return;
    if (frames == 0 || pcm == nullptr) {
        cdc_write_str("DONE\n");
        return;
    }

    uint32_t sent = 0;
    while (sent < frames) {
        uint32_t chunk = frames - sent;
        if (chunk > kSlotChunkSize) chunk = kSlotChunkSize;
        if (!cdc_write_all(reinterpret_cast<const uint8_t*>(pcm + sent), chunk)) {
            send_sync();
            return;
        }
        sent += chunk;
        if (sent < frames) {
            // Do not discard inbound here — the host ACK may already be queued.
            uint8_t ack = 0;
            if (!cdc_read_exact(&ack, 1, kWriteTimeoutMs) || ack != 'A') {
                cdc_write_str("TIMEOUT\n");
                return;
            }
        }
        tud_task();
    }
    cdc_write_str("DONE\n");
}

static void drain_rejected_write(uint32_t remaining) {
    uint8_t scratch[32];
    while (remaining > 0) {
        uint32_t chunk = remaining < sizeof(scratch) ? remaining : sizeof(scratch);
        if (!cdc_read_exact(scratch, chunk, kWriteTimeoutMs)) return;
        remaining -= chunk;
    }
}

static void handle_write(void) {
    // Host sends: W + u32 LE total_len + [header 4096][audio…]
    // We ACK with OK after accepting the length, then again when flash is done.
    uint8_t len_buf[4];
    if (!cdc_read_exact(len_buf, sizeof(len_buf), kWriteTimeoutMs)) {
        cdc_write_str("TIMEOUT\n");
        return;
    }

    uint32_t total_len = 0;
    memcpy(&total_len, len_buf, sizeof(total_len));

    if (total_len < DS_BANK_HEADER_SIZE ||
        total_len > DS_BANK_HEADER_SIZE + drum_seq_audio_capacity_bytes()) {
        cdc_write_str("ERR\n");
        drain_rejected_write(total_len);
        return;
    }

    if (!cdc_write_str("OK\n")) {
        drain_rejected_write(total_len);
        return;
    }

    if (!cdc_read_exact(header_staging, DS_BANK_HEADER_SIZE, kWriteTimeoutMs)) {
        cdc_write_str("TIMEOUT\n");
        return;
    }

    const DrumSeqSampleBank* header =
        reinterpret_cast<const DrumSeqSampleBank*>(header_staging);
    if (!validate_header(*header, total_len)) {
        cdc_write_str("ERR\n");
        drain_rejected_write(total_len - DS_BANK_HEADER_SIZE);
        return;
    }

    // No multicore_lockout here: USB IRQs live on Core 0, and parking it
    // deadlocks CDC RX mid-upload. Firmware is copy_to_ram; Core 0 only
    // mutes when sample_bank_mutating is set, and never reads flash during playback.
    drum_seq_sample_bank_set_mutating(true);
    gState.playing = 0;
    sleep_ms(2);

    flash_range_erase(DS_AUDIO_FLASH_OFFSET, kFlashSectorSize);

    // Stream audio pages first (header sector stays erased until the end).
    uint32_t bytes_written = DS_BANK_HEADER_SIZE;
    uint32_t audio_flash_off = DS_AUDIO_FLASH_OFFSET + DS_BANK_HEADER_SIZE;
    uint32_t next_erase = audio_flash_off;

    while (bytes_written < total_len) {
        uint32_t remaining = total_len - bytes_written;
        uint32_t page_fill = remaining < kFlashPageSize ? remaining : kFlashPageSize;
        memset(page_buf, 0xff, sizeof(page_buf));
        if (!cdc_read_exact(page_buf, page_fill, kWriteTimeoutMs)) {
            drum_seq_sample_bank_rescan();
            drum_seq_voice_cache_rebuild();
            drum_seq_sample_bank_set_mutating(false);
            cdc_write_str("TIMEOUT\n");
            return;
        }

        uint32_t page_off = audio_flash_off + (bytes_written - DS_BANK_HEADER_SIZE);
        if (page_off >= next_erase) {
            flash_range_erase(next_erase, kFlashSectorSize);
            next_erase += kFlashSectorSize;
        }
        flash_range_program(page_off, page_buf, sizeof(page_buf));
        bytes_written += page_fill;
        tud_task();
    }

    memset(page_buf, 0xff, sizeof(page_buf));
    for (uint32_t offset = 0; offset < DS_BANK_HEADER_SIZE; offset += kFlashPageSize) {
        memcpy(page_buf, header_staging + offset, kFlashPageSize);
        flash_range_program(DS_AUDIO_FLASH_OFFSET + offset, page_buf, sizeof(page_buf));
        tud_task();
    }

    flash_flush_cache();
    // Prefer the RAM header we just validated — avoids any XIP stale-read edge cases.
    if (!drum_seq_sample_bank_apply_header(*header)) {
        drum_seq_sample_bank_rescan();
    }
    drum_seq_voice_cache_rebuild();
    drum_seq_sample_bank_set_mutating(false);
    cdc_write_str("OK\n");
    tud_cdc_n_write_flush(kSampleCdcItf);
}

static void handle_erase(void) {
    drum_seq_sample_bank_set_mutating(true);
    gState.playing = 0;
    sleep_ms(2);
    flash_range_erase(DS_AUDIO_FLASH_OFFSET, kFlashSectorSize);
    drum_seq_sample_bank_rescan();
    drum_seq_voice_cache_rebuild();
    drum_seq_sample_bank_set_mutating(false);
    cdc_write_str("OK\n");
}

void drum_seq_sample_manager_task(void) {
    // Non-blocking: one command per available CDC byte when connected.
    if (!cdc_connected()) return;

    while (tud_cdc_n_available(kSampleCdcItf)) {
        uint8_t cmd;
        if (tud_cdc_n_read(kSampleCdcItf, &cmd, 1) != 1) break;
        switch (cmd) {
        case 'X':
            send_sync();
            break;
        case 'I':
            handle_info();
            break;
        case 'R':
            handle_read();
            break;
        case 'S':
            handle_slot_read();
            break;
        case 'W':
            handle_write();
            break;
        case 'E':
            handle_erase();
            break;
        default:
            break;
        }
    }
}
