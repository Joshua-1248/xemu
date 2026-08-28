/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_int.h"
#include "xemu-features/fast-forward/audio.h"
#include "xemu-features/volume-amplifier/volume.h"

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    SDL_AudioSpec spec = {
        .freq = 48000,
        .format = SDL_AUDIO_S16LE,
        .channels = 2,
    };

    d->monitor.stream = NULL;
    xemu_fast_forward_audio_reset(NULL);
    xemu_volume_amplifier_reset(NULL);

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        error_setg(errp, "SDL_Init failed: %s", SDL_GetError());
        return;
    }

    d->monitor.stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (d->monitor.stream == NULL) {
        error_setg(errp, "SDL_OpenAudioDeviceStream failed: %s",
                   SDL_GetError());
        return;
    }

    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(d->monitor.stream);

    SDL_AudioSpec dev_spec;
    int dev_buf_frames = 0;
    int dev_drain_bytes = 0;
    if (SDL_GetAudioDeviceFormat(dev, &dev_spec, &dev_buf_frames)) {
        dev_drain_bytes = dev_buf_frames * spec.channels *
                          SDL_AUDIO_BYTESIZE(spec.format) *
                          spec.freq / dev_spec.freq;
    }
    int frame_bytes = sizeof(d->monitor.frame_buf);
    int drain = MAX(dev_drain_bytes, frame_bytes);
    d->monitor.queued_bytes_low = drain;
    d->monitor.queued_bytes_high = 3 * drain;

    SDL_ResumeAudioDevice(dev);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    if (d->monitor.stream) {
        xemu_fast_forward_audio_reset(NULL);
        xemu_volume_amplifier_reset(NULL);
        SDL_DestroyAudioStream(d->monitor.stream);
        d->monitor.stream = NULL;
    }
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    if (d->monitor.stream) {
        /* Keep the native 0-100% path intact when the optional amplifier is
         * compiled out. The custom module owns only the extended 0-200%
         * behavior and its setter cache. */
        if (!xemu_volume_amplifier_apply(d->monitor.stream,
                                          g_config.audio.volume_limit)) {
            double volume =
                fmax(0.0, fmin(g_config.audio.volume_limit, 1.0));
            SDL_SetAudioStreamGain(d->monitor.stream, pow(volume, M_E));
        }

        /* Fast Forward may consume/transform the completed host block. With
         * the feature omitted, this inline hook is false and normal APU audio
         * follows the ordinary path below. */
        if (!xemu_fast_forward_audio_submit(d->monitor.stream,
                                             d->monitor.frame_buf,
                                             d->monitor.queued_bytes_high)) {
            SDL_PutAudioStreamData(d->monitor.stream,
                                   d->monitor.frame_buf,
                                   sizeof(d->monitor.frame_buf));
        }
    }

    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
}
