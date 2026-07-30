#include "pattern_store.h"

#include "sample_bank.h"
#include "shared_state.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/time.h"

#ifndef XIP_BASE
#define XIP_BASE 0x10000000u
#endif

static constexpr uint32_t kSectorSize = 4096u;
static constexpr uint32_t kPageSize = 256u;

// Packed flash image — must stay within one sector.
struct DrumSeqPatternStore {
    uint32_t magic;
    uint32_t version;
    uint8_t  playing;
    uint8_t  seqLength;
    uint8_t  seqALength;
    uint8_t  seqBLength;
    uint8_t  swing;
    uint8_t  tempoBpm;
    uint8_t  reserved0[2];
    uint8_t  drumHit[DS_DRUM_TRACKS][DS_STEPS];
    uint8_t  seqA_on[DS_STEPS];
    uint8_t  seqA_note[DS_STEPS];
    uint8_t  seqB_on[DS_STEPS];
    uint8_t  seqB_note[DS_STEPS];
    uint8_t  drumLevel[DS_DRUM_TRACKS];
    uint8_t  drumPitch[DS_DRUM_TRACKS];
};

static_assert(sizeof(DrumSeqPatternStore) <= kSectorSize,
              "Pattern store must fit in one flash sector");

static const DrumSeqPatternStore* flash_store(void)
{
    return reinterpret_cast<const DrumSeqPatternStore*>(XIP_BASE + DS_PATTERN_FLASH_OFFSET);
}

static bool store_valid(const DrumSeqPatternStore& s)
{
    if (s.magic != DS_PATTERN_MAGIC || s.version != DS_PATTERN_VERSION) return false;
    if (s.seqLength < 1 || s.seqLength > DS_STEPS) return false;
    if (s.seqALength < 1 || s.seqALength > DS_STEPS) return false;
    if (s.seqBLength < 1 || s.seqBLength > DS_STEPS) return false;
    if (s.tempoBpm < 40 || s.tempoBpm > 240) return false;
    if (s.swing > 127) return false;
    return true;
}

bool drum_seq_pattern_load(void)
{
    const DrumSeqPatternStore& s = *flash_store();
    if (!store_valid(s)) return false;

    gState.playing    = s.playing ? 1 : 0;
    gState.seqLength  = s.seqLength;
    gState.seqALength = s.seqALength;
    gState.seqBLength = s.seqBLength;
    gState.swing      = s.swing;
    gState.tempoBpm   = s.tempoBpm;
    gState.currentStep = 0;
    gState.seqAStep   = 0;
    gState.seqBStep   = 0;

    memcpy((void*)gState.drumHit,   s.drumHit,   sizeof(gState.drumHit));
    memcpy((void*)gState.seqA_on,   s.seqA_on,   sizeof(gState.seqA_on));
    memcpy((void*)gState.seqA_note, s.seqA_note, sizeof(gState.seqA_note));
    memcpy((void*)gState.seqB_on,   s.seqB_on,   sizeof(gState.seqB_on));
    memcpy((void*)gState.seqB_note, s.seqB_note, sizeof(gState.seqB_note));
    memcpy((void*)gState.drumLevel, s.drumLevel, sizeof(gState.drumLevel));
    memcpy((void*)gState.drumPitch, s.drumPitch, sizeof(gState.drumPitch));
    return true;
}

bool drum_seq_pattern_save(void)
{
    DrumSeqPatternStore store = {};
    store.magic       = DS_PATTERN_MAGIC;
    store.version     = DS_PATTERN_VERSION;
    store.playing     = gState.playing ? 1 : 0;
    store.seqLength   = gState.seqLength;
    store.seqALength  = gState.seqALength;
    store.seqBLength  = gState.seqBLength;
    store.swing       = gState.swing;
    store.tempoBpm    = gState.tempoBpm;
    memcpy(store.drumHit,   (const void*)gState.drumHit,   sizeof(store.drumHit));
    memcpy(store.seqA_on,   (const void*)gState.seqA_on,   sizeof(store.seqA_on));
    memcpy(store.seqA_note, (const void*)gState.seqA_note, sizeof(store.seqA_note));
    memcpy(store.seqB_on,   (const void*)gState.seqB_on,   sizeof(store.seqB_on));
    memcpy(store.seqB_note, (const void*)gState.seqB_note, sizeof(store.seqB_note));
    memcpy(store.drumLevel, (const void*)gState.drumLevel, sizeof(store.drumLevel));
    memcpy(store.drumPitch, (const void*)gState.drumPitch, sizeof(store.drumPitch));

    if (!store_valid(store)) return false;

    // Mute Core 0 while erasing/programming (same flag as sample upload).
    const uint8_t was_playing = gState.playing;
    drum_seq_sample_bank_set_mutating(true);
    gState.playing = 0;
    sleep_ms(2);

    uint8_t sector[kSectorSize];
    memset(sector, 0xff, sizeof(sector));
    memcpy(sector, &store, sizeof(store));

    flash_range_erase(DS_PATTERN_FLASH_OFFSET, kSectorSize);
    for (uint32_t off = 0; off < kSectorSize; off += kPageSize) {
        flash_range_program(DS_PATTERN_FLASH_OFFSET + off, sector + off, kPageSize);
    }
    flash_flush_cache();

    drum_seq_sample_bank_set_mutating(false);
    gState.playing = was_playing;
    return true;
}
