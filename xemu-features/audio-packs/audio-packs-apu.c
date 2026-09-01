/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Portions of this bridge are derived from the xemu/QEMU MCPX Audio
 * Processing Unit voice-processing implementation. Original notices:
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * Feature isolation/integration changes are part of the Joshua-1248 fork.
 */
/*
 * xemu custom fork - isolated MCPX audio-pack bridge
 *
 * The MCPX VP owns hardware voice semantics.  Audio-pack source discovery is
 * feature policy, so the snapshot/decode adapter lives here rather than in
 * vp.c.  The core VP calls one explicit hook.
 */

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include <math.h>
#include "hw/xbox/mcpx/apu/apu_int.h"
#include "hw/xbox/mcpx/apu/fpconv.h"
#include "hw/xbox/mcpx/apu/vp/adpcm.h"
#include "xemu-features/audio-packs/audio-packs.h"
#include "xemu-features/audio-packs/audio-packs-apu.h"

static uint32_t audio_packs_voice_get_mask(MCPXAPUState *d,
                                            uint16_t voice_handle,
                                            hwaddr offset, uint32_t mask)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] +
                   voice_handle * NV_PAVS_SIZE;
    return (ldl_le_phys(&address_space_memory, voice + offset) & mask) >>
           ctz32(mask);
}

static hwaddr audio_packs_get_data_ptr(hwaddr sge_base, unsigned int max_sge,
                                       uint32_t addr)
{
    unsigned int entry = addr / TARGET_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address =
        ldl_le_phys(&address_space_memory, sge_base + entry * 8);
    return prd_address + addr % TARGET_PAGE_SIZE;
}

static int16_t audio_packs_float_to_s16(float v)
{
    if (v <= -1.0f) {
        return INT16_MIN;
    }
    if (v >= 1.0f) {
        return INT16_MAX;
    }
    return (int16_t)lrintf(v * 32767.0f);
}

static uint16_t audio_packs_read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t audio_packs_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static const char *audio_packs_source_format_name(bool adpcm,
                                                unsigned int sample_size)
{
    if (adpcm) {
        return "xbox_adpcm";
    }
    switch (sample_size) {
    case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
        return "pcm_u8";
    case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
        return "pcm_s16";
    case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
        return "pcm_s24";
    case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
        return "pcm_s32";
    default:
        return "pcm_unknown";
    }
}

/*
 * Snapshot a non-streaming hardware voice without mutating CBO or notifier
 * state. This deliberately mirrors voice_get_samples()'s source decode so the
 * dumped WAV represents the exact source samples the APU consumes before
 * pitch/envelope/filter/HRTF/DSP processing.
 */
static int16_t *audio_packs_snapshot_static_voice(MCPXAPUState *d, uint16_t v,
                                                uint32_t *out_frames,
                                                unsigned int *out_channels,
                                                uint32_t *out_rate,
                                                bool *out_loop,
                                                uint32_t *out_loop_start,
                                                const char **out_format)
{
    bool stream = audio_packs_voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE);
    bool multipass = audio_packs_voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS);
    if (stream || multipass) {
        return NULL;
    }

    bool stereo = audio_packs_voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO);
    unsigned int channels = stereo ? 2 : 1;
    unsigned int sample_size = audio_packs_voice_get_mask(
        d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned int container_size_index = audio_packs_voice_get_mask(
        d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    static const unsigned int container_sizes[4] = { 1, 2, 0, 4 };
    if (container_size_index >= ARRAY_SIZE(container_sizes)) {
        return NULL;
    }
    unsigned int container_size = container_sizes[container_size_index];
    bool adpcm = container_size_index ==
        NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM;
    if (!adpcm && (sample_size >= 4 || container_size == 0)) {
        return NULL;
    }

    uint32_t ebo = audio_packs_voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT,
                                  NV_PAVS_VOICE_PAR_NEXT_EBO);
    uint32_t lbo = audio_packs_voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                                  NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    uint32_t ba = audio_packs_voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSL_START,
                                 NV_PAVS_VOICE_CUR_PSL_START_BA);
    unsigned int samples_per_block = 1 + audio_packs_voice_get_mask(
        d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    bool loop = audio_packs_voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                               NV_PAVS_VOICE_CFG_FMT_LOOP);

    uint64_t frames64 = (uint64_t)ebo + 1;
    if (frames64 == 0 || frames64 > UINT32_MAX) {
        return NULL;
    }
    uint32_t frames = (uint32_t)frames64;
    if ((uint64_t)frames * channels > SIZE_MAX / sizeof(int16_t)) {
        return NULL;
    }
    int16_t *pcm = g_try_malloc_n((size_t)frames * channels, sizeof(*pcm));
    if (!pcm) {
        return NULL;
    }

    size_t block_size = adpcm ? 36 : container_size;
    block_size *= samples_per_block;

    int adpcm_block_index = -1;
    uint32_t adpcm_block[36 * 2 / 4];
    int16_t adpcm_decoded[ADPCM_SAMPLES_PER_BLOCK * 2];

    for (uint32_t frame = 0; frame < frames; frame++) {
        if (adpcm) {
            unsigned int block_index = frame / ADPCM_SAMPLES_PER_BLOCK;
            unsigned int block_position = frame % ADPCM_SAMPLES_PER_BLOCK;
            if ((int)block_index != adpcm_block_index) {
                uint32_t linear_addr = ba + block_index * block_size;
                for (unsigned int word_index = 0;
                     word_index < 9 * samples_per_block; word_index++) {
                    hwaddr addr = audio_packs_get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                               0xFFFFFFFF, linear_addr);
                    adpcm_block[word_index] =
                        ldl_le_phys(&address_space_memory, addr);
                    linear_addr += 4;
                }
                adpcm_decode_block(adpcm_decoded, (uint8_t *)adpcm_block,
                                   block_size, channels);
                adpcm_block_index = block_index;
            }
            for (unsigned int ch = 0; ch < channels; ch++) {
                pcm[(size_t)frame * channels + ch] =
                    adpcm_decoded[block_position * channels + ch];
            }
        } else {
            uint32_t linear_addr = ba + frame * block_size;
            hwaddr addr = audio_packs_get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                       0xFFFFFFFF, linear_addr);
            for (unsigned int ch = 0; ch < channels; ch++) {
                uint32_t ival = 0;
                float fval = 0.0f;
                switch (sample_size) {
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                    ival = ldub_phys(&address_space_memory, addr);
                    fval = uint8_to_float(ival & 0xff);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                    ival = lduw_le_phys(&address_space_memory, addr);
                    fval = int16_to_float(ival & 0xffff);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                    ival = ldl_le_phys(&address_space_memory, addr);
                    fval = int24_to_float(ival);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                    ival = ldl_le_phys(&address_space_memory, addr);
                    fval = int32_to_float(ival);
                    break;
                default:
                    g_free(pcm);
                    return NULL;
                }
                pcm[(size_t)frame * channels + ch] =
                    audio_packs_float_to_s16(fval);
                addr += container_size;
            }
        }
    }

    int16_t pitch = (int16_t)audio_packs_voice_get_mask(
        d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
        NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    double rate = 48000.0 * pow(2.0, pitch / 4096.0);
    rate = MAX(100.0, MIN(rate, 384000.0));

    *out_frames = frames;
    *out_channels = channels;
    *out_rate = (uint32_t)lrint(rate);
    *out_loop = loop;
    *out_loop_start = MIN(lbo, frames - 1);
    *out_format = audio_packs_source_format_name(adpcm, sample_size);
    return pcm;
}


/*
 * Snapshot the currently active SSL segment of a streaming hardware voice.
 * This mirrors the streaming branch of voice_get_samples() without changing
 * CBO, SSL indices, notifier state, envelopes, or DSP routing.  The feature
 * core stitches successive decoded segments into one WAV on its background
 * dump thread.
 */
static int16_t *audio_packs_snapshot_stream_segment(
    MCPXAPUState *d, uint16_t v, uint32_t *out_frames,
    unsigned int *out_channels, uint32_t *out_rate,
    uint64_t *out_signature, uint32_t *out_cbo, const char **out_format)
{
    bool stream = audio_packs_voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                              NV_PAVS_VOICE_CFG_FMT_DATA_TYPE);
    bool multipass = audio_packs_voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                                 NV_PAVS_VOICE_CFG_FMT_MULTIPASS);
    if (!stream || multipass || v >= MCPX_HW_MAX_VOICES) {
        return NULL;
    }

    int ssl_index = d->vp.ssl[v].ssl_index;
    int ssl_seg = d->vp.ssl[v].ssl_seg;
    if (ssl_index < 0 || ssl_index >= MCPX_HW_SSLS_PER_VOICE || ssl_seg < 0) {
        return NULL;
    }
    uint8_t count = d->vp.ssl[v].count[ssl_index];
    if (count == 0 || ssl_seg >= count) {
        return NULL;
    }

    uint32_t page = d->vp.ssl[v].base[ssl_index] + (uint32_t)ssl_seg;
    hwaddr desc = d->regs[NV_PAPU_VPSSLADDR] + (hwaddr)page * 8;
    hwaddr segment_offset = ldl_le_phys(&address_space_memory, desc);
    uint32_t segment_length = ldl_le_phys(&address_space_memory, desc + 4);
    if (segment_offset == 0 || segment_length == 0) {
        return NULL;
    }

    uint32_t live_cbo = audio_packs_voice_get_mask(
        d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO);
    uint64_t signature = (uint64_t)segment_offset;
    signature ^= (uint64_t)segment_length << 32;
    signature ^= (uint64_t)(unsigned int)ssl_index << 61;
    signature ^= (uint64_t)(unsigned int)ssl_seg << 48;

    /*
     * Some titles refill one SSL descriptor in place and can switch content
     * between service calls without presenting a clean CBO wrap.  Mix a tiny
     * immutable-at-activation source probe into the descriptor signature so
     * those refills are still observed.  This is deliberately only the first
     * few bytes: hashing the whole segment every 1500-Hz service pass would
     * defeat the cheap "segment unchanged" gate.
     */
    const uint64_t stream_ram_size = memory_region_size(d->ram);
    if ((uint64_t)segment_offset < stream_ram_size) {
        size_t probe_len = MIN((uint64_t)32,
                               stream_ram_size - (uint64_t)segment_offset);
        if (probe_len) {
            signature ^= fast_hash(&d->ram_ptr[segment_offset], probe_len);
        }
    }
    if (!xemu_audio_packs_stream_segment_needed(v, signature, live_cbo)) {
        return NULL;
    }

    uint32_t frames = segment_length & 0xffff;
    unsigned int container_size_index = (segment_length >> 16) & 3;
    unsigned int samples_per_block = 1 + ((segment_length >> 18) & 0x1f);
    bool stereo = ((segment_length >> 23) & 1) != 0;
    unsigned int channels = stereo ? 2 : 1;
    static const unsigned int container_sizes[4] = { 1, 2, 0, 4 };
    if (frames == 0 || container_size_index >= ARRAY_SIZE(container_sizes)) {
        return NULL;
    }

    unsigned int configured_cs = audio_packs_voice_get_mask(
        d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    unsigned int configured_spb = 1 + audio_packs_voice_get_mask(
        d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    bool configured_stereo = audio_packs_voice_get_mask(
        d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_STEREO);
    if (configured_cs != container_size_index ||
        configured_spb != samples_per_block ||
        configured_stereo != stereo) {
        return NULL;
    }

    unsigned int sample_size = audio_packs_voice_get_mask(
        d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned int container_size = container_sizes[container_size_index];
    bool adpcm = container_size_index ==
                 NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM;
    if (adpcm) {
        sample_size = NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
    } else if (sample_size >= 4 || container_size == 0) {
        return NULL;
    }

    if ((uint64_t)frames * channels > SIZE_MAX / sizeof(int16_t)) {
        return NULL;
    }
    int16_t *pcm = g_try_malloc_n((size_t)frames * channels, sizeof(*pcm));
    if (!pcm) {
        return NULL;
    }

    size_t block_size = (adpcm ? 36u : container_size) * samples_per_block;
    int adpcm_block_index = -1;
    uint32_t adpcm_block[36 * 2 / 4];
    int16_t adpcm_decoded[ADPCM_SAMPLES_PER_BLOCK * 2];

    for (uint32_t frame = 0; frame < frames; frame++) {
        if (adpcm) {
            unsigned int block_index = frame / ADPCM_SAMPLES_PER_BLOCK;
            unsigned int block_position = frame % ADPCM_SAMPLES_PER_BLOCK;
            if ((int)block_index != adpcm_block_index) {
                uint64_t byte_offset = (uint64_t)block_index * block_size;
                uint64_t max_seg_byte = ((uint64_t)frames >> 6) * block_size;
                const uint64_t src_offset = (uint64_t)segment_offset +
                                            byte_offset;
                if (byte_offset + block_size > max_seg_byte ||
                    block_size > sizeof(adpcm_block) ||
                    src_offset >= stream_ram_size ||
                    block_size > stream_ram_size - src_offset) {
                    g_free(pcm);
                    return NULL;
                }
                memcpy(adpcm_block, &d->ram_ptr[src_offset], block_size);
                adpcm_decode_block(adpcm_decoded, (uint8_t *)adpcm_block,
                                   block_size, channels);
                adpcm_block_index = (int)block_index;
            }
            for (unsigned int ch = 0; ch < channels; ch++) {
                pcm[(size_t)frame * channels + ch] =
                    adpcm_decoded[block_position * channels + ch];
            }
        } else {
            /* Streaming SSL data lives in the APU's RAM mapping. Native VP
             * already uses d->ram_ptr for streamed ADPCM; use the same direct
             * mapping for PCM snapshots instead of issuing one address-space
             * transaction per sample/channel. Bounds are checked before each
             * source read so malformed descriptors still fail safely. */
            uint64_t addr = (uint64_t)segment_offset +
                            (uint64_t)frame * block_size;
            for (unsigned int ch = 0; ch < channels; ch++) {
                if (addr >= stream_ram_size ||
                    container_size > stream_ram_size - addr) {
                    g_free(pcm);
                    return NULL;
                }
                const uint8_t *src = &d->ram_ptr[addr];
                uint32_t ival = 0;
                float fval = 0.0f;
                switch (sample_size) {
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                    ival = src[0];
                    fval = uint8_to_float(ival);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                    if (container_size < 2) { g_free(pcm); return NULL; }
                    ival = audio_packs_read_le16(src);
                    fval = int16_to_float(ival);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                    if (container_size < 4) { g_free(pcm); return NULL; }
                    ival = audio_packs_read_le32(src);
                    fval = sample_size == NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24
                        ? int24_to_float(ival) : int32_to_float(ival);
                    break;
                default:
                    g_free(pcm);
                    return NULL;
                }
                pcm[(size_t)frame * channels + ch] =
                    audio_packs_float_to_s16(fval);
                addr += container_size;
            }
        }
    }

    int16_t pitch = (int16_t)audio_packs_voice_get_mask(
        d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
        NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    double rate = 48000.0 * pow(2.0, pitch / 4096.0);
    rate = MAX(100.0, MIN(rate, 384000.0));

    *out_frames = frames;
    *out_channels = channels;
    *out_rate = (uint32_t)lrint(rate);
    *out_signature = signature;
    *out_cbo = live_cbo;
    *out_format = audio_packs_source_format_name(adpcm, sample_size);
    return pcm;
}

void xemu_audio_packs_apu_prepare_voice_if_needed(MCPXAPUState *d, uint16_t v)
{
    /*
     * The native VP already calls this feature boundary before consuming a
     * voice. Static buffers are normally snapshotted once, but an unmatched
     * resident buffer may be retried after a later guest CBO/SetCurrentPosition
     * event because some engines refill/reuse it for a different logical sound.
     * Streaming SSL voices remain observable so successive hardware segments
     * can be stitched together.
     */
    if (!xemu_audio_packs_should_prepare_voice()) {
        return;
    }

    bool stream = audio_packs_voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                              NV_PAVS_VOICE_CFG_FMT_DATA_TYPE);
    bool multipass = audio_packs_voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                                 NV_PAVS_VOICE_CFG_FMT_MULTIPASS);
    if (multipass) {
        if (!xemu_audio_packs_voice_prepared(v)) {
            xemu_audio_packs_voice_mark_unsupported(v);
        }
        return;
    }

    if (stream) {
        if (!xemu_audio_packs_should_prepare_stream_voice()) {
            return;
        }

        int ssl_index = d->vp.ssl[v].ssl_index;
        if (ssl_index >= 0 && ssl_index < MCPX_HW_SSLS_PER_VOICE &&
            d->vp.ssl[v].count[ssl_index] == 0) {
            xemu_audio_packs_stream_voice_idle(v);
            return;
        }

        uint32_t frames = 0;
        uint32_t source_rate = 0;
        uint32_t live_cbo = 0;
        unsigned int channels = 0;
        uint64_t signature = 0;
        const char *source_format = NULL;
        int16_t *pcm = audio_packs_snapshot_stream_segment(
            d, v, &frames, &channels, &source_rate, &signature, &live_cbo,
            &source_format);
        if (!pcm) {
            return;
        }
        xemu_audio_packs_stream_append_segment(v, pcm, frames, channels,
                                               source_rate, signature, live_cbo,
                                               source_format);
        g_free(pcm);
        return;
    }

    if ((xemu_audio_packs_voice_prepared(v) &&
         !xemu_audio_packs_static_voice_retry_needed(v)) ||
        !xemu_audio_packs_should_prepare_static_voice()) {
        return;
    }

    uint32_t frames = 0;
    uint32_t source_rate = 0;
    uint32_t loop_start = 0;
    unsigned int channels = 0;
    bool loop = false;
    const char *source_format = NULL;
    int16_t *pcm = audio_packs_snapshot_static_voice(
        d, v, &frames, &channels, &source_rate, &loop, &loop_start,
        &source_format);
    if (!pcm) {
        xemu_audio_packs_voice_mark_unsupported(v);
        return;
    }

    xemu_audio_packs_prepare_static_voice(v, pcm, frames, channels, source_rate,
                                          loop, loop_start, source_format);
    g_free(pcm);
}

void xemu_audio_packs_apu_override_stream_samples(uint16_t v,
                                                  float samples[][2],
                                                  int count)
{
    if (count > 0) {
        /* This feature bridge sits immediately after the native VP source
         * fetch. It now handles both exact SSL replacement and the universal
         * consumed-source-window fallback for resident/ring/packet sources. */
        xemu_audio_packs_process_consumed_source(v, samples, count);
    }
}

