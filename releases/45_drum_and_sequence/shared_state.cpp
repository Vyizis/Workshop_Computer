#include "shared_state.h"

volatile SharedState gState = {};

void drum_seq_init_state(void)
{
    // Default pattern so the card is immediately musical with no browser.
    gState.playing      = 1;
    gState.currentStep  = 0;
    gState.seqLength    = 16;
    gState.swing        = 64;
    gState.tempoBpm     = 120;
    gState.tickEpoch    = 0;
    gState.seqAStep     = 0;
    gState.seqBStep     = 0;
    gState.seqALength   = 16;
    gState.seqBLength   = 16;
    gState.editTarget   = 0;
    gState.editStep     = 0;
    gState.selectedDrum = 0;

    // Default drum pattern: kick on 1/5/9/13, snare on 5/13, hats every other step.
    for (int t = 0; t < DS_DRUM_TRACKS; t++) {
        gState.drumLevel[t] = 100;
        gState.drumPitch[t] = 64;
        for (int s = 0; s < DS_STEPS; s++) {
            gState.drumHit[t][s] = 0;
        }
    }
    for (int s = 0; s < DS_STEPS; s++) {
        if ((s & 3) == 0) gState.drumHit[0][s] = 127;           // kick
        if (s == 4 || s == 12) gState.drumHit[1][s] = 110;      // snare
        if (s & 1) gState.drumHit[2][s] = 80;                   // hat
    }

    // Seq A: simple C minor pentatonic-ish line
    const uint8_t aNotes[] = { 60, 63, 65, 67, 72, 67, 65, 63,
                               60, 63, 65, 67, 72, 74, 72, 67 };
    for (int s = 0; s < DS_STEPS; s++) {
        gState.seqA_on[s]   = (s % 2 == 0) ? 1 : 0;
        gState.seqA_note[s] = aNotes[s];
        gState.seqB_on[s]   = (s == 0 || s == 8) ? 1 : 0;
        gState.seqB_note[s] = 48; // low bass C
    }
}
