/* xemu custom fork - optional 0-200% host output amplifier */
#include "qemu/osdep.h"
#include <math.h>
#include <SDL3/SDL.h>
#include "xemu-features/volume-amplifier/volume.h"

static SDL_AudioStream *g_stream;
static double g_source_volume = -1.0;
static float g_applied_gain = -1.0f;

void xemu_volume_amplifier_reset(SDL_AudioStream *stream)
{
    if (stream == NULL || stream == g_stream) {
        g_stream = stream;
        g_source_volume = -1.0;
        g_applied_gain = -1.0f;
    }
}

bool xemu_volume_amplifier_apply(SDL_AudioStream *stream, double volume)
{
    if (stream == NULL) {
        return true;
    }
    if (g_stream != stream) {
        g_stream = stream;
        g_source_volume = -1.0;
        g_applied_gain = -1.0f;
    }

    volume = fmax(0.0, fmin(volume, 2.0));
    if (g_source_volume != volume) {
        /* Keep Xemu's perceptual curve through 100%; above 100%, apply the
         * requested host gain directly so 200% means 2.0x output gain. */
        float gain = volume <= 1.0 ? pow(volume, M_E) : (float)volume;
        if (g_applied_gain != gain) {
            SDL_SetAudioStreamGain(stream, gain);
            g_applied_gain = gain;
        }
        g_source_volume = volume;
    }
    return true;
}
