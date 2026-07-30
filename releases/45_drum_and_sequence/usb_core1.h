#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Launched on Core 1 after USB init on Core 0. Never returns.
void core1_entry(void);

#ifdef __cplusplus
}
#endif
