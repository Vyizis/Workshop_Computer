#include "voice_cache.h"

#include "sample_bank.h"

#include <string.h>

static int8_t g_voice_cache[DS_DRUM_TRACKS][DS_VOICE_CACHE_FRAMES];
static uint32_t g_voice_cache_len[DS_DRUM_TRACKS] = {};

void drum_seq_voice_cache_rebuild(void)
{
    // Call only when sample_bank_mutating is true (upload) or at boot — never from
    // the audio ISR. Reads flash via drum_seq_read_byte.
    memset(g_voice_cache, 0, sizeof(g_voice_cache));
    memset(g_voice_cache_len, 0, sizeof(g_voice_cache_len));

    if (!drum_seq_sample_bank_valid()) {
        return;
    }

    for (uint32_t track = 0; track < DS_DRUM_TRACKS; ++track) {
        const DrumSeqSample& sample = drum_seq_sample(track);
        if (sample.frame_count == 0) {
            continue;
        }

        uint32_t copy_len = sample.frame_count;
        if (copy_len > DS_VOICE_CACHE_FRAMES) {
            copy_len = DS_VOICE_CACHE_FRAMES;
        }

        for (uint32_t i = 0; i < copy_len; ++i) {
            g_voice_cache[track][i] =
                static_cast<int8_t>(drum_seq_read_byte(sample.offset + i));
        }
        g_voice_cache_len[track] = copy_len;
    }
}

uint32_t drum_seq_voice_cache_length(uint32_t track)
{
    if (track >= DS_DRUM_TRACKS) return 0u;
    return g_voice_cache_len[track];
}

const int8_t* drum_seq_voice_cache_pcm(uint32_t track)
{
    if (track >= DS_DRUM_TRACKS || g_voice_cache_len[track] == 0u) {
        return nullptr;
    }
    return g_voice_cache[track];
}

int8_t drum_seq_voice_cache_sample(uint32_t track, uint32_t frame)
{
    if (track >= DS_DRUM_TRACKS || frame >= g_voice_cache_len[track]) {
        return 0;
    }
    return g_voice_cache[track][frame];
}
