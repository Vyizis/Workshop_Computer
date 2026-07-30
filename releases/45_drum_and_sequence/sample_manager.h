#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Poll CDC sample-manager commands (X/I/R/S/W/E). Call from Core 1 USB loop.
void drum_seq_sample_manager_task(void);

#ifdef __cplusplus
}
#endif
