# Drum&Sequence

Card **#45** — a Pocket Operator–style drum machine for the [Music Thing Modular Workshop Computer](https://www.musicthing.co.uk/Workshop-Computer/).

**Drum&Sequence** combines:

- **8-voice sample drum sequencer** — 16 steps × 8 tracks, flash samples (≤ ~250 ms per voice)
- **Two melodic sequencers** — independent 16-step loops with calibrated **1V/oct CV** and **gates** on CV Out 1/2 + Pulse Out 1/2
- **Browser editor** — PO-style step grid in Chrome over WebMIDI SysEx + Web Serial sample upload

## Quick start

### Build firmware

Requires the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) and ARM GCC toolchain. With the Pico VS Code extension installed:

```bash
cd releases/45_drum_and_sequence
mkdir build && cd build
cmake .. -DPICO_SDK_FETCH_FROM_GIT=ON
make -j
```

Flash `drum_and_sequence.uf2` (works on **2MB** or **16MB** program cards).

### Web editor

1. Insert the card and connect USB-C to your computer.
2. Open [`web/index.html`](web/index.html) in Chrome or Edge.
3. **Pattern** tab: patterns sync automatically over WebMIDI (device name **DrumAndSequence**).
4. **Samples** tab: **Connect Serial**, pick the DrumAndSequence port, load WAVs, **Upload Bank**.

Short one-shot samples work best. Audio is converted to 48 kHz mono 8-bit on upload (trimmed to ~250 ms).

## Panel controls

The Z switch is **(ON)–OFF–ON**: **Up** and **Middle** latch; **Down** is momentary and springs back to Middle (same as drumdrum).

| Switch | Main | X | Y |
|--------|------|---|---|
| **Up** (play) | Tempo (BPM) | Pattern length (1–16) | Swing |
| **Middle** (edit) | Target: voices 1–8, then Seq A, Seq B | Pitch / note | Level / gate |
| **Down** (momentary) | — | — | — |

- **Short press Down** — advance edit step (used when Middle is on Seq A/B)  
- **Long press Down** (≥500ms) — play / pause  

In Middle, turn Main through ten zones: eight drum voices (X/Y = pitch/level), then Seq A and Seq B (X/Y = note/gate for the current edit step).

## I/O

| Jack | Function |
|------|----------|
| Audio Out 1/2 | Drum mix (flash samples, 48 kHz mono) |
| CV Out 1 + Pulse Out 1 | Melodic sequencer A (pitch holds between notes) |
| CV Out 2 + Pulse Out 2 | Melodic sequencer B (pitch holds between notes) |
| Pulse In 1 | External clock |
| Pulse In 2 | Reset |
| CV In 1 | Transpose seq A |
| CV In 2 | Transpose seq B |

## Roadmap

- [x] Shared clock, drum grid, dual melodic CV/gate sequencers
- [x] WebMIDI SysEx editor (drums + seq A/B)
- [x] Flash sample playback (48 kHz mono 8-bit PCM)
- [x] Web Serial sample upload (Samples tab)
- [x] Pattern save to flash
- [x] Swing, accent, tempo (BPM field + panel)
- [ ] PO-style sound parameter layers (web shift params)

## License

MIT
