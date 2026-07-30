// WebMIDI SysEx handler for Drum&Sequence (Core 1).
//
// Envelope: F0 7D <cmd> <payload...> F7
// Runs entirely on Core 1; reads/writes gState directly (single-byte
// fields are atomic on the M0+). See midi_sysex.h for the command table.
//
// Live sync model (same idea as drumdrum):
//   1. On connect → FULL_DUMP of the whole pattern
//   2. Each step advance → TICK with drum / seqA / seqB playheads
//   3. Panel (or any) edits → PARAM_UPDATE mirroring the SET_* payload

#include "midi_sysex.h"
#include "shared_state.h"
#include "pattern_store.h"

#include "tusb.h"
#include <string.h>
#include "pico/time.h"

// ── Inbound SysEx accumulator ─────────────────────────────────
// Large enough for a full dump (~220 bytes) if the host echoes one back.
static uint8_t  rx_buf[320];
static uint16_t rx_len    = 0;
static bool     rx_in_msg = false;

// ── Outbound state mirror ─────────────────────────────────────
// Core 1 keeps a snapshot of pattern/transport. Whenever gState diverges
// (panel knobs, switch, or our own inbound SysEx), we push PARAM_UPDATE
// messages so the browser stays in sync without polling.
struct Mirror {
    uint8_t  playing;
    uint8_t  currentStep;
    uint8_t  seqAStep;
    uint8_t  seqBStep;
    uint8_t  seqLength;
    uint8_t  seqALength;
    uint8_t  seqBLength;
    uint8_t  swing;
    uint8_t  tempoBpm;
    uint8_t  drumHit[DS_DRUM_TRACKS][DS_STEPS];
    uint8_t  seqA_on[DS_STEPS];
    uint8_t  seqA_note[DS_STEPS];
    uint8_t  seqB_on[DS_STEPS];
    uint8_t  seqB_note[DS_STEPS];
    uint32_t tickEpoch;
    bool     was_mounted;
    bool     dumped;          // initial FULL_DUMP sent for this USB session
};

static Mirror mirror = {};

// ── Outbound helpers ──────────────────────────────────────────

static void send_sysex(const uint8_t *payload, uint16_t len)
{
    if (!tud_midi_mounted()) return;

    uint8_t buf[340];
    if ((size_t)(len + 3) > sizeof(buf)) return;

    buf[0] = 0xF0;
    buf[1] = DS_SYSEX_MFR;
    memcpy(&buf[2], payload, len);
    buf[2 + len] = 0xF7;

    // FULL_DUMP is ~220 stream bytes. Write in chunks and pump USB so the
    // TinyUSB MIDI TX FIFO never silently truncates mid-message.
    const uint16_t total = (uint16_t)(len + 3);
    uint16_t sent = 0;
    absolute_time_t deadline = make_timeout_time_ms(100);
    while (sent < total && tud_midi_mounted() && !time_reached(deadline)) {
        uint32_t n = tud_midi_stream_write(0, buf + sent, total - sent);
        if (n == 0) {
            tud_task();
            continue;
        }
        sent = (uint16_t)(sent + n);
        tud_task();
    }
}

static void send_tick(void)
{
    uint8_t p[4] = {
        DS_SYSEX_TICK,
        gState.currentStep,
        gState.seqAStep,
        gState.seqBStep
    };
    send_sysex(p, sizeof(p));
}

static void send_param(const uint8_t *payload, uint16_t len)
{
    uint8_t buf[16];
    if ((size_t)len + 1u > sizeof(buf)) return;
    buf[0] = DS_SYSEX_PARAM_UPDATE;
    memcpy(&buf[1], payload, len);
    send_sysex(buf, (uint16_t)(len + 1));
}

static void send_param_playing(void)
{
    uint8_t p[2] = { DS_SYSEX_SET_PLAYING, (uint8_t)(gState.playing ? 1 : 0) };
    send_param(p, sizeof(p));
}

static void send_param_length(uint8_t target, uint8_t length)
{
    uint8_t p[3] = { DS_SYSEX_SET_LENGTH, target, length };
    send_param(p, sizeof(p));
}

static void send_param_tempo(void)
{
    uint8_t bpm = gState.tempoBpm;
    uint8_t p[3] = {
        DS_SYSEX_SET_TEMPO,
        (uint8_t)((bpm >> 7) & 0x7F),
        (uint8_t)(bpm & 0x7F)
    };
    send_param(p, sizeof(p));
}

static void send_param_swing(void)
{
    uint8_t p[2] = { DS_SYSEX_SET_SWING, (uint8_t)(gState.swing & 0x7F) };
    send_param(p, sizeof(p));
}

static void send_param_drum(uint8_t track, uint8_t step)
{
    uint8_t p[4] = {
        DS_SYSEX_SET_DRUM_STEP,
        track,
        step,
        (uint8_t)(gState.drumHit[track][step] & 0x7F)
    };
    send_param(p, sizeof(p));
}

static void send_param_seq(uint8_t seq, uint8_t step)
{
    if (seq == 0) {
        uint8_t p[5] = {
            DS_SYSEX_SET_SEQ_STEP,
            0,
            step,
            (uint8_t)(gState.seqA_on[step] ? 1 : 0),
            (uint8_t)(gState.seqA_note[step] & 0x7F)
        };
        send_param(p, sizeof(p));
    } else {
        uint8_t p[5] = {
            DS_SYSEX_SET_SEQ_STEP,
            1,
            step,
            (uint8_t)(gState.seqB_on[step] ? 1 : 0),
            (uint8_t)(gState.seqB_note[step] & 0x7F)
        };
        send_param(p, sizeof(p));
    }
}

static void send_firmware_version(void)
{
    uint8_t p[4] = { DS_SYSEX_FIRMWARE_VERSION, 0, 1, 0 };
    send_sysex(p, sizeof(p));
}

static uint8_t clamp_bpm(uint8_t bpm)
{
    if (bpm < 40) return 40;
    if (bpm > 240) return 240;
    return bpm;
}

static void snapshot_mirror(void)
{
    mirror.playing     = gState.playing;
    mirror.currentStep = gState.currentStep;
    mirror.seqAStep    = gState.seqAStep;
    mirror.seqBStep    = gState.seqBStep;
    mirror.seqLength   = gState.seqLength;
    mirror.seqALength  = gState.seqALength;
    mirror.seqBLength  = gState.seqBLength;
    mirror.swing       = gState.swing;
    mirror.tempoBpm    = gState.tempoBpm;
    mirror.tickEpoch   = gState.tickEpoch;
    memcpy(mirror.drumHit,   (const void*)gState.drumHit,   sizeof(mirror.drumHit));
    memcpy(mirror.seqA_on,   (const void*)gState.seqA_on,   sizeof(mirror.seqA_on));
    memcpy(mirror.seqA_note, (const void*)gState.seqA_note, sizeof(mirror.seqA_note));
    memcpy(mirror.seqB_on,   (const void*)gState.seqB_on,   sizeof(mirror.seqB_on));
    memcpy(mirror.seqB_note, (const void*)gState.seqB_note, sizeof(mirror.seqB_note));
}

// FULL_DUMP layout (payload after cmd byte):
//   playing, currentStep, seqLength, seqALength, seqBLength, seqAStep, seqBStep,
//   swing, tempoHi, tempoLo,
//   drumHit[8][16], then per step: A_on, A_note, B_on, B_note
static void send_full_dump(void)
{
    uint8_t p[11 + DS_DRUM_TRACKS * DS_STEPS + 4 * DS_STEPS];
    uint16_t i = 0;
    uint8_t bpm = clamp_bpm(gState.tempoBpm);

    p[i++] = DS_SYSEX_FULL_DUMP;
    p[i++] = gState.playing;
    p[i++] = gState.currentStep;
    p[i++] = gState.seqLength;
    p[i++] = gState.seqALength;
    p[i++] = gState.seqBLength;
    p[i++] = gState.seqAStep;
    p[i++] = gState.seqBStep;
    p[i++] = gState.swing & 0x7F;
    p[i++] = (uint8_t)((bpm >> 7) & 0x7F);
    p[i++] = (uint8_t)(bpm & 0x7F);

    for (int t = 0; t < DS_DRUM_TRACKS; t++) {
        for (int s = 0; s < DS_STEPS; s++) {
            p[i++] = gState.drumHit[t][s] & 0x7F;
        }
    }
    for (int s = 0; s < DS_STEPS; s++) {
        p[i++] = gState.seqA_on[s] ? 1 : 0;
        p[i++] = gState.seqA_note[s] & 0x7F;
        p[i++] = gState.seqB_on[s] ? 1 : 0;
        p[i++] = gState.seqB_note[s] & 0x7F;
    }

    send_sysex(p, i);
    snapshot_mirror();
}

static void apply_sysex(const uint8_t *body, uint16_t len)
{
    if (len < 2 || body[0] != DS_SYSEX_MFR) return;

    uint8_t cmd = body[1];
    switch (cmd) {
    case DS_SYSEX_SET_DRUM_STEP:
        if (len >= 5) {
            uint8_t track = body[2];
            uint8_t step  = body[3];
            if (track < DS_DRUM_TRACKS && step < DS_STEPS) {
                gState.drumHit[track][step] = body[4] & 0x7F;
                mirror.drumHit[track][step] = gState.drumHit[track][step];
            }
        }
        break;

    case DS_SYSEX_SET_SEQ_STEP:
        if (len >= 6) {
            uint8_t seq  = body[2];
            uint8_t step = body[3];
            if (step < DS_STEPS) {
                if (seq == 0) {
                    gState.seqA_on[step]   = body[4] ? 1 : 0;
                    gState.seqA_note[step] = body[5] & 0x7F;
                    mirror.seqA_on[step]   = gState.seqA_on[step];
                    mirror.seqA_note[step] = gState.seqA_note[step];
                } else if (seq == 1) {
                    gState.seqB_on[step]   = body[4] ? 1 : 0;
                    gState.seqB_note[step] = body[5] & 0x7F;
                    mirror.seqB_on[step]   = gState.seqB_on[step];
                    mirror.seqB_note[step] = gState.seqB_note[step];
                }
            }
        }
        break;

    case DS_SYSEX_SET_LENGTH:
        if (len >= 4) {
            uint8_t target = body[2];
            uint8_t v = body[3];
            if (v < 1) v = 1;
            if (v > DS_STEPS) v = DS_STEPS;
            if (target == 0) {
                gState.seqLength = v;
                mirror.seqLength = v;
            } else if (target == 1) {
                gState.seqALength = v;
                mirror.seqALength = v;
            } else if (target == 2) {
                gState.seqBLength = v;
                mirror.seqBLength = v;
            }
        }
        break;

    case DS_SYSEX_SET_PLAYING:
        if (len >= 3) {
            gState.playing = body[2] ? 1 : 0;
            mirror.playing = gState.playing;
        }
        break;

    case DS_SYSEX_SET_TEMPO:
        if (len >= 4) {
            uint16_t bpm = (uint16_t)(((body[2] & 0x7F) << 7) | (body[3] & 0x7F));
            if (bpm > 255) bpm = 255;
            gState.tempoBpm = clamp_bpm((uint8_t)bpm);
            mirror.tempoBpm = gState.tempoBpm;
        }
        break;

    case DS_SYSEX_SET_SWING:
        if (len >= 3) {
            gState.swing = body[2] & 0x7F;
            mirror.swing = gState.swing;
        }
        break;

    case DS_SYSEX_SAVE_TO_FLASH: {
        uint8_t ok = drum_seq_pattern_save() ? DS_SYSEX_SAVE_OK : DS_SYSEX_SAVE_ERR;
        send_sysex(&ok, 1);
        break;
    }

    case DS_SYSEX_REQUEST_DUMP:
        send_full_dump();
        break;

    case DS_SYSEX_INTERFACE_VERSION:
        send_firmware_version();
        send_full_dump();
        break;

    default:
        break;
    }
}

static void parse_midi_bytes(const uint8_t *data, uint32_t count)
{
    for (uint32_t n = 0; n < count; n++) {
        uint8_t b = data[n];
        if (!rx_in_msg) {
            if (b == 0xF0) {
                rx_in_msg = true;
                rx_len = 0;
                if (rx_len < sizeof(rx_buf)) rx_buf[rx_len++] = b;
            }
        } else {
            if (rx_len < sizeof(rx_buf)) rx_buf[rx_len++] = b;
            if (b == 0xF7) {
                if (rx_len >= 3) {
                    apply_sysex(rx_buf + 1, (uint16_t)(rx_len - 2));
                }
                rx_in_msg = false;
                rx_len = 0;
            }
        }
    }
}

static void push_mirror_diffs(void)
{
    if (gState.playing != mirror.playing) {
        mirror.playing = gState.playing;
        send_param_playing();
    }
    if (gState.seqLength != mirror.seqLength) {
        mirror.seqLength = gState.seqLength;
        send_param_length(0, mirror.seqLength);
    }
    if (gState.seqALength != mirror.seqALength) {
        mirror.seqALength = gState.seqALength;
        send_param_length(1, mirror.seqALength);
    }
    if (gState.seqBLength != mirror.seqBLength) {
        mirror.seqBLength = gState.seqBLength;
        send_param_length(2, mirror.seqBLength);
    }
    if (gState.tempoBpm != mirror.tempoBpm) {
        mirror.tempoBpm = gState.tempoBpm;
        send_param_tempo();
    }
    if (gState.swing != mirror.swing) {
        mirror.swing = gState.swing;
        send_param_swing();
    }

    for (int t = 0; t < DS_DRUM_TRACKS; t++) {
        for (int s = 0; s < DS_STEPS; s++) {
            uint8_t v = gState.drumHit[t][s];
            if (v != mirror.drumHit[t][s]) {
                mirror.drumHit[t][s] = v;
                send_param_drum((uint8_t)t, (uint8_t)s);
            }
        }
    }
    for (int s = 0; s < DS_STEPS; s++) {
        if (gState.seqA_on[s] != mirror.seqA_on[s] ||
            gState.seqA_note[s] != mirror.seqA_note[s]) {
            mirror.seqA_on[s] = gState.seqA_on[s];
            mirror.seqA_note[s] = gState.seqA_note[s];
            send_param_seq(0, (uint8_t)s);
        }
        if (gState.seqB_on[s] != mirror.seqB_on[s] ||
            gState.seqB_note[s] != mirror.seqB_note[s]) {
            mirror.seqB_on[s] = gState.seqB_on[s];
            mirror.seqB_note[s] = gState.seqB_note[s];
            send_param_seq(1, (uint8_t)s);
        }
    }
}

void midi_device_task(void)
{
    const bool mounted_now = tud_midi_mounted();

    if (mounted_now && !mirror.was_mounted) {
        mirror.dumped = false;
    }
    mirror.was_mounted = mounted_now;

    if (mounted_now && !mirror.dumped) {
        send_firmware_version();
        send_full_dump();
        mirror.dumped = true;
    }

    if (mounted_now && gState.tickEpoch != mirror.tickEpoch) {
        mirror.tickEpoch   = gState.tickEpoch;
        mirror.currentStep = gState.currentStep;
        mirror.seqAStep    = gState.seqAStep;
        mirror.seqBStep    = gState.seqBStep;
        send_tick();
    }

    if (mounted_now) {
        push_mirror_diffs();
    }

    uint8_t buf[64];
    while (tud_midi_available()) {
        uint32_t n = tud_midi_stream_read(buf, sizeof(buf));
        if (n) parse_midi_bytes(buf, n);
    }
}
