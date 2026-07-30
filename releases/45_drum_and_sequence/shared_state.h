#pragma once

#include <stdint.h>

// Number of steps in the drum grid and melodic sequencers.
constexpr int DS_STEPS = 16;
constexpr int DS_DRUM_TRACKS = 8;

// Cross-core shared state for Drum&Sequence.
//
// Core 0  — ProcessSample reads/writes continuously (audio + panel).
// Core 1  — USB SysEx / CDC sample manager reads and occasionally writes.
//
// Single-byte fields are naturally atomic on Cortex-M0+. tickEpoch is the
// cross-core signal for "a step just advanced" (Core 1 compares against its
// mirror and sends a TICK SysEx to the browser).

struct SharedState {
    // Transport — one shared clock; drums use currentStep, melodic seqs
    // may use independent lengths via seqAStep / seqBStep.
    uint8_t  playing;       // 0 = paused, 1 = running
    uint8_t  currentStep;   // 0..15 drum / bar playback position
    uint8_t  seqLength;     // 1..16 active steps for the shared bar
    uint8_t  swing;         // 0..127 timing shuffle (64 = straight)
    uint8_t  tempoBpm;      // 40..240 internal clock BPM (16th notes)
    uint32_t tickEpoch;     // ++ on each step advance / reset

    uint8_t  seqAStep;      // melodic A playhead (may wrap sooner than drums)
    uint8_t  seqBStep;      // melodic B playhead

    // Drum grid: velocity 0 = off, 1..127 = hit strength.
    uint8_t drumHit[DS_DRUM_TRACKS][DS_STEPS];

    // Melodic sequencer A → CV Out 1 + Pulse Out 1
    uint8_t seqA_on[DS_STEPS];    // 0/1 gate
    uint8_t seqA_note[DS_STEPS];  // MIDI note 0..127

    // Melodic sequencer B → CV Out 2 + Pulse Out 2
    uint8_t seqB_on[DS_STEPS];
    uint8_t seqB_note[DS_STEPS];

    uint8_t seqALength;     // 1..16 independent loop length
    uint8_t seqBLength;

    // Panel edit state (switch Down mode)
    uint8_t editTarget;     // 1 = seq A, 2 = seq B
    uint8_t editStep;       // 0..15 cursor
    uint8_t selectedDrum;   // 0..7 (switch Middle)

    // Per-voice drum params (pitch centre 64 = 1.0× playback rate)
    uint8_t drumLevel[DS_DRUM_TRACKS];   // 0..127
    uint8_t drumPitch[DS_DRUM_TRACKS];   // 0..127
};

extern volatile SharedState gState;

void drum_seq_init_state(void);
