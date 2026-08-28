/*
 * xemu custom fork - isolated MCPX audio-pack bridge
 *
 * The MCPX VP owns hardware voice semantics.  Audio-pack source discovery is
 * feature policy, so the snapshot/decode adapter lives here rather than in
 * vp.c.  The core VP calls one explicit hook.
 */

#include "qemu/osdep.h"
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

void xemu_audio_packs_apu_prepare_voice_if_needed(MCPXAPUState *d, uint16_t v)
{
    /*
     * The MCPX model deliberately does not know which custom settings control
     * audio packs.  It asks the isolated feature boundary whether source
     * discovery is currently useful.  In replacement-only mode an empty index
     * therefore stays a true no-op without exposing that policy here.
     */
    if (!xemu_audio_packs_should_prepare_voice() ||
        xemu_audio_packs_voice_prepared(v)) {
        return;
    }

    bool stream = audio_packs_voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE);
    bool multipass = audio_packs_voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS);
    if (stream || multipass) {
        xemu_audio_packs_voice_mark_unsupported(v);
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

