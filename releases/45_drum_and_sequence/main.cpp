/*
 * Drum&Sequence — sample drum machine + dual melodic sequencers
 * ==============================================================
 * Program card #45 for the Music Thing Modular Workshop Computer.
 *
 * Architecture
 * ------------
 * Core 0  ProcessSample() — audio, sequencers, panel UI (~20 µs budget)
 * Core 1  USB — WebMIDI SysEx (pattern editor) + CDC (sample bank upload)
 *
 * Samples live in flash after the 256 KB firmware reserve, but playback
 * never reads flash from the audio ISR: after each upload, samples are
 * copied into a RAM voice cache (see voice_cache.h).
 *
 * I/O map
 * -------
 * Audio Out 1/2  — drum mix (48 kHz mono 8-bit samples from RAM cache)
 * CV Out 1       — Sequencer A pitch (calibrated 1V/oct)
 * Pulse Out 1    — Sequencer A gate
 * CV Out 2       — Sequencer B pitch (calibrated 1V/oct)
 * Pulse Out 2    — Sequencer B gate
 * Pulse In 1     — External clock (one rising edge = one step)
 * Pulse In 2     — Reset all playheads to step 0
 * CV In 1        — Transpose sequencer A (±24 semitones)
 * CV In 2        — Transpose sequencer B
 *
 * Switch UP      — play: Main=tempo (BPM), X=bar length, Y=swing
 * Switch MIDDLE  — edit: Main=target (voices 1–8, Seq A, Seq B);
 *                  X/Y = pitch/level (drums) or note/gate (seqs)
 * Switch DOWN    — momentary only (springs to Middle):
 *                  short press = advance edit step; long press = play/pause
 */

#include "ComputerCard.h"
#include "shared_state.h"
#include "usb_core1.h"
#include "sample_bank.h"
#include "voice_cache.h"
#include "pattern_store.h"

#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "tusb.h"
#include "bsp/board_api.h"

class DrumAndSequence : public ComputerCard
{
    static constexpr uint32_t BOOT_MUTE_SAMPLES = 7200;      // ~150 ms at 48 kHz
    static constexpr uint32_t LONG_PRESS_SAMPLES = 24000;    // ~0.5 s

    // Internal clock: samples-per-step from tempoBpm + swing (switch Up).
    uint32_t tickCounter  = 0;
    uint32_t ticksPerStep = 6000;   // 120 BPM 16ths at 48 kHz
    uint32_t bootMute     = BOOT_MUTE_SAMPLES;

    static uint32_t ticks_from_bpm(uint8_t bpm)
    {
        // One sequencer step = one 16th note at 48 kHz.
        // ticks = 48000 * 60 / (bpm * 4) = 720000 / bpm
        if (bpm < 40) bpm = 40;
        if (bpm > 240) bpm = 240;
        return 720000u / static_cast<uint32_t>(bpm);
    }

    static uint8_t bpm_from_knob(int32_t raw)
    {
        // Main knob 0..4095 → 40..240 BPM
        int32_t bpm = 40 + ((raw * 200) >> 12);
        if (bpm < 40) bpm = 40;
        if (bpm > 240) bpm = 240;
        return static_cast<uint8_t>(bpm);
    }

    void refresh_ticks_per_step(void)
    {
        uint32_t base = ticks_from_bpm(gState.tempoBpm);
        // swing 64 = straight; 127 ≈ +25% on even→odd, −25% on odd→even
        int32_t swingOff = static_cast<int32_t>(gState.swing) - 64;
        if (swingOff < 0) swingOff = 0;   // only delay even→odd (classic shuffle)
        uint32_t extra = (base * static_cast<uint32_t>(swingOff)) >> 8;  // 0..~base/4
        if ((gState.currentStep & 1) == 0) {
            ticksPerStep = base + extra;
        } else {
            ticksPerStep = (base > extra) ? (base - extra) : (base >> 1);
        }
        if (ticksPerStep < 800u) ticksPerStep = 800u;
    }

    // Melodic gates are latched for the duration of the current step.
    // Pitch CV holds the last note placed on the roll until a new one appears.
    bool gateA = false;
    bool gateB = false;
    uint8_t heldNoteA = 60;
    uint8_t heldNoteB = 48;

    // Switch-Down edge detection for short-press (cursor) vs long-press (play).
    bool switchDownActive = false;
    uint32_t switchDownCount = 0;
    bool longPressHandled = false;

    Switch prevMode = Switch::Up;

    // One-shot drum voice. Samples come from the RAM voice cache;
    // use_noise is retained only for legacy/debug paths (normally unused).
    struct DrumVoice {
        bool     active;
        uint32_t pos_q16;   // fractional sample position (Q16)
        uint32_t rate_q16;  // playback rate; 65536 = 1.0× (pitch centre 64)
        uint32_t length;    // frames in cache for this hit
        uint32_t offset;    // unused once cache is active (legacy flash path)
        int32_t  env;       // velocity gain (samples) or decaying env (noise)
        uint32_t rng;       // xorshift state for noise fallback
        bool     use_noise;
    };
    DrumVoice voices[DS_DRUM_TRACKS] = {};

    // Expand signed 8-bit PCM toward ComputerCard's ±2048 audio range.
    static int16_t __not_in_flash_func(decode_sample_byte)(uint8_t value)
    {
        return static_cast<int16_t>(static_cast<int8_t>(value) * 16);
    }

    uint32_t __not_in_flash_func(xorshift)(uint32_t &state)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    // Start (or retrigger) a drum on `track`. Empty slots stay silent.
    void trigger_drum(int track, int32_t velocity)
    {
        if (track < 0 || track >= DS_DRUM_TRACKS) return;
        int32_t v = velocity;
        if (v < 1) v = 1;
        if (v > 127) v = 127;

        DrumVoice& voice = voices[track];
        voice.env = v << 9;   // fixed gain for sample one-shots
        voice.pos_q16 = 0;
        // drumPitch 64 → rate 65536 (unity). Knob maps 0..127 → Q16 rate.
        voice.rate_q16 = static_cast<uint32_t>(gState.drumPitch[track]) << 10;
        if (voice.rate_q16 == 0) voice.rate_q16 = 65536u;

        uint32_t cached_len = drum_seq_voice_cache_length(static_cast<uint32_t>(track));
        if (cached_len == 0) {
            voice.active = false;
            return;
        }
        voice.active = true;
        voice.use_noise = false;
        voice.length = cached_len;
    }

    // Map bipolar CV (~±5 V as ±2048) to roughly ±24 semitones.
    int32_t transpose_from_cv(int32_t cv)
    {
        return (cv * 3) >> 8;
    }

    uint8_t clamp_note(int32_t note)
    {
        if (note < 0) return 0;
        if (note > 127) return 127;
        return (uint8_t)note;
    }

    // Advance all three playheads together, then fire hits/gates for the
    // new step. tickEpoch notifies Core 1 so the web UI can move playheads.
    void advance_sequencer(void)
    {
        gState.currentStep = (gState.currentStep + 1) % gState.seqLength;
        gState.seqAStep = (gState.seqAStep + 1) % gState.seqALength;
        gState.seqBStep = (gState.seqBStep + 1) % gState.seqBLength;
        gState.tickEpoch++;

        for (int t = 0; t < DS_DRUM_TRACKS; t++) {
            int32_t vel = gState.drumHit[t][gState.currentStep];
            if (vel > 0) {
                int32_t level = (vel * gState.drumLevel[t]) >> 7;
                trigger_drum(t, level);
            }
        }

        gateA = gState.seqA_on[gState.seqAStep] != 0;
        gateB = gState.seqB_on[gState.seqBStep] != 0;
        // Only update held pitch when this step has a note on the roll.
        if (gateA) heldNoteA = gState.seqA_note[gState.seqAStep];
        if (gateB) heldNoteB = gState.seqB_note[gState.seqBStep];
    }

    // LED0 = playing; LED1–3 = binary step nibble; LED4/5 = seq A/B gates.
    void update_leds(Switch sw)
    {
        for (int i = 0; i < 6; i++) LedOff(i);

        LedOn(0, gState.playing != 0);
        LedOn(1, (gState.currentStep & 1) != 0);
        LedOn(2, (gState.currentStep & 2) != 0);
        LedOn(3, (gState.currentStep & 4) != 0);
        LedOn(4, gateA);
        LedOn(5, gateB);

        if (sw == Switch::Middle) {
            // LED1 brightness hints edit target: drums 0–7, brighter for Seq A/B.
            int32_t bright = (gState.editTarget == 0)
                ? (gState.selectedDrum * 512)
                : (gState.editTarget == 1 ? 3072 : 4095);
            LedBrightness(1, bright);
        }
    }

    // Switch Down is momentary on the Workshop Computer ((ON)-OFF-ON):
    // it springs back to Middle. Only short/long press gestures live here —
    // sustained knob editing belongs in Up / Middle.
    void handle_switch_down(void)
    {
        Switch sw = SwitchVal();
        if (sw == Switch::Down) {
            if (!switchDownActive) {
                switchDownActive = true;
                switchDownCount = 0;
                longPressHandled = false;
            }
            switchDownCount++;
            if (switchDownCount >= LONG_PRESS_SAMPLES && !longPressHandled) {
                longPressHandled = true;
                gState.playing = gState.playing ? 0 : 1;
            }
        } else if (switchDownActive) {
            switchDownActive = false;
            if (!longPressHandled) {
                gState.editStep = (gState.editStep + 1) % gState.seqLength;
            }
        }
    }

    // Continuous knob mapping for latched modes (Up / Middle).
    void handle_knobs(Switch sw)
    {
        if (sw == Switch::Up) {
            int32_t mainRaw = KnobVal(Knob::Main);
            gState.tempoBpm = bpm_from_knob(mainRaw);

            int32_t xRaw = KnobVal(Knob::X);
            int32_t len = 1 + ((xRaw * 15) >> 12);
            if (len > DS_STEPS) len = DS_STEPS;
            gState.seqLength = (uint8_t)len;

            int32_t yRaw = KnobVal(Knob::Y);
            gState.swing = (uint8_t)((yRaw * 127) >> 12);
        } else if (sw == Switch::Middle) {
            // Main picks edit target: 0–7 drum voices, 8 = Seq A, 9 = Seq B.
            int32_t mainRaw = KnobVal(Knob::Main);
            int32_t sel = (mainRaw * 9) >> 12; // 0..9
            if (sel > 9) sel = 9;

            if (sel < DS_DRUM_TRACKS) {
                gState.selectedDrum = (uint8_t)sel;
                gState.editTarget = 0;
                int32_t pitch = KnobVal(Knob::X);
                gState.drumPitch[gState.selectedDrum] = (uint8_t)((pitch * 127) >> 12);
                int32_t level = KnobVal(Knob::Y);
                gState.drumLevel[gState.selectedDrum] = (uint8_t)((level * 127) >> 12);
            } else {
                gState.editTarget = (sel == 8) ? 1 : 2;
                uint8_t es = gState.editStep;
                int32_t noteKnob = KnobVal(Knob::X);
                int32_t gateKnob = KnobVal(Knob::Y);
                uint8_t note = (uint8_t)((noteKnob * 127) >> 12);
                uint8_t on = gateKnob > 2048 ? 1 : 0;

                if (gState.editTarget == 1) {
                    gState.seqA_note[es] = note;
                    gState.seqA_on[es] = on;
                } else {
                    gState.seqB_note[es] = note;
                    gState.seqB_on[es] = on;
                }
            }
        }
        // Switch::Down — no knob mapping (momentary gestures only).
    }

public:
    DrumAndSequence()
    {
        sleep_ms(150);   // let supplies settle before touching flash / USB
        drum_seq_init_state();
        // Prefer saved pattern; defaults remain if flash is empty/invalid.
        drum_seq_pattern_load();
        drum_seq_sample_bank_init();
        drum_seq_voice_cache_rebuild();
        refresh_ticks_per_step();

        // Seed held pitch from the first note on each roll (else keep defaults).
        for (int s = 0; s < DS_STEPS; s++) {
            if (gState.seqA_on[s]) {
                heldNoteA = gState.seqA_note[s];
                break;
            }
        }
        for (int s = 0; s < DS_STEPS; s++) {
            if (gState.seqB_on[s]) {
                heldNoteB = gState.seqB_note[s];
                break;
            }
        }
    }

    virtual void ProcessSample() override
    {
        // Quiet boot so DAC/codecs don't click on power-up.
        if (bootMute > 0) {
            bootMute--;
            AudioOut1(0);
            AudioOut2(0);
            PulseOut1(false);
            PulseOut2(false);
            CVOut1(0);
            CVOut2(0);
            return;
        }

        // Core 1 is erasing/programming flash — mute and skip XIP-sensitive work.
        if (drum_seq_sample_bank_mutating()) {
            for (int t = 0; t < DS_DRUM_TRACKS; t++) voices[t].active = false;
            AudioOut1(0);
            AudioOut2(0);
            return;
        }

        Switch sw = SwitchVal();
        handle_switch_down();
        handle_knobs(sw);
        prevMode = sw;

        // Reset to step 0 on all three sequencers.
        if (PulseIn2RisingEdge()) {
            gState.currentStep = 0;
            gState.seqAStep = 0;
            gState.seqBStep = 0;
            gState.tickEpoch++;
            tickCounter = 0;
        }

        // Clock: Pulse In 1 if patched, otherwise tempoBpm + swing (internal).
        bool useExtClock = Connected(Input::Pulse1);
        if (gState.playing) {
            bool advance = false;
            if (useExtClock) {
                if (PulseIn1RisingEdge()) advance = true;
            } else {
                refresh_ticks_per_step();
                tickCounter++;
                if (tickCounter >= ticksPerStep) {
                    tickCounter = 0;
                    advance = true;
                }
            }
            if (advance) advance_sequencer();
        } else {
            gateA = false;
            gateB = false;
        }

        // Clamp playheads if the browser/panel shortened a loop mid-bar.
        if (gState.currentStep >= gState.seqLength) {
            gState.currentStep = 0;
            gState.tickEpoch++;
        }
        if (gState.seqAStep >= gState.seqALength) {
            gState.seqAStep = 0;
            gState.tickEpoch++;
        }
        if (gState.seqBStep >= gState.seqBLength) {
            gState.seqBStep = 0;
            gState.tickEpoch++;
        }

        // Melodic CV holds last roll note; gate only fires on steps with a note.
        int32_t transA = transpose_from_cv(CVIn1());
        int32_t transB = transpose_from_cv(CVIn2());

        uint8_t noteA = clamp_note(static_cast<int32_t>(heldNoteA) + transA);
        uint8_t noteB = clamp_note(static_cast<int32_t>(heldNoteB) + transB);

        CVOut1MIDINote(noteA);
        CVOut2MIDINote(noteB);

        PulseOut1(gateA);
        PulseOut2(gateB);

        // Mix active drum voices. Samples play full length at fixed velocity
        // gain; any noise path (unused in normal operation) uses a fast decay.
        int32_t mix = 0;
        for (int t = 0; t < DS_DRUM_TRACKS; t++) {
            DrumVoice& voice = voices[t];
            if (!voice.active || voice.env <= 0) continue;

            int16_t sample = 0;
            if (voice.use_noise) {
                sample = static_cast<int16_t>((xorshift(voice.rng) >> 20) & 0x7FF) - 1024;
                mix += (static_cast<int32_t>(sample) * voice.env) >> 16;
                voice.env -= voice.env >> 5;
                if (voice.env < 32) {
                    voice.env = 0;
                    voice.active = false;
                }
            } else {
                uint32_t frame = voice.pos_q16 >> 16;
                if (frame >= voice.length) {
                    voice.active = false;
                    continue;
                }
                sample = decode_sample_byte(
                    static_cast<uint8_t>(drum_seq_voice_cache_sample(
                        static_cast<uint32_t>(t), frame)));
                mix += (static_cast<int32_t>(sample) * voice.env) >> 16;
                voice.pos_q16 += voice.rate_q16;
            }
        }
        if (mix > 2047) mix = 2047;
        if (mix < -2048) mix = -2048;

        AudioOut1((int16_t)mix);
        AudioOut2((int16_t)mix);

        update_leds(sw);
    }
};

int main()
{
    // 144 MHz is the Workshop Computer default for audio cards.
    set_sys_clock_khz(144000, true);

    // USB must be initialised on Core 0 before Core 1 polls tud_task().
    board_init();
    tud_init(0);

    DrumAndSequence card;
    multicore_launch_core1(core1_entry);
    card.Run();   // never returns — audio ISR / ProcessSample loop
}
