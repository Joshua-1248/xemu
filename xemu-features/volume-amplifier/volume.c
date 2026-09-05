/* SPDX-License-Identifier: GPL-2.0-or-later */
/* xemu custom fork - optional 0-300% host output amplifier + mute hotkey */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include <limits.h>
#include <math.h>
#include <SDL3/SDL.h>
#include "xemu-features/volume-amplifier/volume.h"

static SDL_AudioStream *g_stream;
static double g_source_volume = -1.0;
static float g_applied_gain = -1.0f;

/*
 * Mute state is written by SDL's event-watch path and consumed by the audio
 * monitor path. The callback never touches the audio stream, so it remains
 * safe even if SDL delivers an event watch from a different thread.
 */
static int g_muted;
static unsigned int g_mute_revision;
static unsigned int g_applied_mute_revision = UINT_MAX;
static int g_hotkey_watch_registered;

bool xemu_volume_amplifier_is_muted(void)
{
    return qatomic_read(&g_muted) != 0;
}

bool xemu_volume_amplifier_toggle_mute(void)
{
    int old_value;
    int new_value;

    do {
        old_value = qatomic_read(&g_muted);
        new_value = !old_value;
    } while (qatomic_cmpxchg(&g_muted, old_value, new_value) != old_value);

    qatomic_inc(&g_mute_revision);
    return new_value != 0;
}

static bool SDLCALL xemu_volume_amplifier_event_watch(void *userdata,
                                                      SDL_Event *event)
{
    (void)userdata;

    if (event != NULL &&
        event->type == SDL_EVENT_KEY_DOWN &&
        event->key.key == SDLK_M &&
        !event->key.repeat) {
        xemu_volume_amplifier_toggle_mute();
    }

    /* Event watches observe only; never consume the event. */
    return true;
}

static void xemu_volume_amplifier_ensure_hotkey_watch(void)
{
    /*
     * SDL_AddEventWatch() is thread-safe. Claim registration atomically so an
     * APU stream reset and monitor update cannot install duplicate callbacks.
     */
    if (qatomic_cmpxchg(&g_hotkey_watch_registered, 0, 1) == 0) {
        if (!SDL_AddEventWatch(xemu_volume_amplifier_event_watch, NULL)) {
            qatomic_set(&g_hotkey_watch_registered, 0);
        }
    }
}

void xemu_volume_amplifier_reset(SDL_AudioStream *stream)
{
    xemu_volume_amplifier_ensure_hotkey_watch();

    if (stream == NULL || stream == g_stream) {
        g_stream = stream;
        g_source_volume = -1.0;
        g_applied_gain = -1.0f;
        g_applied_mute_revision = UINT_MAX;
    }
}

bool xemu_volume_amplifier_apply(SDL_AudioStream *stream, double volume)
{
    xemu_volume_amplifier_ensure_hotkey_watch();

    if (stream == NULL) {
        return true;
    }
    if (g_stream != stream) {
        g_stream = stream;
        g_source_volume = -1.0;
        g_applied_gain = -1.0f;
        g_applied_mute_revision = UINT_MAX;
    }

    volume = fmax(0.0, fmin(volume, 3.0));

    const unsigned int mute_revision = qatomic_read(&g_mute_revision);
    const bool muted = qatomic_read(&g_muted) != 0;

    if (g_source_volume != volume ||
        g_applied_mute_revision != mute_revision) {
        /*
         * Keep Xemu's perceptual curve through 100%; above 100%, apply the
         * requested host gain directly so 300% means 3.0x output gain.
         * Muting overrides only the applied host gain; the user's configured
         * volume remains untouched and is restored immediately on unmute.
         */
        float gain = muted ? 0.0f :
                     (volume <= 1.0 ? pow(volume, M_E) : (float)volume);

        if (g_applied_gain != gain) {
            SDL_SetAudioStreamGain(stream, gain);
            g_applied_gain = gain;
        }

        g_source_volume = volume;
        g_applied_mute_revision = mute_revision;
    }
    return true;
}
