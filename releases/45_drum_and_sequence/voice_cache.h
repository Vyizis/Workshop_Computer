#pragma once

#include <stdint.h>

#include "shared_state.h"

// RAM sample cache for drum playback.
//
// Why: the RP2040 cannot safely XIP-read flash from the audio ISR while
// Core 1 is erasing/programming the bank. After each upload (and at boot)
// we copy up to DS_VOICE_CACHE_FRAMES of 8-bit PCM per voice into RAM.
// ProcessSample only reads this cache.

// ~250 ms at 48 kHz — enough for most one-shot drums; web UI trims to match.
static constexpr uint32_t DS_VOICE_CACHE_FRAMES = 12000u;

void drum_seq_voice_cache_rebuild(void);
uint32_t drum_seq_voice_cache_length(uint32_t track);
const int8_t* drum_seq_voice_cache_pcm(uint32_t track);
int8_t drum_seq_voice_cache_sample(uint32_t track, uint32_t frame);
