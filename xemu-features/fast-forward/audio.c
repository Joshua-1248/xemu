/* SPDX-License-Identifier: GPL-2.0-or-later */
/* xemu custom fork - Fast Forward host-audio transform */
#include "qemu/osdep.h"
#include <SDL3/SDL.h>
#include "ui/xemu-settings.h"
#include "xemu-features/fast-forward/fast-forward.h"
#include "xemu-features/fast-forward/audio.h"

#define FF_PITCH_XFADE_FRAMES 32

static SDL_AudioStream *g_stream;
static unsigned g_pitch_block_counter;
static int g_pitch_last_divisor = 1;
static bool g_pitch_have_tail;
static int16_t g_pitch_tail[FF_PITCH_XFADE_FRAMES][2];
static float g_applied_frequency_ratio = -1.0f;

static void pitch_reset(void)
{
    g_pitch_block_counter = 0;
    g_pitch_last_divisor = 1;
    g_pitch_have_tail = false;
}

void xemu_fast_forward_audio_reset(SDL_AudioStream *stream)
{
    g_stream = stream;
    g_applied_frequency_ratio = -1.0f;
    pitch_reset();
}

static void set_frequency_ratio(SDL_AudioStream *stream, float ratio)
{
    if (g_stream != stream) {
        xemu_fast_forward_audio_reset(stream);
    }
    if (g_applied_frequency_ratio == ratio) {
        return;
    }
    SDL_SetAudioStreamFrequencyRatio(stream, ratio);
    g_applied_frequency_ratio = ratio;
}

static bool pitch_prepare_block(const int16_t input[256][2], int divisor,
                                int16_t output[256][2])
{
    divisor = MAX(divisor, 1);
    if (divisor != g_pitch_last_divisor) {
        g_pitch_block_counter = 0;
        g_pitch_have_tail = false;
        g_pitch_last_divisor = divisor;
    }

    bool emit = (g_pitch_block_counter % (unsigned)divisor) == 0;
    g_pitch_block_counter++;
    if (!emit) {
        return false;
    }

    memcpy(output, input, sizeof(int16_t) * 256 * 2);
    if (g_pitch_have_tail) {
        for (int i = 0; i < FF_PITCH_XFADE_FRAMES; i++) {
            int a = FF_PITCH_XFADE_FRAMES - i;
            int b = i;
            for (int ch = 0; ch < 2; ch++) {
                int mixed = g_pitch_tail[i][ch] * a + output[i][ch] * b;
                output[i][ch] = (int16_t)(mixed / FF_PITCH_XFADE_FRAMES);
            }
        }
    }
    for (int i = 0; i < FF_PITCH_XFADE_FRAMES; i++) {
        g_pitch_tail[i][0] = input[256 - FF_PITCH_XFADE_FRAMES + i][0];
        g_pitch_tail[i][1] = input[256 - FF_PITCH_XFADE_FRAMES + i][1];
    }
    g_pitch_have_tail = true;
    return true;
}

bool xemu_fast_forward_audio_submit(SDL_AudioStream *stream,
                                    const int16_t input[256][2],
                                    int queued_bytes_high)
{
    if (stream == NULL) {
        return true;
    }
    if (g_stream != stream) {
        xemu_fast_forward_audio_reset(stream);
    }

    const int mode = xemu_fast_forward_mode();
    const bool active = mode != 1;
    if (!active) {
        pitch_reset();
        set_frequency_ratio(stream, 1.0f);
        return false; /* normal monitor path submits the block */
    }

    const bool unlimited = mode == 0;
    const bool preserve_pitch = g_config.general.fast_forward_preserve_pitch;

    if (unlimited) {
        /*
         * Do not ask SDL to resample every guest block at the old 100x ratio.
         * At true turbo rates the guest can produce blocks much faster than a
         * host device can consume them, creating a pathological stream churn
         * path.  Thin the source blocks first and keep SDL's ratio bounded.
         */
        /*
         * Unlimited is safely paced at roughly <=16.7x by the timing layer.
         * Keep host audio proportional to that rate without queueing every
         * guest block: pitch-preserved mode emits one block per ~16 guest
         * blocks; raised-pitch mode emits every other block at an 8x ratio.
         */
        const int divisor = preserve_pitch ? 16 : 2;
        set_frequency_ratio(stream, preserve_pitch ? 1.0f : 8.0f);
        int16_t turbo_block[256][2];
        if (pitch_prepare_block(input, divisor, turbo_block)) {
            SDL_PutAudioStreamData(stream, turbo_block, sizeof(turbo_block));
            /* Discarded guest blocks never enter SDL, so querying the host
             * stream for them is pure overhead at unlimited speed. */
            int queued = SDL_GetAudioStreamQueued(stream);
            if (queued > queued_bytes_high * 2) {
                SDL_ClearAudioStream(stream);
                pitch_reset();
            }
        }
        return true;
    }

    if (preserve_pitch) {
        set_frequency_ratio(stream, 1.0f);
        int16_t pitch_block[256][2];
        if (pitch_prepare_block(input, mode, pitch_block)) {
            SDL_PutAudioStreamData(stream, pitch_block, sizeof(pitch_block));
        }
        return true;
    }

    pitch_reset();
    set_frequency_ratio(stream, (float)mode);
    SDL_PutAudioStreamData(stream, input, sizeof(int16_t) * 256 * 2);
    return true;
}
