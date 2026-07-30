// Core 1 entry: TinyUSB + WebMIDI pattern sync + Web Serial sample loader.
//
// board_init() / tud_init(0) run on Core 0 in main() before this is launched.
// Audio stays on Core 0 (ComputerCard::Run); this core never touches the DAC.

#include "usb_core1.h"
#include "midi_sysex.h"
#include "sample_manager.h"

#include "tusb.h"

extern "C" void core1_entry(void)
{
    while (true) {
        tud_task();                          // USB device stack
        midi_device_task();                  // Pattern tab (MIDI SysEx)
        drum_seq_sample_manager_task();   // Samples tab (CDC)
    }
}
