/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * xemu custom fork - Original Xbox APU source audio dump/replacement
 *
 * Copyright (c) 2026 Joshua-1248
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "qemu/timer.h"
#include "ui/xemu-settings.h"
#include "xemu-xbe.h"
#include "hw/xbox/mcpx/apu/apu_regs.h"
#include "xemu-features/audio-packs/audio-packs.h"

#include <SDL3/SDL_audio.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <math.h>
#include <xxhash.h>

#define AUDIO_HASH_KEY_LEN 17
#define AUDIO_DUMP_SENTINEL ((gpointer)(uintptr_t)1)
#define AUDIO_STREAM_FINGERPRINT_VERSION 1u
#define AUDIO_STREAM_MATCH_MAX_FRAMES 65536u
#define AUDIO_STREAM_IDLE_GRACE_PASSES 128u
#define AUDIO_LOGICAL_MIN_LOOP_FRAMES 64u
#define AUDIO_LOGICAL_PCM_MIN_LOOP_FRAMES 256u
#define AUDIO_LOGICAL_PCM_ANALYSIS_MAX_BYTES (64u * 1024u * 1024u)
#define AUDIO_LOGICAL_LOOP_ANCHOR_MASK UINT64_C(0x3f)
#define AUDIO_LOGICAL_SEEN_OFFSETS 4u

/*
 * Transport-agnostic consumed-source matching. The matcher watches the exact
 * decoded PCM frames that the MCPX VP consumed, so source identity no longer
 * depends on where a game stores/submits the sound (static buffer, software
 * ring, SSL packet, or a reused/refilled backing store).
 *
 * Runtime uses a cheap rolling 16-frame anchor for every decoded frame. Only
 * content-defined anchors (roughly 1/16 windows) take the index lock and pay
 * for an XXH3 verification hash. This keeps the worker cost bounded while
 * making window positions independent of callback/ring alignment.
 */
#define AUDIO_CONSUMED_WINDOW_FRAMES 16u
#define AUDIO_CONSUMED_ANCHOR_MASK UINT64_C(0x0f)
#define AUDIO_CONSUMED_STREAM_INDEX_MAX_FRAMES 262144u
#define AUDIO_CONSUMED_BLOOM_BITS (1u << 20)
#define AUDIO_CONSUMED_BLOOM_WORDS (AUDIO_CONSUMED_BLOOM_BITS / 32u)
#define AUDIO_CONSUMED_REMOVE_POW UINT64_C(0x21e53a2d2fb67c3b)

static const uint32_t audio_stream_fingerprint_tiers[] = {
    16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768,
    65536,
};
#define AUDIO_STREAM_FINGERPRINT_TIER_COUNT \
    ARRAY_SIZE(audio_stream_fingerprint_tiers)

typedef enum AudioDumpJobKind {
    AUDIO_DUMP_JOB_STATIC = 0,
    AUDIO_DUMP_JOB_STREAM_BEGIN,
    AUDIO_DUMP_JOB_STREAM_APPEND,
    AUDIO_DUMP_JOB_STREAM_END,
    AUDIO_DUMP_JOB_STREAM_CANCEL,
} AudioDumpJobKind;

bool xemu_audio_packs_enabled(void)
{
    return g_config.audio.dump_enabled || g_config.audio.replace_enabled;
}

static uint32_t g_dump_static_enabled = 1;
static uint32_t g_dump_streams_enabled = 1;
/* Published with the replacement index; read lock-free on the APU hot path. */
static uint32_t g_stream_match_count;
/* Static/resident-buffer prefix matches are kept separate from SSL matches so
 * identical intros cannot cross-match between transport classes. */
static uint32_t g_static_match_count;
/* Number of unique verified transport-agnostic source windows. */
static uint32_t g_consumed_window_unique_count;

bool xemu_audio_packs_should_prepare_voice(void)
{
    if (g_config.audio.dump_enabled &&
        (qatomic_read(&g_dump_static_enabled) ||
         qatomic_read(&g_dump_streams_enabled))) {
        return true;
    }
    return g_config.audio.replace_enabled &&
           xemu_audio_packs_replacements_available();
}

bool xemu_audio_packs_should_prepare_static_voice(void)
{
    return (g_config.audio.dump_enabled &&
            qatomic_read(&g_dump_static_enabled)) ||
           (g_config.audio.replace_enabled &&
            xemu_audio_packs_replacements_available());
}

bool xemu_audio_packs_should_dump_streams(void)
{
    return g_config.audio.dump_enabled &&
           qatomic_read(&g_dump_streams_enabled);
}

bool xemu_audio_packs_should_prepare_stream_voice(void)
{
    return xemu_audio_packs_should_dump_streams() ||
           (g_config.audio.replace_enabled &&
            (qatomic_read(&g_stream_match_count) != 0 ||
             qatomic_read(&g_consumed_window_unique_count) != 0));
}

void xemu_audio_packs_set_dump_categories(bool dump_static, bool dump_streams)
{
    qatomic_set(&g_dump_static_enabled, dump_static ? 1u : 0u);
    qatomic_set(&g_dump_streams_enabled, dump_streams ? 1u : 0u);
    if (!dump_streams) {
        xemu_audio_packs_finish_stream_dumps();
    }
}

/*
 * Voice processing is parallel: xemu dispatches voices to several
 * mcpx.voice_worker threads.  Asset-cache mutation therefore needs a lock.
 * Per-voice playback state remains lock-free because a hardware voice is
 * processed by only one worker in a frame, and cache invalidation is deferred
 * to the VP frame boundary while all workers are idle.
 */
/*
 * Replacement WAVs are decoded before they become visible to the APU.  The
 * cache keeps the WAV's own mono/stereo topology; first use only performs a
 * cheap in-memory channel adaptation to the Xbox source voice.  This prevents
 * SDL_LoadWAV/SDL_ConvertAudioSamples and filesystem I/O from ever blocking a
 * mcpx.voice_worker thread and starving the host audio stream.
 */
typedef struct AudioPreloadedWav {
    int refcount;
    float *samples;
    uint32_t frames;
    uint32_t sample_rate;
    unsigned int channels;
} AudioPreloadedWav;

typedef struct AudioReplacement {
    float *samples;              /* Interleaved, converted to source channels. */
    uint32_t frames;
    uint32_t sample_rate;
    unsigned int channels;
    /* Non-NULL when samples directly alias the immutable warm cache. */
    AudioPreloadedWav *shared_wav;
} AudioReplacement;

typedef struct AudioReplacementVariant {
    char *path;
    bool load_in_progress;
    bool load_failed;
    AudioReplacement *audio;

    /* Source-rate materialization is used by passthrough replacement modes
     * (SSL and consumed-window matching). Native/static replacement keeps its
     * historical encoded-rate materialization and SRC ratio. */
    bool source_rate_load_in_progress;
    bool source_rate_load_failed;
    AudioReplacement *source_rate_audio;
} AudioReplacementVariant;

typedef struct AudioReplacementIndexVariant {
    uint32_t number;
    char *path;
} AudioReplacementIndexVariant;

typedef struct AudioReplacementIndexGroup {
    char *single_path;           /* <hash>.wav */
    GPtrArray *variants;         /* AudioReplacementIndexVariant* */

    /*
     * Stable source metadata loaded from dump/replacement JSON.  The rate is
     * deliberately separate from the WAV's own sample rate: the former is the
     * Xbox source voice's normal pitch reference, while the latter is simply
     * how the replacement file itself is encoded.
     */
    uint32_t reference_rate;
    uint32_t source_frames;
    unsigned int source_channels;
    bool source_streaming;
    uint32_t stream_fingerprint_mask;
    uint64_t stream_fingerprints[AUDIO_STREAM_FINGERPRINT_TIER_COUNT];
} AudioReplacementIndexGroup;

typedef struct AudioStreamMatchEntry {
    uint64_t source_hash;
    bool ambiguous;
} AudioStreamMatchEntry;

typedef struct AudioAsset {
    uint64_t hash;
    uint32_t canonical_rate;     /* Stable source pitch reference for this PCM.
                                  Metadata wins; first observation is fallback. */
    uint32_t source_frames;
    bool loop;
    uint32_t loop_start;
    char source_format[24];

    bool replacements_checked;
    GPtrArray *replacements;     /* AudioReplacementVariant* */

    /* Per-asset replacement policy, loaded once from <hash>.json. */
    bool retrigger_policy_checked;
    bool restart_on_retrigger;

    /*
     * Monophonic retrigger epoch for restart mode.  Each new playback of this
     * source hash increments the epoch.  Older hardware voices notice the
     * mismatch and terminate their replacement on their next service pass, so
     * the newest trigger wins without workers mutating each other's voice
     * state.
     */
    uint32_t retrigger_epoch;
} AudioAsset;

typedef struct AudioVoiceState {
    bool prepared;
    bool replacement_active;
    bool finished;
    bool streaming;
    AudioAsset *asset;
    AudioReplacement *replacement;
    uint64_t cursor;
    uint64_t stream_source_cursor;
    uint32_t guest_cbo;
    uint32_t replacement_loop_start;
    float replacement_rate_scale;
    uint32_t retrigger_epoch;
    uint32_t guest_cbo_event_seen;

    /* Source metadata needed by the post-decode consumed-source matcher. */
    uint32_t observed_source_rate;
    unsigned int source_channels;
    char observed_source_format[24];

    /* Passthrough mode keeps the guest/native source reader authoritative and
     * only substitutes decoded samples after consumption. This is mandatory
     * for software rings because taking over source fetch/CBO would change the
     * game's buffer timing. */
    bool passthrough_source;

    /* Consumed-source identity is kept separately from hardware cursor state.
     * It lets a persistent ring reacquire a later sound (including the same
     * logical source after the source offset wraps backwards) without needing
     * a VOICE_OFF/SetCurrentPosition boundary from the guest. */
    uint64_t consumed_source_hash;
    uint32_t consumed_match_offset;
    uint32_t consumed_last_seen_offset;
} AudioVoiceState;

typedef struct AudioDumpJob {
    AudioDumpJobKind kind;
    char *wav_path;
    char *json_path;
    char *dump_dir;
    int16_t *pcm;
    uint32_t frames;
    uint32_t sample_rate;
    unsigned int channels;
    uint64_t hash;
    uint64_t stream_session;
    uint64_t stream_segment_signature;
    bool stream_wrap_hint;
    uint32_t stream_segments;
    uint32_t stream_fingerprint_mask;
    uint64_t stream_fingerprints[AUDIO_STREAM_FINGERPRINT_TIER_COUNT];
    bool loop;
    bool streaming;
    uint32_t loop_start;
    uint32_t loop_end;          /* Exclusive frame index in published WAV. */
    uint64_t observed_frames;   /* Frames seen before exact loop compaction. */
    uint32_t loop_repetitions_observed;
    char loop_detection[32];
    char source_format[24];
} AudioDumpJob;

typedef struct AudioStreamCapture {
    /* Dump-session state. */
    bool active;
    uint64_t session;
    uint32_t sample_rate;
    unsigned int channels;
    char source_format[24];

    /* Segment activation tracker, shared by dumping and replacement matching. */
    bool have_segment;
    uint64_t last_segment_signature;
    uint32_t last_cbo;
    bool idle_pending;
    uint32_t idle_passes;

    /* Incremental prefix matcher used until a stream is identified. The
     * XXH3 state is created once per hardware voice at feature init; only the
     * completed tier digests are retained, avoiding a large PCM prefix buffer
     * on audio workers. */
    XXH3_state_t *match_hash_state;
    uint32_t match_hash_frames;
    uint32_t match_fingerprint_mask;
    uint64_t match_fingerprints[AUDIO_STREAM_FINGERPRINT_TIER_COUNT];
    uint32_t match_tried_mask;
    bool match_exhausted;
    bool replacement_identified;
    uint64_t matched_source_hash;
    uint64_t source_frames_seen;
} AudioStreamCapture;

typedef struct AudioStreamSegmentRecord {
    uint64_t start_frame;
    uint32_t frames;
    uint64_t pcm_hash;
    uint64_t segment_signature;
    bool has_signal;
} AudioStreamSegmentRecord;

typedef struct AudioStreamWriter {
    FILE *file;
    char *tmp_path;
    char *dump_dir;
    uint64_t frames;          /* Frames physically retained in temp WAV. */
    uint64_t observed_frames; /* Includes repetitions suppressed online. */
    uint32_t segments;        /* Observed source segment activations. */
    uint32_t sample_rate;
    unsigned int channels;
    char source_format[24];
    GArray *segment_records; /* Retained AudioStreamSegmentRecord entries. */

    /* After three exact cycles are proven, complete later repetitions can be
     * suppressed online. A mismatch exits suppression and is retained as
     * outro/new unique source data. */
    bool loop_suppression_active;
    uint32_t loop_cycle_first_record;
    uint32_t loop_cycle_record_count;
    uint32_t loop_cycle_phase;
    uint32_t loop_repetitions_suppressed;
    bool saw_wrap_hint;
} AudioStreamWriter;

/* One verified source landmark inside an original dumped WAV. Buckets are
 * keyed by the cheap rolling anchor; verify_hash is the exact portable XXH3
 * 16-frame fingerprint. Repeated/identical windows are marked ambiguous so
 * silence and duplicated phrases can never guess an offset. */
typedef struct AudioConsumedWindowEntry {
    uint64_t verify_hash;
    uint64_t source_hash;
    uint32_t source_offset;
    bool ambiguous;
} AudioConsumedWindowEntry;

typedef struct AudioConsumedWindowBucket {
    GArray *entries; /* AudioConsumedWindowEntry */
} AudioConsumedWindowBucket;

typedef struct AudioConsumedVoiceState {
    uint64_t frame_tokens[AUDIO_CONSUMED_WINDOW_FRAMES];
    int16_t pcm[AUDIO_CONSUMED_WINDOW_FRAMES * 2];
    uint32_t write_pos;
    uint32_t valid_frames;
    unsigned int channels;
    uint64_t rolling_hash;
    uint64_t total_frames;
} AudioConsumedVoiceState;

/* UI thread swaps immutable path/index objects under this short mutex. */
static GMutex g_index_lock;
static char *g_dump_dir;
static char *g_replace_dir;
static GHashTable *g_replace_index; /* 16-hex stem -> AudioReplacementIndexGroup* */
/* Stream-prefix fingerprint -> AudioStreamMatchEntry. Immutable per generation. */
static GHashTable *g_stream_match_index;
/* Static/resident source-prefix fingerprint -> AudioStreamMatchEntry. */
static GHashTable *g_static_match_index;
/* Rolling source-window anchor -> AudioConsumedWindowBucket. Derived from
 * original dump WAVs during replacement-index rebuild; immutable while live. */
static GHashTable *g_consumed_window_index;
/* Lock-free negative filter for the immutable consumed-window index. Most
 * decoded windows miss here and never contend on g_index_lock. */
static uint32_t g_consumed_anchor_bloom[AUDIO_CONSUMED_BLOOM_WORDS];
/* Absolute WAV path/rate -> AudioPreloadedWav. Built outside voice workers and
 * swapped atomically with g_replace_index. */
static GHashTable *g_preload_cache;
static GHashTable *g_dumped_set;    /* 16-hex stem */
static uint32_t g_title_id;
static bool g_paths_valid;
static uint32_t g_index_generation;
static uint32_t g_replacement_count; /* lock-free APU fast-path hint */
static uint32_t g_variant_sequence;   /* one increment per randomized playback */

/* APU-thread-only state. */
static GHashTable *g_asset_cache;   /* guint64* -> AudioAsset* */
static AudioVoiceState g_voices[MCPX_HW_MAX_VOICES];
static AudioConsumedVoiceState g_consumed_voices[MCPX_HW_MAX_VOICES];
/* Exact guest FE writes to SET_VOICE_BUF_CBO are recorded out-of-band so the
 * FE/MMIO path never mutates a worker-owned AudioVoiceState directly. */
static uint32_t g_guest_cbo_event_seq[MCPX_HW_MAX_VOICES];
static uint32_t g_guest_cbo_event_value[MCPX_HW_MAX_VOICES];
static uint32_t g_apu_generation;
static GMutex g_asset_lock;           /* protects asset-cache mutation */
static GCond g_asset_load_cond;       /* first-use in-memory materialization */

/* Background dump writer: no disk compression/I/O on the 1500 Hz APU path. */
static GAsyncQueue *g_dump_queue;
static GThread *g_dump_thread;
static bool g_dump_worker_stopping;
static GMutex g_dump_worker_lock;     /* multiple voice workers may enqueue */

/* Streaming SSL capture stays feature-owned.  The existing VP hook sees each
 * hardware voice before source samples are consumed, so no native APU changes
 * are required.  A short mutex only protects session bookkeeping between the
 * voice worker and FE/reset paths; disk I/O remains on g_dump_thread. */
static GMutex g_stream_capture_lock;
static AudioStreamCapture g_stream_captures[MCPX_HW_MAX_VOICES];
static uint64_t g_stream_session_sequence;

static void audio_stream_match_generation_changed(void);

static void hash_key(uint64_t hash, char key[AUDIO_HASH_KEY_LEN])
{
    snprintf(key, AUDIO_HASH_KEY_LEN, "%016" PRIx64, hash);
}

/*
 * Hardware-driven replacement retrigger/seek behavior is automatic.  A tiny
 * optional JSON sidecar remains only for the stronger cross-hardware-voice
 * monophonic override.  We only need one field for that override, so pulling a
 * full JSON parser into the 1500 Hz audio subsystem would be needless.  The
 * reader accepts metadata emitted by this file and a minimal pack sidecar such
 * as:
 *
 *     { "retrigger_mode": "restart" }
 *
 * Retrigger/seek handling is automatic by default: audio-io follows actual
 * guest CBO/SetCurrentPosition writes for every active static replacement.
 * This means titles that rewind an already-playing hardware voice (for example
 * rapid-fire weapon sounds) need no JSON sidecar at all.
 *
 * Optional policy values remain for compatibility:
 *   "auto" or "natural" -> hardware-driven behavior only (default)
 *   "restart"           -> hardware-driven behavior plus forced monophonic
 *                          newest-instance-wins behavior across hardware voices
 *                          for the same source hash.
 */
static bool read_retrigger_mode_file(const char *path, bool *restart)
{
    if (!path || !restart) {
        return false;
    }

    char *contents = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &contents, &len, NULL) || !contents) {
        g_free(contents);
        return false;
    }

    const char *key = "\"retrigger_mode\"";
    char *p = strstr(contents, key);
    if (!p) {
        g_free(contents);
        return false;
    }
    p += strlen(key);
    while (*p && g_ascii_isspace(*p)) {
        p++;
    }
    if (*p != ':') {
        g_free(contents);
        return false;
    }
    p++;
    while (*p && g_ascii_isspace(*p)) {
        p++;
    }
    if (*p != '"') {
        g_free(contents);
        return false;
    }
    p++;

    const char *end = strchr(p, '"');
    if (!end) {
        g_free(contents);
        return false;
    }
    gsize value_len = end - p;
    if (value_len == strlen("restart") &&
        g_ascii_strncasecmp(p, "restart", value_len) == 0) {
        *restart = true;
        g_free(contents);
        return true;
    }
    if ((value_len == strlen("natural") &&
         g_ascii_strncasecmp(p, "natural", value_len) == 0) ||
        (value_len == strlen("auto") &&
         g_ascii_strncasecmp(p, "auto", value_len) == 0)) {
        *restart = false;
        g_free(contents);
        return true;
    }

    fprintf(stderr,
            "mcpx: audio-io: ignoring unknown retrigger_mode in %s "
            "(expected auto, natural, or restart)\n", path);
    g_free(contents);
    return false;
}

static bool read_sample_rate_file(const char *path, uint32_t *sample_rate)
{
    if (!path || !sample_rate) {
        return false;
    }

    char *contents = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &contents, &len, NULL) || !contents) {
        g_free(contents);
        return false;
    }

    const char *key = "\"sample_rate\"";
    char *p = strstr(contents, key);
    if (!p) {
        g_free(contents);
        return false;
    }
    p += strlen(key);
    while (*p && g_ascii_isspace(*p)) {
        p++;
    }
    if (*p != ':') {
        g_free(contents);
        return false;
    }
    p++;
    while (*p && g_ascii_isspace(*p)) {
        p++;
    }

    char *end = NULL;
    guint64 rate = g_ascii_strtoull(p, &end, 10);
    bool ok = end != p && rate >= 100 && rate <= 384000;
    if (ok) {
        *sample_rate = (uint32_t)rate;
    }
    g_free(contents);
    return ok;
}


typedef struct AudioSourceMetadata {
    bool rate_valid;
    uint32_t rate;
    bool frames_valid;
    uint32_t frames;
    bool channels_valid;
    unsigned int channels;
    bool streaming_valid;
    bool streaming;
    uint32_t stream_fingerprint_mask;
    uint64_t stream_fingerprints[AUDIO_STREAM_FINGERPRINT_TIER_COUNT];
} AudioSourceMetadata;

static const char *json_find_value(const char *contents, const char *key)
{
    if (!contents || !key) {
        return NULL;
    }
    g_autofree char *needle = g_strdup_printf("\"%s\"", key);
    const char *p = strstr(contents, needle);
    if (!p) {
        return NULL;
    }
    p += strlen(needle);
    while (*p && g_ascii_isspace(*p)) {
        p++;
    }
    if (*p++ != ':') {
        return NULL;
    }
    while (*p && g_ascii_isspace(*p)) {
        p++;
    }
    return p;
}

static bool json_read_u32(const char *contents, const char *key, uint32_t *out)
{
    const char *p = json_find_value(contents, key);
    if (!p || !out) {
        return false;
    }
    char *end = NULL;
    guint64 value = g_ascii_strtoull(p, &end, 10);
    if (end == p || value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool json_read_bool_value(const char *contents, const char *key, bool *out)
{
    const char *p = json_find_value(contents, key);
    if (!p || !out) {
        return false;
    }
    if (g_ascii_strncasecmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (g_ascii_strncasecmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool json_read_hex64_string(const char *contents, const char *key,
                                   uint64_t *out)
{
    const char *p = json_find_value(contents, key);
    if (!p || !out || *p++ != '"') {
        return false;
    }
    char hex[17];
    for (unsigned int i = 0; i < 16; i++) {
        if (!g_ascii_isxdigit(p[i])) {
            return false;
        }
        hex[i] = p[i];
    }
    if (p[16] != '"') {
        return false;
    }
    hex[16] = 0;
    char *end = NULL;
    guint64 value = g_ascii_strtoull(hex, &end, 16);
    if (!end || *end) {
        return false;
    }
    *out = value;
    return true;
}

/*
 * Stream fingerprints deliberately normalize each signed PCM16 sample to
 * little-endian bytes before hashing.  Unlike hashing an int16_t array
 * directly, this keeps stream replacement packs portable across host
 * endianness while still matching the exact decoded source samples.
 */
static uint64_t audio_stream_fingerprint_tag(uint32_t frames,
                                             unsigned int channels)
{
    uint8_t tag[8] = {
        AUDIO_STREAM_FINGERPRINT_VERSION & 0xff,
        channels & 0xff,
        frames & 0xff,
        (frames >> 8) & 0xff,
        (frames >> 16) & 0xff,
        (frames >> 24) & 0xff,
        0, 0,
    };
    return XXH3_64bits(tag, sizeof(tag));
}

static XXH_errorcode audio_stream_hash_update_pcm(XXH3_state_t *state,
                                                   const int16_t *pcm,
                                                   uint32_t frames,
                                                   unsigned int channels)
{
    if (!state || !pcm || frames == 0 || channels < 1 || channels > 2) {
        return XXH_ERROR;
    }

#if HOST_BIG_ENDIAN
    /* Keep pack identity little-endian without large worker-stack scratch. */
    uint8_t normalized[512];
    uint32_t done = 0;
    while (done < frames) {
        uint32_t chunk_frames = MIN(frames - done,
                                    (uint32_t)(sizeof(normalized) /
                                               (channels * sizeof(int16_t))));
        size_t sample_count = (size_t)chunk_frames * channels;
        const int16_t *src = &pcm[(size_t)done * channels];
        for (size_t i = 0; i < sample_count; i++) {
            uint16_t sample = (uint16_t)src[i];
            normalized[i * 2] = sample & 0xff;
            normalized[i * 2 + 1] = sample >> 8;
        }
        if (XXH3_64bits_update(state, normalized, sample_count * 2) ==
            XXH_ERROR) {
            return XXH_ERROR;
        }
        done += chunk_frames;
    }
    return XXH_OK;
#else
    size_t sample_count = (size_t)frames * channels;
    return XXH3_64bits_update(state, pcm, sample_count * sizeof(*pcm));
#endif
}

static uint64_t audio_stream_fingerprint(const int16_t *pcm, uint32_t frames,
                                         unsigned int channels)
{
    if (!pcm || frames == 0 || channels < 1 || channels > 2 ||
        frames > AUDIO_STREAM_MATCH_MAX_FRAMES) {
        return 0;
    }

#if HOST_BIG_ENDIAN
    /* Index/dump-side one-shot helper. The runtime matcher below is fully
     * incremental and never retains this full prefix. */
    XXH3_state_t *state = XXH3_createState();
    if (!state || XXH3_64bits_reset(state) == XXH_ERROR ||
        audio_stream_hash_update_pcm(state, pcm, frames, channels) == XXH_ERROR) {
        if (state) {
            XXH3_freeState(state);
        }
        return 0;
    }
    uint64_t content_hash = XXH3_64bits_digest(state);
    XXH3_freeState(state);
#else
    size_t sample_count = (size_t)frames * channels;
    uint64_t content_hash =
        XXH3_64bits(pcm, sample_count * sizeof(*pcm));
#endif
    return content_hash ^ audio_stream_fingerprint_tag(frames, channels);
}


static uint64_t audio_consumed_frame_token(const int16_t *frame,
                                           unsigned int channels)
{
    uint64_t left = (uint16_t)frame[0];
    uint64_t right = channels == 2 ? (uint16_t)frame[1] : 0;
    uint64_t token = left | (right << 16) | ((uint64_t)channels << 40);
    /* Prevent long zero/silence runs from degenerating to an all-zero
     * polynomial state while preserving exact deterministic identity. */
    return token + UINT64_C(0x9e3779b97f4a7c15);
}

static bool audio_consumed_window_has_signal(const int16_t *pcm,
                                             unsigned int channels)
{
    /* Exact digital silence and near-zero codec padding are not useful source
     * landmarks and are frequently shared by hundreds of assets. */
    for (unsigned int i = 0;
         i < AUDIO_CONSUMED_WINDOW_FRAMES * channels; i++) {
        int sample = pcm[i];
        if (sample > 8 || sample < -8) {
            return true;
        }
    }
    return false;
}

static void free_consumed_window_bucket(gpointer p)
{
    AudioConsumedWindowBucket *bucket = p;
    if (!bucket) {
        return;
    }
    if (bucket->entries) {
        g_array_free(bucket->entries, TRUE);
    }
    g_free(bucket);
}

static void add_consumed_window_entry(GHashTable *table, uint64_t anchor,
                                      uint64_t verify_hash,
                                      uint64_t source_hash,
                                      uint32_t source_offset,
                                      uint32_t *unique_count)
{
    AudioConsumedWindowBucket *bucket = g_hash_table_lookup(table, &anchor);
    if (!bucket) {
        guint64 *key = g_new(guint64, 1);
        *key = anchor;
        bucket = g_new0(AudioConsumedWindowBucket, 1);
        bucket->entries = g_array_new(FALSE, FALSE,
                                      sizeof(AudioConsumedWindowEntry));
        g_hash_table_insert(table, key, bucket);
    }

    for (guint i = 0; i < bucket->entries->len; i++) {
        AudioConsumedWindowEntry *entry = &g_array_index(
            bucket->entries, AudioConsumedWindowEntry, i);
        if (entry->verify_hash != verify_hash) {
            continue;
        }
        if (entry->source_hash == source_hash &&
            entry->source_offset == source_offset) {
            return;
        }
        if (!entry->ambiguous) {
            entry->ambiguous = true;
            if (unique_count && *unique_count > 0) {
                (*unique_count)--;
            }
        }
        return;
    }

    AudioConsumedWindowEntry entry = {
        .verify_hash = verify_hash,
        .source_hash = source_hash,
        .source_offset = source_offset,
    };
    g_array_append_val(bucket->entries, entry);
    if (unique_count) {
        (*unique_count)++;
    }
}


static uint32_t audio_consumed_bloom_bit1(uint64_t anchor)
{
    return (uint32_t)((anchor >> 4) & (AUDIO_CONSUMED_BLOOM_BITS - 1u));
}

static uint32_t audio_consumed_bloom_bit2(uint64_t anchor)
{
    uint64_t mixed = anchor ^ (anchor >> 29) ^ (anchor >> 47);
    return (uint32_t)(mixed & (AUDIO_CONSUMED_BLOOM_BITS - 1u));
}

static bool audio_consumed_anchor_maybe_present(uint64_t anchor)
{
    uint32_t b1 = audio_consumed_bloom_bit1(anchor);
    uint32_t b2 = audio_consumed_bloom_bit2(anchor);
    uint32_t w1 = qatomic_read(&g_consumed_anchor_bloom[b1 >> 5]);
    uint32_t w2 = qatomic_read(&g_consumed_anchor_bloom[b2 >> 5]);
    return (w1 & (1u << (b1 & 31))) && (w2 & (1u << (b2 & 31)));
}

static void audio_consumed_publish_bloom(GHashTable *table)
{
    uint32_t *next = g_new0(uint32_t, AUDIO_CONSUMED_BLOOM_WORDS);
    if (table) {
        GHashTableIter it;
        gpointer key_ptr;
        g_hash_table_iter_init(&it, table);
        while (g_hash_table_iter_next(&it, &key_ptr, NULL)) {
            uint64_t anchor = *(const uint64_t *)key_ptr;
            uint32_t b1 = audio_consumed_bloom_bit1(anchor);
            uint32_t b2 = audio_consumed_bloom_bit2(anchor);
            next[b1 >> 5] |= 1u << (b1 & 31);
            next[b2 >> 5] |= 1u << (b2 & 31);
        }
    }
    for (uint32_t i = 0; i < AUDIO_CONSUMED_BLOOM_WORDS; i++) {
        qatomic_set(&g_consumed_anchor_bloom[i], next[i]);
    }
    g_free(next);
}

static void reset_consumed_voice_state(unsigned int voice)
{
    if (voice >= MCPX_HW_MAX_VOICES) {
        return;
    }
    memset(&g_consumed_voices[voice], 0, sizeof(g_consumed_voices[voice]));
}

static bool stream_match_feed_locked(AudioStreamCapture *sc,
                                     const int16_t *pcm, uint32_t frames,
                                     unsigned int channels)
{
    if (!sc || !pcm || frames == 0 || channels < 1 || channels > 2 ||
        sc->match_hash_frames >= AUDIO_STREAM_MATCH_MAX_FRAMES) {
        return false;
    }
    if (!sc->match_hash_state) {
        sc->match_exhausted = true;
        return false;
    }

    if (sc->match_hash_frames == 0 &&
        XXH3_64bits_reset(sc->match_hash_state) == XXH_ERROR) {
        sc->match_exhausted = true;
        return false;
    }

    uint32_t consumed = 0;
    while (consumed < frames &&
           sc->match_hash_frames < AUDIO_STREAM_MATCH_MAX_FRAMES) {
        guint tier_index = 0;
        while (tier_index < AUDIO_STREAM_FINGERPRINT_TIER_COUNT &&
               audio_stream_fingerprint_tiers[tier_index] <=
                   sc->match_hash_frames) {
            tier_index++;
        }
        if (tier_index >= AUDIO_STREAM_FINGERPRINT_TIER_COUNT) {
            break;
        }

        uint32_t tier_frames = audio_stream_fingerprint_tiers[tier_index];
        uint32_t take = MIN(frames - consumed,
                            tier_frames - sc->match_hash_frames);
        if (audio_stream_hash_update_pcm(
                sc->match_hash_state,
                &pcm[(size_t)consumed * channels], take, channels) ==
            XXH_ERROR) {
            sc->match_exhausted = true;
            return false;
        }
        sc->match_hash_frames += take;
        consumed += take;

        if (sc->match_hash_frames == tier_frames) {
            uint64_t content_hash = XXH3_64bits_digest(sc->match_hash_state);
            sc->match_fingerprints[tier_index] =
                content_hash ^
                audio_stream_fingerprint_tag(tier_frames, channels);
            sc->match_fingerprint_mask |= 1u << tier_index;
        }
    }
    return true;
}

static void read_source_metadata_file(const char *path, AudioSourceMetadata *meta)
{
    if (!path || !meta) {
        return;
    }
    char *contents = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &contents, &len, NULL) || !contents) {
        g_free(contents);
        return;
    }

    uint32_t u32;
    bool b;
    if (!meta->rate_valid && json_read_u32(contents, "sample_rate", &u32) &&
        u32 >= 100 && u32 <= 384000) {
        meta->rate = u32;
        meta->rate_valid = true;
    }
    if (!meta->frames_valid && json_read_u32(contents, "frames", &u32) &&
        u32 > 0) {
        meta->frames = u32;
        meta->frames_valid = true;
    }
    if (!meta->channels_valid && json_read_u32(contents, "channels", &u32) &&
        u32 >= 1 && u32 <= 2) {
        meta->channels = u32;
        meta->channels_valid = true;
    }
    if (!meta->streaming_valid &&
        json_read_bool_value(contents, "streaming", &b)) {
        meta->streaming = b;
        meta->streaming_valid = true;
    }

    for (guint i = 0; i < AUDIO_STREAM_FINGERPRINT_TIER_COUNT; i++) {
        if (meta->stream_fingerprint_mask & (1u << i)) {
            continue;
        }
        char key[48];
        uint64_t fingerprint;
        snprintf(key, sizeof(key), "source_fingerprint_%u",
                 audio_stream_fingerprint_tiers[i]);
        if (!json_read_hex64_string(contents, key, &fingerprint)) {
            /* V1 compatibility: packetized stream dumps used the older key. */
            snprintf(key, sizeof(key), "stream_fingerprint_%u",
                     audio_stream_fingerprint_tiers[i]);
            if (!json_read_hex64_string(contents, key, &fingerprint)) {
                continue;
            }
        }
        meta->stream_fingerprints[i] = fingerprint;
        meta->stream_fingerprint_mask |= 1u << i;
    }
    g_free(contents);
}

static bool load_pcm16_source_prefix(const char *path, int16_t **out_pcm,
                                     uint32_t *out_frames,
                                     unsigned int *out_channels,
                                     uint32_t *out_total_frames,
                                     uint32_t *out_rate)
{
    if (!path || !out_pcm || !out_frames || !out_channels) {
        return false;
    }

    SDL_AudioSpec src_spec;
    Uint8 *src = NULL;
    Uint32 src_len = 0;
    if (!SDL_LoadWAV(path, &src_spec, &src, &src_len) ||
        src_spec.freq <= 0 || src_spec.channels < 1 || src_spec.channels > 2) {
        SDL_free(src);
        return false;
    }

    SDL_AudioSpec dst_spec = {
        .freq = src_spec.freq,
        .format = SDL_AUDIO_S16LE,
        .channels = src_spec.channels,
    };
    Uint8 *dst = NULL;
    int dst_len = 0;
    bool ok = SDL_ConvertAudioSamples(&src_spec, src, (int)src_len,
                                      &dst_spec, &dst, &dst_len);
    SDL_free(src);
    if (!ok || !dst || dst_len <= 0) {
        SDL_free(dst);
        return false;
    }

    unsigned int channels = dst_spec.channels;
    size_t frame_bytes = channels * sizeof(int16_t);
    uint64_t total_frames64 = (uint64_t)dst_len / frame_bytes;
    uint32_t total_frames = total_frames64 > UINT32_MAX
        ? UINT32_MAX : (uint32_t)total_frames64;
    uint32_t frames = MIN((uint64_t)AUDIO_STREAM_MATCH_MAX_FRAMES,
                          total_frames64);
    if (frames == 0) {
        SDL_free(dst);
        return false;
    }

    int16_t *pcm = g_try_malloc_n((size_t)frames * channels, sizeof(*pcm));
    if (!pcm) {
        SDL_free(dst);
        return false;
    }
    for (size_t i = 0; i < (size_t)frames * channels; i++) {
        const uint8_t *sample = &dst[i * 2];
        pcm[i] = (int16_t)((uint16_t)sample[0] | (uint16_t)sample[1] << 8);
    }
    SDL_free(dst);

    *out_pcm = pcm;
    *out_frames = frames;
    *out_channels = channels;
    if (out_total_frames) {
        *out_total_frames = total_frames;
    }
    if (out_rate) {
        *out_rate = (uint32_t)dst_spec.freq;
    }
    return true;
}


static bool load_pcm16_source_wav(const char *path, uint32_t max_frames,
                                  int16_t **out_pcm, uint32_t *out_frames,
                                  unsigned int *out_channels,
                                  uint32_t *out_rate)
{
    if (!path || !out_pcm || !out_frames || !out_channels) {
        return false;
    }

    SDL_AudioSpec src_spec;
    Uint8 *src = NULL;
    Uint32 src_len = 0;
    if (!SDL_LoadWAV(path, &src_spec, &src, &src_len) ||
        src_spec.freq <= 0 || src_spec.channels < 1 || src_spec.channels > 2) {
        SDL_free(src);
        return false;
    }

    SDL_AudioSpec dst_spec = {
        .freq = src_spec.freq,
        .format = SDL_AUDIO_S16LE,
        .channels = src_spec.channels,
    };
    Uint8 *dst = NULL;
    int dst_len = 0;
    bool ok = SDL_ConvertAudioSamples(&src_spec, src, (int)src_len,
                                      &dst_spec, &dst, &dst_len);
    SDL_free(src);
    if (!ok || !dst || dst_len <= 0) {
        SDL_free(dst);
        return false;
    }

    unsigned int channels = dst_spec.channels;
    size_t frame_bytes = channels * sizeof(int16_t);
    uint64_t total_frames = (uint64_t)dst_len / frame_bytes;
    if (max_frames) {
        total_frames = MIN(total_frames, (uint64_t)max_frames);
    }
    if (total_frames == 0 || total_frames > UINT32_MAX) {
        SDL_free(dst);
        return false;
    }

    uint32_t frames = (uint32_t)total_frames;
    int16_t *pcm = g_try_malloc_n((size_t)frames * channels, sizeof(*pcm));
    if (!pcm) {
        SDL_free(dst);
        return false;
    }
    for (size_t i = 0; i < (size_t)frames * channels; i++) {
        const uint8_t *sample = &dst[i * 2];
        pcm[i] = (int16_t)((uint16_t)sample[0] | (uint16_t)sample[1] << 8);
    }
    SDL_free(dst);

    *out_pcm = pcm;
    *out_frames = frames;
    *out_channels = channels;
    if (out_rate) {
        *out_rate = (uint32_t)dst_spec.freq;
    }
    return true;
}

static AudioPreloadedWav *preloaded_wav_ref(AudioPreloadedWav *w)
{
    if (w) {
        g_atomic_int_inc(&w->refcount);
    }
    return w;
}

static void preloaded_wav_unref(AudioPreloadedWav *w)
{
    if (!w || !g_atomic_int_dec_and_test(&w->refcount)) {
        return;
    }
    SDL_free(w->samples);
    g_free(w);
}

static void free_replacement(AudioReplacement *r)
{
    if (!r) {
        return;
    }
    if (r->shared_wav) {
        preloaded_wav_unref(r->shared_wav);
    } else {
        SDL_free(r->samples);
    }
    g_free(r);
}

static void free_preloaded_wav(gpointer p)
{
    preloaded_wav_unref((AudioPreloadedWav *)p);
}

static void free_replacement_variant(gpointer p)
{
    AudioReplacementVariant *v = p;
    if (!v) {
        return;
    }
    g_free(v->path);
    free_replacement(v->audio);
    free_replacement(v->source_rate_audio);
    g_free(v);
}

static void free_index_variant(gpointer p)
{
    AudioReplacementIndexVariant *v = p;
    if (!v) {
        return;
    }
    g_free(v->path);
    g_free(v);
}

static void free_index_group(gpointer p)
{
    AudioReplacementIndexGroup *g = p;
    if (!g) {
        return;
    }
    g_free(g->single_path);
    if (g->variants) {
        g_ptr_array_free(g->variants, TRUE);
    }
    g_free(g);
}

static void free_asset(gpointer p)
{
    AudioAsset *a = p;
    if (!a) {
        return;
    }
    if (a->replacements) {
        g_ptr_array_free(a->replacements, TRUE);
    }
    g_free(a);
}

static void clear_apu_cache(void)
{
    /*
     * Callers are lifecycle/frame-boundary paths where voice workers are idle.
     * The lock still protects against concurrent first-use cache mutation and
     * makes that invariant explicit.
     */
    g_mutex_lock(&g_asset_lock);
    memset(g_voices, 0, sizeof(g_voices));
    for (unsigned int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        reset_consumed_voice_state(v);
        g_voices[v].guest_cbo_event_seen =
            qatomic_read(&g_guest_cbo_event_seq[v]);
    }
    if (g_asset_cache) {
        g_hash_table_destroy(g_asset_cache);
    }
    g_asset_cache = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                          g_free, free_asset);
    g_mutex_unlock(&g_asset_lock);
}

static uint32_t current_generation(void)
{
    /* Hot path: one atomic load per VP frame, no mutex when unchanged. */
    return qatomic_read(&g_index_generation);
}

void xemu_audio_packs_frame_sync(void)
{
    /*
     * IMPORTANT: this runs before voice_work_dispatch(), while every voice
     * worker is idle.  V1/Hotfix2 used to clear g_asset_cache from whichever
     * worker first noticed an index-generation change.  Another worker could
     * simultaneously be reading an AudioAsset/AudioReplacement from that
     * table, producing a real use-after-free and intermittent emulator crash.
     */
    uint32_t generation = current_generation();
    if (generation == g_apu_generation) {
        return;
    }
    clear_apu_cache();
    audio_stream_match_generation_changed();
    g_apu_generation = generation;
}

static bool parse_hash_wav_name(const char *name, char key[AUDIO_HASH_KEY_LEN])
{
    size_t len = strlen(name);
    if (len != 20 || g_ascii_strcasecmp(name + 16, ".wav") != 0) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        if (!g_ascii_isxdigit(name[i])) {
            return false;
        }
        key[i] = g_ascii_tolower(name[i]);
    }
    key[16] = 0;
    return true;
}

/*
 * Replacement naming:
 *   <hash>.wav       = the traditional single replacement
 *   <hash>_1.wav     = randomized replacement variant 1
 *   <hash>_2.wav ... = additional randomized variants
 *
 * Any positive decimal suffix is accepted, so a missing number does not make
 * later variants disappear.  If numbered variants and an unsuffixed WAV both
 * exist for the same hash, the numbered pool wins and the unsuffixed WAV is a
 * fallback only when no numbered variants are present.
 */
static bool parse_replacement_wav_name(const char *name,
                                       char key[AUDIO_HASH_KEY_LEN],
                                       uint32_t *variant_number,
                                       bool *numbered)
{
    size_t len = strlen(name);
    if (len < 20 || g_ascii_strcasecmp(name + len - 4, ".wav") != 0) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        if (!g_ascii_isxdigit(name[i])) {
            return false;
        }
        key[i] = g_ascii_tolower(name[i]);
    }
    key[16] = 0;

    if (len == 20) {
        *variant_number = 0;
        *numbered = false;
        return true;
    }
    if (name[16] != '_' || len <= 21) {
        return false;
    }

    const char *digits = name + 17;
    const char *suffix = name + len - 4;
    if (digits >= suffix) {
        return false;
    }
    for (const char *p = digits; p < suffix; p++) {
        if (!g_ascii_isdigit(*p)) {
            return false;
        }
    }

    char *end = NULL;
    guint64 n = g_ascii_strtoull(digits, &end, 10);
    if (end != suffix || n == 0 || n > UINT32_MAX) {
        return false;
    }
    *variant_number = (uint32_t)n;
    *numbered = true;
    return true;
}

static AudioReplacementIndexGroup *get_or_create_index_group(
    GHashTable *table, const char *key)
{
    AudioReplacementIndexGroup *group = g_hash_table_lookup(table, key);
    if (group) {
        return group;
    }
    group = g_new0(AudioReplacementIndexGroup, 1);
    group->variants = g_ptr_array_new_with_free_func(free_index_variant);
    g_hash_table_insert(table, g_strdup(key), group);
    return group;
}

static void index_group_set_single(AudioReplacementIndexGroup *group,
                                   const char *path)
{
    /* Recursive directory order is unspecified; make duplicate resolution
     * deterministic if two subdirectories contain the same singleton hash. */
    if (!group->single_path || g_strcmp0(path, group->single_path) < 0) {
        g_free(group->single_path);
        group->single_path = g_strdup(path);
    }
}

static void index_group_add_variant(AudioReplacementIndexGroup *group,
                                    uint32_t number, const char *path)
{
    for (guint i = 0; i < group->variants->len; i++) {
        AudioReplacementIndexVariant *v = g_ptr_array_index(group->variants, i);
        if (v->number == number) {
            if (g_strcmp0(path, v->path) < 0) {
                g_free(v->path);
                v->path = g_strdup(path);
            }
            return;
        }
    }
    AudioReplacementIndexVariant *v = g_new0(AudioReplacementIndexVariant, 1);
    v->number = number;
    v->path = g_strdup(path);
    g_ptr_array_add(group->variants, v);
}

static void scan_replacement_wavs_recursive(const char *dir, GHashTable *table)
{
    if (!dir || !dir[0]) {
        return;
    }
    GDir *gd = g_dir_open(dir, 0, NULL);
    if (!gd) {
        return;
    }

    const char *name;
    while ((name = g_dir_read_name(gd)) != NULL) {
        g_autofree char *path = g_build_filename(dir, name, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            scan_replacement_wavs_recursive(path, table);
            continue;
        }

        char key[AUDIO_HASH_KEY_LEN];
        uint32_t variant_number;
        bool numbered;
        if (!parse_replacement_wav_name(name, key, &variant_number,
                                         &numbered)) {
            continue;
        }

        AudioReplacementIndexGroup *group =
            get_or_create_index_group(table, key);
        if (numbered) {
            index_group_add_variant(group, variant_number, path);
        } else {
            index_group_set_single(group, path);
        }
    }
    g_dir_close(gd);
}

static uint32_t load_reference_rate_for_group(const char *replace_dir,
                                              const char *dump_dir,
                                              const char *key,
                                              const AudioReplacementIndexGroup *group)
{
    uint32_t rate = 0;

    /*
     * Prefer metadata shipped beside the replacement.  A copied dump JSON is
     * therefore self-contained when a pack moves to another machine.  For
     * nested packs, also check beside the selected WAV before falling back to
     * the replacement root and finally this machine's source dump directory.
     */
    const char *sample_path = group->single_path;
    if (!sample_path && group->variants && group->variants->len > 0) {
        AudioReplacementIndexVariant *v = g_ptr_array_index(group->variants, 0);
        sample_path = v->path;
    }
    if (sample_path) {
        g_autofree char *parent = g_path_get_dirname(sample_path);
        g_autofree char *json = g_strdup_printf("%s%c%s.json", parent,
                                                G_DIR_SEPARATOR, key);
        if (read_sample_rate_file(json, &rate)) {
            return rate;
        }
    }

    if (replace_dir && replace_dir[0]) {
        g_autofree char *json = g_strdup_printf("%s%c%s.json", replace_dir,
                                                G_DIR_SEPARATOR, key);
        if (read_sample_rate_file(json, &rate)) {
            return rate;
        }
    }
    if (dump_dir && dump_dir[0]) {
        g_autofree char *json = g_strdup_printf("%s%c%s.json", dump_dir,
                                                G_DIR_SEPARATOR, key);
        if (read_sample_rate_file(json, &rate)) {
            return rate;
        }
    }
    return 0;
}


static void load_stream_metadata_for_group(const char *replace_dir,
                                           const char *dump_dir,
                                           const char *key,
                                           AudioReplacementIndexGroup *group)
{
    AudioSourceMetadata meta = { 0 };

    const char *sample_path = group->single_path;
    if (!sample_path && group->variants && group->variants->len > 0) {
        AudioReplacementIndexVariant *v = g_ptr_array_index(group->variants, 0);
        sample_path = v->path;
    }

    /* Pack-local metadata has priority, but missing fields are allowed to fall
     * through to the replacement root and local source dump metadata. */
    if (sample_path) {
        g_autofree char *parent = g_path_get_dirname(sample_path);
        g_autofree char *json = g_strdup_printf("%s%c%s.json", parent,
                                                G_DIR_SEPARATOR, key);
        read_source_metadata_file(json, &meta);
    }
    if (replace_dir && replace_dir[0]) {
        g_autofree char *json = g_strdup_printf("%s%c%s.json", replace_dir,
                                                G_DIR_SEPARATOR, key);
        read_source_metadata_file(json, &meta);
    }
    if (dump_dir && dump_dir[0]) {
        g_autofree char *json = g_strdup_printf("%s%c%s.json", dump_dir,
                                                G_DIR_SEPARATOR, key);
        read_source_metadata_file(json, &meta);
    }

    if (meta.rate_valid) {
        group->reference_rate = meta.rate;
    }
    if (meta.frames_valid) {
        group->source_frames = meta.frames;
    }
    if (meta.channels_valid) {
        group->source_channels = meta.channels;
    }
    if (meta.streaming_valid) {
        group->source_streaming = meta.streaming;
    }
    group->stream_fingerprint_mask = meta.stream_fingerprint_mask;
    memcpy(group->stream_fingerprints, meta.stream_fingerprints,
           sizeof(group->stream_fingerprints));

    /*
     * Backward compatibility and resident-buffer coverage: derive source-prefix
     * fingerprints from the original dump WAV whenever metadata does not carry
     * them yet.  This applies to both SSL streams and ordinary DirectSound
     * buffers.  The latter is important for titles which reuse a large backing
     * buffer: the audible prefix can remain identical while unrelated tail/ring
     * contents change the traditional full-buffer hash.  This is rebuild-side
     * work only; the APU thread never opens source files.
     */
    if (dump_dir && dump_dir[0] &&
        (group->stream_fingerprint_mask !=
             ((1u << AUDIO_STREAM_FINGERPRINT_TIER_COUNT) - 1u) ||
         !group->reference_rate || !group->source_frames ||
         !group->source_channels)) {
        g_autofree char *wav = g_strdup_printf("%s%c%s.wav", dump_dir,
                                               G_DIR_SEPARATOR, key);
        int16_t *pcm = NULL;
        uint32_t frames = 0;
        uint32_t total_frames = 0;
        uint32_t wav_rate = 0;
        unsigned int channels = 0;
        if (load_pcm16_source_prefix(wav, &pcm, &frames, &channels,
                                     &total_frames, &wav_rate)) {
            if (!group->reference_rate && wav_rate >= 100 &&
                wav_rate <= 384000) {
                group->reference_rate = wav_rate;
            }
            if (!group->source_frames && total_frames) {
                group->source_frames = total_frames;
            }
            if (!group->source_channels) {
                group->source_channels = channels;
            }
            for (guint i = 0; i < AUDIO_STREAM_FINGERPRINT_TIER_COUNT; i++) {
                uint32_t tier_frames = audio_stream_fingerprint_tiers[i];
                if ((group->stream_fingerprint_mask & (1u << i)) ||
                    frames < tier_frames || channels != group->source_channels) {
                    continue;
                }
                group->stream_fingerprints[i] =
                    audio_stream_fingerprint(pcm, tier_frames, channels);
                group->stream_fingerprint_mask |= 1u << i;
            }
            g_free(pcm);
        }
    }
}

static void free_stream_match_entry(gpointer p)
{
    g_free(p);
}

static GHashTable *build_stream_match_index(GHashTable *replacements,
                                            uint32_t *out_unique)
{
    GHashTable *table = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                               g_free,
                                               free_stream_match_entry);
    uint32_t unique = 0;
    uint32_t matchable_groups = 0;
    if (!replacements) {
        if (out_unique) {
            *out_unique = 0;
        }
        return table;
    }

    GHashTableIter it;
    gpointer key_ptr;
    gpointer value;
    g_hash_table_iter_init(&it, replacements);
    while (g_hash_table_iter_next(&it, &key_ptr, &value)) {
        const char *key = key_ptr;
        AudioReplacementIndexGroup *group = value;
        if (!group->source_streaming || group->stream_fingerprint_mask == 0 ||
            group->source_channels < 1 || group->source_channels > 2 ||
            group->reference_rate < 100 || group->reference_rate > 384000) {
            continue;
        }

        matchable_groups++;
        char *end = NULL;
        uint64_t source_hash = g_ascii_strtoull(key, &end, 16);
        if (!end || *end) {
            continue;
        }

        for (guint i = 0; i < AUDIO_STREAM_FINGERPRINT_TIER_COUNT; i++) {
            if (!(group->stream_fingerprint_mask & (1u << i))) {
                continue;
            }
            uint64_t fingerprint = group->stream_fingerprints[i];
            AudioStreamMatchEntry *entry =
                g_hash_table_lookup(table, &fingerprint);
            if (!entry) {
                guint64 *fp_key = g_new(guint64, 1);
                *fp_key = fingerprint;
                entry = g_new0(AudioStreamMatchEntry, 1);
                entry->source_hash = source_hash;
                g_hash_table_insert(table, fp_key, entry);
                unique++;
            } else if (entry->source_hash != source_hash && !entry->ambiguous) {
                entry->ambiguous = true;
                if (unique > 0) {
                    unique--;
                }
            }
        }
    }

    if (out_unique) {
        *out_unique = unique;
    }
    if (matchable_groups) {
        fprintf(stderr,
                "mcpx: audio-io: stream matcher indexed %u source(s), "
                "%u unique prefix fingerprint(s)\n",
                matchable_groups, unique);
    }
    return table;
}

static GHashTable *build_static_match_index(GHashTable *replacements,
                                            uint32_t *out_unique)
{
    GHashTable *table = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                               g_free,
                                               free_stream_match_entry);
    uint32_t unique = 0;
    uint32_t matchable_groups = 0;
    if (!replacements) {
        if (out_unique) {
            *out_unique = 0;
        }
        return table;
    }

    GHashTableIter it;
    gpointer key_ptr;
    gpointer value;
    g_hash_table_iter_init(&it, replacements);
    while (g_hash_table_iter_next(&it, &key_ptr, &value)) {
        const char *key = key_ptr;
        AudioReplacementIndexGroup *group = value;
        if (group->source_streaming || group->stream_fingerprint_mask == 0 ||
            group->source_channels < 1 || group->source_channels > 2) {
            continue;
        }

        matchable_groups++;
        char *end = NULL;
        uint64_t source_hash = g_ascii_strtoull(key, &end, 16);
        if (!end || *end) {
            continue;
        }

        for (guint i = 0; i < AUDIO_STREAM_FINGERPRINT_TIER_COUNT; i++) {
            if (!(group->stream_fingerprint_mask & (1u << i))) {
                continue;
            }
            uint64_t fingerprint = group->stream_fingerprints[i];
            AudioStreamMatchEntry *entry =
                g_hash_table_lookup(table, &fingerprint);
            if (!entry) {
                guint64 *fp_key = g_new(guint64, 1);
                *fp_key = fingerprint;
                entry = g_new0(AudioStreamMatchEntry, 1);
                entry->source_hash = source_hash;
                g_hash_table_insert(table, fp_key, entry);
                unique++;
            } else if (entry->source_hash != source_hash && !entry->ambiguous) {
                entry->ambiguous = true;
                if (unique > 0) {
                    unique--;
                }
            }
        }
    }

    if (out_unique) {
        *out_unique = unique;
    }
    if (matchable_groups) {
        fprintf(stderr,
                "mcpx: audio-io: resident/static matcher indexed %u source(s), "
                "%u unique prefix fingerprint(s)\n",
                matchable_groups, unique);
    }
    return table;
}


static GHashTable *build_consumed_window_index(GHashTable *replacements,
                                               const char *dump_dir,
                                               uint32_t *out_unique)
{
    GHashTable *table = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                               g_free,
                                               free_consumed_window_bucket);
    uint32_t unique = 0;
    uint32_t indexed_sources = 0;
    uint64_t indexed_windows = 0;
    if (!replacements || !dump_dir || !dump_dir[0]) {
        if (out_unique) {
            *out_unique = 0;
        }
        return table;
    }

    GHashTableIter it;
    gpointer key_ptr;
    gpointer value;
    g_hash_table_iter_init(&it, replacements);
    while (g_hash_table_iter_next(&it, &key_ptr, &value)) {
        const char *key = key_ptr;
        AudioReplacementIndexGroup *group = value;
        char *end = NULL;
        uint64_t source_hash = g_ascii_strtoull(key, &end, 16);
        if (!end || *end) {
            continue;
        }

        g_autofree char *wav = g_strdup_printf("%s%c%s.wav", dump_dir,
                                               G_DIR_SEPARATOR, key);
        int16_t *pcm = NULL;
        uint32_t frames = 0;
        unsigned int channels = 0;
        uint32_t max_frames = group->source_streaming
            ? AUDIO_CONSUMED_STREAM_INDEX_MAX_FRAMES : 0;
        if (!load_pcm16_source_wav(wav, max_frames, &pcm, &frames, &channels,
                                   NULL) ||
            frames < AUDIO_CONSUMED_WINDOW_FRAMES ||
            (group->source_channels && group->source_channels != channels)) {
            g_free(pcm);
            continue;
        }

        indexed_sources++;
        const uint64_t base = UINT64_C(0x100000001b3);
        const uint64_t remove_pow = AUDIO_CONSUMED_REMOVE_POW;
        const uint64_t channel_tag =
            (uint64_t)channels * UINT64_C(0xd6e8feb86659fd93);
        uint64_t rolling = 0;
        for (uint32_t i = 0; i < AUDIO_CONSUMED_WINDOW_FRAMES; i++) {
            rolling *= base;
            rolling += audio_consumed_frame_token(
                &pcm[(size_t)i * channels], channels);
        }

        uint32_t last_start = frames - AUDIO_CONSUMED_WINDOW_FRAMES;
        for (uint32_t offset = 0; offset <= last_start; offset++) {
            uint64_t anchor = rolling ^ channel_tag;
            /* Content-defined sparse landmarks keep runtime lookups and index
             * memory low while remaining independent of source/callback/ring
             * alignment. The first window is handled by the existing prefix
             * matcher and does not need to be forced into this fallback. */
            if ((anchor & AUDIO_CONSUMED_ANCHOR_MASK) == 0 &&
                audio_consumed_window_has_signal(
                    &pcm[(size_t)offset * channels], channels)) {
                uint64_t verify = audio_stream_fingerprint(
                    &pcm[(size_t)offset * channels],
                    AUDIO_CONSUMED_WINDOW_FRAMES, channels);
                if (verify) {
                    add_consumed_window_entry(table, anchor, verify,
                                              source_hash, offset, &unique);
                    indexed_windows++;
                }
            }

            if (offset == last_start) {
                break;
            }
            uint64_t old_token = audio_consumed_frame_token(
                &pcm[(size_t)offset * channels], channels);
            uint64_t new_token = audio_consumed_frame_token(
                &pcm[(size_t)(offset + AUDIO_CONSUMED_WINDOW_FRAMES) * channels],
                channels);
            rolling -= old_token * remove_pow;
            rolling *= base;
            rolling += new_token;
        }
        g_free(pcm);
    }

    if (out_unique) {
        *out_unique = unique;
    }
    if (indexed_sources) {
        fprintf(stderr,
                "mcpx: audio-io: consumed-window matcher indexed %u source(s), "
                "%" PRIu64 " landmark(s), %u unique verified window(s)\n",
                indexed_sources, indexed_windows, unique);
    }
    return table;
}

static GHashTable *build_replacement_index(const char *dir, const char *dump_dir)
{
    GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, free_index_group);
    scan_replacement_wavs_recursive(dir, table);

    GHashTableIter it;
    gpointer key_ptr;
    gpointer value;
    g_hash_table_iter_init(&it, table);
    while (g_hash_table_iter_next(&it, &key_ptr, &value)) {
        const char *key = key_ptr;
        AudioReplacementIndexGroup *group = value;
        group->reference_rate =
            load_reference_rate_for_group(dir, dump_dir, key, group);
        load_stream_metadata_for_group(dir, dump_dir, key, group);
    }
    return table;
}

static uint32_t lookup_reference_rate(uint64_t hash)
{
    char key[AUDIO_HASH_KEY_LEN];
    hash_key(hash, key);

    uint32_t rate = 0;
    g_mutex_lock(&g_index_lock);
    if (g_paths_valid && g_replace_index) {
        AudioReplacementIndexGroup *group =
            g_hash_table_lookup(g_replace_index, key);
        if (group) {
            rate = group->reference_rate;
        }
    }
    g_mutex_unlock(&g_index_lock);
    return rate;
}

static AudioPreloadedWav *preload_wav_file(const char *path, uint32_t target_rate)
{
    SDL_AudioSpec src_spec;
    Uint8 *src = NULL;
    Uint32 src_len = 0;
    if (!SDL_LoadWAV(path, &src_spec, &src, &src_len)) {
        fprintf(stderr, "mcpx: audio-io: SDL_LoadWAV failed for %s: %s\n",
                path, SDL_GetError());
        return NULL;
    }
    if (src_spec.freq <= 0 || src_spec.channels <= 0) {
        SDL_free(src);
        return NULL;
    }

    /* Keep native mono/stereo topology in the warm cache.  Multichannel WAVs
     * are reduced to stereo once here; Xbox source-voice adaptation happens
     * later using only memory already resident in the cache. */
    unsigned int warm_channels = src_spec.channels == 1 ? 1 : 2;
    SDL_AudioSpec dst_spec = {
        .freq = target_rate ? (int)target_rate : src_spec.freq,
        .format = SDL_AUDIO_F32,
        .channels = warm_channels,
    };
    Uint8 *dst = NULL;
    int dst_len = 0;
    bool ok = SDL_ConvertAudioSamples(&src_spec, src, (int)src_len,
                                      &dst_spec, &dst, &dst_len);
    SDL_free(src);
    if (!ok || !dst || dst_len <= 0) {
        SDL_free(dst);
        fprintf(stderr,
                "mcpx: audio-io: WAV preload conversion failed for %s: %s\n",
                path, SDL_GetError());
        return NULL;
    }

    size_t frame_bytes = warm_channels * sizeof(float);
    uint64_t frames64 = (uint64_t)dst_len / frame_bytes;
    if (frames64 == 0 || frames64 > UINT32_MAX) {
        SDL_free(dst);
        return NULL;
    }

    AudioPreloadedWav *w = g_new0(AudioPreloadedWav, 1);
    w->refcount = 1; /* cache/job ownership */
    w->samples = (float *)dst;
    w->frames = (uint32_t)frames64;
    w->sample_rate = dst_spec.freq;
    w->channels = warm_channels;
    return w;
}

typedef struct AudioPreloadJob {
    char *path;
    char *cache_key;
    uint32_t target_rate;
    AudioPreloadedWav *wav;
} AudioPreloadJob;

static void free_preload_job(gpointer p)
{
    AudioPreloadJob *job = p;
    if (!job) {
        return;
    }
    g_free(job->path);
    g_free(job->cache_key);
    free_preloaded_wav(job->wav);
    g_free(job);
}

static void preload_job_worker(gpointer data, gpointer user_data)
{
    (void)user_data;
    AudioPreloadJob *job = data;
    job->wav = preload_wav_file(job->path, job->target_rate);
}

static char *audio_preload_cache_key(const char *path, uint32_t target_rate)
{
    return g_strdup_printf("%s\x1f%u", path ? path : "", target_rate);
}

static void add_preload_job(GPtrArray *jobs, GHashTable *seen,
                            const char *path, uint32_t target_rate)
{
    if (!path || !path[0]) {
        return;
    }

    g_autofree char *key = audio_preload_cache_key(path, target_rate);
    if (g_hash_table_contains(seen, key)) {
        return;
    }
    g_hash_table_add(seen, g_strdup(key));
    AudioPreloadJob *job = g_new0(AudioPreloadJob, 1);
    job->path = g_strdup(path);
    job->cache_key = g_strdup(key);
    job->target_rate = target_rate;
    g_ptr_array_add(jobs, job);
}

static GHashTable *build_preload_cache(GHashTable *replacements)
{
    GHashTable *cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, free_preloaded_wav);
    if (!g_config.audio.replace_enabled || !replacements ||
        g_hash_table_size(replacements) == 0) {
        return cache;
    }

    GPtrArray *jobs = g_ptr_array_new_with_free_func(free_preload_job);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
    GHashTableIter it;
    gpointer value;
    g_hash_table_iter_init(&it, replacements);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        AudioReplacementIndexGroup *group = value;
        /* Native/static replacement keeps the encoded WAV rate and uses the
         * established SRC ratio. Passthrough modes (SSL and consumed-window)
         * additionally need a source-rate-normalized copy so one replacement
         * frame maps to one guest-consumed frame without perturbing CBO/SSL or
         * software-ring timing. All conversion stays off the APU workers. */
        uint32_t passthrough_rate = group->reference_rate;

        /* Match runtime selection semantics exactly: numbered variants take
         * precedence over an unsuffixed singleton when both are present. */
        if (group->variants && group->variants->len > 0) {
            for (guint i = 0; i < group->variants->len; i++) {
                AudioReplacementIndexVariant *v =
                    g_ptr_array_index(group->variants, i);
                if (!group->source_streaming) {
                    add_preload_job(jobs, seen, v->path, 0);
                }
                if (passthrough_rate) {
                    add_preload_job(jobs, seen, v->path, passthrough_rate);
                }
            }
        } else {
            if (!group->source_streaming) {
                add_preload_job(jobs, seen, group->single_path, 0);
            }
            if (passthrough_rate) {
                add_preload_job(jobs, seen, group->single_path,
                                passthrough_rate);
            }
        }
    }
    g_hash_table_destroy(seen);

    int64_t start_us = g_get_monotonic_time();
    guint workers = MIN(4u, MAX(1u, (guint)g_get_num_processors()));
    GError *error = NULL;
    GThreadPool *pool = g_thread_pool_new(preload_job_worker, NULL,
                                          (gint)workers, FALSE, &error);
    if (pool) {
        for (guint i = 0; i < jobs->len; i++) {
            g_thread_pool_push(pool, g_ptr_array_index(jobs, i), NULL);
        }
        /* Wait here on the UI/index-rebuild side.  The new replacement index
         * is not published until every WAV is resident, so the APU never sees
         * a half-warm pack and never performs disk I/O itself. */
        g_thread_pool_free(pool, FALSE, TRUE);
    } else {
        fprintf(stderr,
                "mcpx: audio-io: preload thread pool unavailable%s%s; "
                "loading serially\n",
                error ? ": " : "", error ? error->message : "");
        g_clear_error(&error);
        for (guint i = 0; i < jobs->len; i++) {
            preload_job_worker(g_ptr_array_index(jobs, i), NULL);
        }
    }

    guint loaded = 0;
    uint64_t frames = 0;
    for (guint i = 0; i < jobs->len; i++) {
        AudioPreloadJob *job = g_ptr_array_index(jobs, i);
        if (!job->wav) {
            continue;
        }
        frames += job->wav->frames;
        g_hash_table_insert(cache, g_strdup(job->cache_key), job->wav);
        job->wav = NULL; /* ownership moved to cache */
        loaded++;
    }
    int64_t elapsed_us = g_get_monotonic_time() - start_us;
    fprintf(stderr,
            "mcpx: audio-io: preloaded %u/%u replacement WAV(s), %" PRIu64
            " frame(s), in %.3f s\n",
            loaded, jobs->len, frames, elapsed_us / 1000000.0);
    g_ptr_array_free(jobs, TRUE);
    return cache;
}

/*
 * Materialize the source voice's 1/2-channel view from already-decoded memory.
 * This function is safe on mcpx.voice_worker: no filesystem access, no WAV
 * parsing and no SDL audio conversion occur here.
 */
static AudioReplacement *materialize_preloaded_replacement(
    const char *path, unsigned int channels, uint32_t target_rate)
{
    if (!path || channels < 1 || channels > 2) {
        return NULL;
    }

    g_autofree char *cache_key = audio_preload_cache_key(path, target_rate);
    g_mutex_lock(&g_index_lock);
    AudioPreloadedWav *w = g_preload_cache ?
        g_hash_table_lookup(g_preload_cache, cache_key) : NULL;
    if (!w || !w->samples || w->frames == 0 ||
        (w->channels != 1 && w->channels != 2)) {
        g_mutex_unlock(&g_index_lock);
        return NULL;
    }
    /* Hold the immutable WAV after dropping the index lock so a reload can
     * swap/free its cache concurrently without extending this critical section. */
    preloaded_wav_ref(w);
    g_mutex_unlock(&g_index_lock);

    AudioReplacement *r = g_new0(AudioReplacement, 1);
    r->frames = w->frames;
    r->sample_rate = w->sample_rate;
    r->channels = channels;

    if (w->channels == channels) {
        /* Common path: zero copy. AudioReplacement owns this reference. */
        r->shared_wav = w;
        r->samples = w->samples;
        return r;
    }

    uint64_t sample_count = (uint64_t)w->frames * channels;
    if (sample_count > SIZE_MAX / sizeof(float)) {
        preloaded_wav_unref(w);
        g_free(r);
        return NULL;
    }
    float *samples = SDL_malloc((size_t)sample_count * sizeof(float));
    if (!samples) {
        preloaded_wav_unref(w);
        g_free(r);
        return NULL;
    }

    if (w->channels == 1 && channels == 2) {
        for (uint32_t i = 0; i < w->frames; i++) {
            float sample = w->samples[i];
            samples[(size_t)i * 2] = sample;
            samples[(size_t)i * 2 + 1] = sample;
        }
    } else { /* warm stereo -> Xbox mono */
        for (uint32_t i = 0; i < w->frames; i++) {
            samples[i] = 0.5f * (w->samples[(size_t)i * 2] +
                                 w->samples[(size_t)i * 2 + 1]);
        }
    }
    preloaded_wav_unref(w);
    r->samples = samples;
    return r;
}

static GHashTable *build_dump_set(const char *dir)
{
    GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
    if (!dir || !dir[0]) {
        return table;
    }
    GDir *gd = g_dir_open(dir, 0, NULL);
    if (!gd) {
        return table;
    }
    const char *name;
    while ((name = g_dir_read_name(gd)) != NULL) {
        char key[AUDIO_HASH_KEY_LEN];
        if (parse_hash_wav_name(name, key)) {
            g_hash_table_add(table, g_strdup(key));
        }
    }
    g_dir_close(gd);
    return table;
}

static void swap_replacement_index(GHashTable *replacements,
                                   GHashTable *preloads,
                                   GHashTable *stream_matches,
                                   uint32_t stream_match_count,
                                   GHashTable *static_matches,
                                   uint32_t static_match_count,
                                   GHashTable *consumed_windows,
                                   uint32_t consumed_window_count)
{
    GHashTable *old_replacements;
    GHashTable *old_preloads;
    GHashTable *old_stream_matches;
    GHashTable *old_static_matches;
    GHashTable *old_consumed_windows;

    audio_consumed_publish_bloom(consumed_windows);
    g_mutex_lock(&g_index_lock);
    old_replacements = g_replace_index;
    old_preloads = g_preload_cache;
    old_stream_matches = g_stream_match_index;
    old_static_matches = g_static_match_index;
    old_consumed_windows = g_consumed_window_index;
    g_replace_index = replacements;
    g_preload_cache = preloads;
    g_stream_match_index = stream_matches;
    g_static_match_index = static_matches;
    g_consumed_window_index = consumed_windows;
    qatomic_set(&g_replacement_count, replacements ?
                (uint32_t)g_hash_table_size(replacements) : 0);
    qatomic_set(&g_stream_match_count, stream_match_count);
    qatomic_set(&g_static_match_count, static_match_count);
    qatomic_set(&g_consumed_window_unique_count, consumed_window_count);
    /* Replacement identity/preloaded data changed: invalidate decoded asset
     * views at the next safe VP frame boundary. */
    qatomic_inc(&g_index_generation);
    g_mutex_unlock(&g_index_lock);

    if (old_replacements) {
        g_hash_table_destroy(old_replacements);
    }
    if (old_preloads) {
        g_hash_table_destroy(old_preloads);
    }
    if (old_stream_matches) {
        g_hash_table_destroy(old_stream_matches);
    }
    if (old_static_matches) {
        g_hash_table_destroy(old_static_matches);
    }
    if (old_consumed_windows) {
        g_hash_table_destroy(old_consumed_windows);
    }
}

static void swap_dump_index(GHashTable *dumps)
{
    GHashTable *old_dumps;

    g_mutex_lock(&g_index_lock);
    old_dumps = g_dumped_set;
    g_dumped_set = dumps;
    g_mutex_unlock(&g_index_lock);

    if (old_dumps) {
        g_hash_table_destroy(old_dumps);
    }
}

void xemu_audio_packs_refresh_paths(void)
{
    if (!g_config.audio.dump_enabled && !g_config.audio.replace_enabled) {
        return;
    }

    static int64_t last_identify_us;
    static bool identified_once;
    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (identified_once && now_us >= last_identify_us &&
        now_us - last_identify_us < 500000) {
        return;
    }
    identified_once = true;
    last_identify_us = now_us;

    uint32_t title_id = 0;
    if (!xemu_get_xbe_title_id(&title_id)) {
        return;
    }

    static char *cached_dump_root;
    static char *cached_replace_root;
    const char *dump_root = g_config.audio.dump_dir ? g_config.audio.dump_dir : "";
    const char *replace_root = g_config.audio.replace_dir ?
                               g_config.audio.replace_dir : "";

    bool roots_changed = !cached_dump_root || !cached_replace_root ||
        strcmp(cached_dump_root, dump_root) != 0 ||
        strcmp(cached_replace_root, replace_root) != 0;

    g_mutex_lock(&g_index_lock);
    bool title_changed = !g_paths_valid || g_title_id != title_id;
    g_mutex_unlock(&g_index_lock);
    if (!title_changed && !roots_changed) {
        return;
    }

    g_free(cached_dump_root);
    g_free(cached_replace_root);
    cached_dump_root = g_strdup(dump_root);
    cached_replace_root = g_strdup(replace_root);

    char title_hex[9];
    snprintf(title_hex, sizeof(title_hex), "%08X", title_id);
    const char *base = xemu_settings_get_base_path();

    g_autofree char *new_dump = dump_root[0]
        ? g_build_filename(dump_root, title_hex, "dumps", NULL)
        : g_build_filename(base, "audio", title_hex, "dumps", NULL);
    g_autofree char *new_replace = replace_root[0]
        ? g_build_filename(replace_root, title_hex, "replacements", NULL)
        : g_build_filename(base, "audio", title_hex, "replacements", NULL);

    if (g_mkdir_with_parents(new_dump, 0755) != 0 ||
        g_mkdir_with_parents(new_replace, 0755) != 0) {
        fprintf(stderr, "mcpx: audio-io: could not create directories for %s\n",
                title_hex);
        return;
    }

    GHashTable *new_replacements = build_replacement_index(new_replace, new_dump);
    GHashTable *new_preloads = build_preload_cache(new_replacements);
    uint32_t new_stream_match_count = 0;
    GHashTable *new_stream_matches =
        build_stream_match_index(new_replacements, &new_stream_match_count);
    uint32_t new_static_match_count = 0;
    GHashTable *new_static_matches =
        build_static_match_index(new_replacements, &new_static_match_count);
    uint32_t new_consumed_window_count = 0;
    GHashTable *new_consumed_windows =
        build_consumed_window_index(new_replacements, new_dump,
                                    &new_consumed_window_count);
    GHashTable *new_dumps = build_dump_set(new_dump);

    char *old_dump;
    char *old_replace;
    GHashTable *old_replacements;
    GHashTable *old_preloads;
    GHashTable *old_stream_matches;
    GHashTable *old_static_matches;
    GHashTable *old_consumed_windows;
    GHashTable *old_dumps;
    audio_consumed_publish_bloom(new_consumed_windows);
    g_mutex_lock(&g_index_lock);
    old_dump = g_dump_dir;
    old_replace = g_replace_dir;
    old_replacements = g_replace_index;
    old_preloads = g_preload_cache;
    old_stream_matches = g_stream_match_index;
    old_static_matches = g_static_match_index;
    old_consumed_windows = g_consumed_window_index;
    old_dumps = g_dumped_set;
    g_dump_dir = g_strdup(new_dump);
    g_replace_dir = g_strdup(new_replace);
    g_replace_index = new_replacements;
    g_preload_cache = new_preloads;
    g_stream_match_index = new_stream_matches;
    g_static_match_index = new_static_matches;
    g_consumed_window_index = new_consumed_windows;
    g_dumped_set = new_dumps;
    qatomic_set(&g_replacement_count, new_replacements ?
                (uint32_t)g_hash_table_size(new_replacements) : 0);
    qatomic_set(&g_stream_match_count, new_stream_match_count);
    qatomic_set(&g_static_match_count, new_static_match_count);
    qatomic_set(&g_consumed_window_unique_count, new_consumed_window_count);
    g_title_id = title_id;
    g_paths_valid = true;
    qatomic_inc(&g_index_generation);
    g_mutex_unlock(&g_index_lock);

    g_free(old_dump);
    g_free(old_replace);
    if (old_replacements) {
        g_hash_table_destroy(old_replacements);
    }
    if (old_preloads) {
        g_hash_table_destroy(old_preloads);
    }
    if (old_stream_matches) {
        g_hash_table_destroy(old_stream_matches);
    }
    if (old_static_matches) {
        g_hash_table_destroy(old_static_matches);
    }
    if (old_consumed_windows) {
        g_hash_table_destroy(old_consumed_windows);
    }
    if (old_dumps) {
        g_hash_table_destroy(old_dumps);
    }

    fprintf(stderr, "mcpx: audio-io: title %s\n  dumps: %s\n  repl:  %s\n",
            title_hex, new_dump, new_replace);
}

void xemu_audio_packs_rebuild_replacement_index(void)
{
    /* refresh_paths() performs the full index+preload build when the title or
     * root changes.  Avoid decoding the entire pack twice on that transition. */
    uint32_t generation_before = current_generation();
    xemu_audio_packs_refresh_paths();
    if (current_generation() != generation_before) {
        return;
    }

    char *replace_dir = NULL;
    char *dump_dir = NULL;
    g_mutex_lock(&g_index_lock);
    if (g_paths_valid) {
        replace_dir = g_strdup(g_replace_dir);
        dump_dir = g_strdup(g_dump_dir);
    }
    g_mutex_unlock(&g_index_lock);
    if (!replace_dir) {
        g_free(dump_dir);
        return;
    }

    GHashTable *replacements = build_replacement_index(replace_dir, dump_dir);
    GHashTable *preloads = build_preload_cache(replacements);
    uint32_t stream_match_count = 0;
    GHashTable *stream_matches =
        build_stream_match_index(replacements, &stream_match_count);
    uint32_t static_match_count = 0;
    GHashTable *static_matches =
        build_static_match_index(replacements, &static_match_count);
    uint32_t consumed_window_count = 0;
    GHashTable *consumed_windows =
        build_consumed_window_index(replacements, dump_dir,
                                    &consumed_window_count);
    g_free(replace_dir);
    g_free(dump_dir);
    swap_replacement_index(replacements, preloads, stream_matches,
                           stream_match_count, static_matches,
                           static_match_count, consumed_windows,
                           consumed_window_count);
}

void xemu_audio_packs_rebuild_dump_index(void)
{
    xemu_audio_packs_refresh_paths();

    char *dump_dir = NULL;
    g_mutex_lock(&g_index_lock);
    if (g_paths_valid) {
        dump_dir = g_strdup(g_dump_dir);
    }
    g_mutex_unlock(&g_index_lock);
    if (!dump_dir) {
        return;
    }

    GHashTable *dumps = build_dump_set(dump_dir);
    g_free(dump_dir);
    /* Dump bookkeeping does not affect replacement playback state. */
    swap_dump_index(dumps);
}

bool xemu_audio_packs_replacements_available(void)
{
    return qatomic_read(&g_replacement_count) != 0;
}

static bool write_u16(FILE *f, uint16_t v)
{
    uint8_t b[2] = { v & 0xff, v >> 8 };
    return fwrite(b, 1, sizeof(b), f) == sizeof(b);
}

static bool write_u32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24 };
    return fwrite(b, 1, sizeof(b), f) == sizeof(b);
}

static bool audio_file_seek_set(FILE *f, uint64_t offset)
{
    if (!f || offset > INT64_MAX) {
        return false;
    }
#ifdef _WIN32
    return _fseeki64(f, (int64_t)offset, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)offset, SEEK_SET) == 0;
#endif
}

static bool write_pcm16_wav_header(FILE *f, unsigned int channels,
                                   uint32_t sample_rate, uint32_t data_bytes)
{
    return fwrite("RIFF", 1, 4, f) == 4 &&
           write_u32(f, 36 + data_bytes) &&
           fwrite("WAVEfmt ", 1, 8, f) == 8 &&
           write_u32(f, 16) && write_u16(f, 1) &&
           write_u16(f, channels) &&
           write_u32(f, sample_rate) &&
           write_u32(f, sample_rate * channels * 2) &&
           write_u16(f, channels * 2) && write_u16(f, 16) &&
           fwrite("data", 1, 4, f) == 4 && write_u32(f, data_bytes);
}

static bool write_pcm16_wav_header_ex(FILE *f, unsigned int channels,
                                      uint32_t sample_rate, uint32_t data_bytes,
                                      bool with_smpl)
{
    uint64_t riff_size = 36u + (uint64_t)data_bytes +
                         (with_smpl ? 68u : 0u);
    if (riff_size > UINT32_MAX) {
        return false;
    }
    return fwrite("RIFF", 1, 4, f) == 4 &&
           write_u32(f, (uint32_t)riff_size) &&
           fwrite("WAVEfmt ", 1, 8, f) == 8 &&
           write_u32(f, 16) && write_u16(f, 1) &&
           write_u16(f, channels) &&
           write_u32(f, sample_rate) &&
           write_u32(f, sample_rate * channels * 2) &&
           write_u16(f, channels * 2) && write_u16(f, 16) &&
           fwrite("data", 1, 4, f) == 4 && write_u32(f, data_bytes);
}

static bool write_wav_smpl_chunk(FILE *f, uint32_t sample_rate,
                                 uint32_t loop_start, uint32_t loop_end)
{
    if (!f || sample_rate == 0 || loop_end <= loop_start) {
        return false;
    }
    uint32_t sample_period = (uint32_t)llrint(1000000000.0 / sample_rate);
    /* RIFF smpl uses an inclusive end point. Play count 0 means infinite. */
    return fwrite("smpl", 1, 4, f) == 4 &&
           write_u32(f, 60) &&
           write_u32(f, 0) && write_u32(f, 0) &&
           write_u32(f, sample_period) &&
           write_u32(f, 60) && write_u32(f, 0) &&
           write_u32(f, 0) && write_u32(f, 0) &&
           write_u32(f, 1) && write_u32(f, 0) &&
           write_u32(f, 0) && write_u32(f, 0) &&
           write_u32(f, loop_start) && write_u32(f, loop_end - 1) &&
           write_u32(f, 0) && write_u32(f, 0);
}

static bool write_pcm16_wav(const AudioDumpJob *job)
{
    if (job->channels < 1 || job->channels > 2 || job->frames == 0) {
        return false;
    }
    uint64_t data64 = (uint64_t)job->frames * job->channels * sizeof(int16_t);
    uint64_t riff64 = 36u + data64 + (job->loop ? 68u : 0u);
    if (data64 > UINT32_MAX || riff64 > UINT32_MAX) {
        return false;
    }
    uint32_t data_bytes = (uint32_t)data64;
    FILE *f = g_fopen(job->wav_path, "wb");
    if (!f) {
        return false;
    }

    bool with_smpl = job->loop && job->loop_end > job->loop_start &&
                     job->loop_end <= job->frames;
    bool ok = write_pcm16_wav_header_ex(f, job->channels, job->sample_rate,
                                        data_bytes, with_smpl) &&
              fwrite(job->pcm, 1, data_bytes, f) == data_bytes;
    if (ok && with_smpl) {
        ok = write_wav_smpl_chunk(f, job->sample_rate, job->loop_start,
                                  job->loop_end);
    }
    fclose(f);
    if (!ok) {
        g_remove(job->wav_path);
    }
    return ok;
}

static void write_metadata_json(const AudioDumpJob *job)
{
    /* Preserve a user's local policy if the WAV/metadata gets re-dumped. */
    bool restart = false;
    read_retrigger_mode_file(job->json_path, &restart);

    GString *json = g_string_new("{\n");
    g_string_append_printf(json,
        "  \"hash\": \"%016" PRIx64 "\",\n"
        "  \"source_format\": \"%s\",\n"
        "  \"channels\": %u,\n"
        "  \"sample_rate\": %u,\n"
        "  \"frames\": %u,\n"
        "  \"duration_seconds\": %.9f,\n"
        "  \"looping\": %s,\n"
        "  \"loop_start_frame\": %u,\n"
        "  \"loop_end_frame\": %u,\n"
        "  \"intro_frames\": %u,\n"
        "  \"loop_frames\": %u,\n"
        "  \"outro_frames\": %u,\n"
        "  \"logical_extraction\": true,\n"
        "  \"observed_frames_before_loop_compaction\": %" PRIu64 ",\n"
        "  \"loop_repetitions_observed\": %u,\n"
        "  \"loop_detection\": \"%s\",\n"
        "  \"wav_loop_metadata\": %s,\n"
        "  \"streaming\": %s,\n"
        "  \"stream_segments\": %u,\n",
        job->hash, job->source_format, job->channels, job->sample_rate,
        job->frames, (double)job->frames / MAX(1u, job->sample_rate),
        job->loop ? "true" : "false", job->loop_start, job->loop_end,
        job->loop ? job->loop_start : 0,
        job->loop && job->loop_end > job->loop_start
            ? job->loop_end - job->loop_start : 0,
        job->loop && job->frames > job->loop_end
            ? job->frames - job->loop_end : 0,
        job->observed_frames ? job->observed_frames : job->frames,
        job->loop_repetitions_observed,
        job->loop_detection[0] ? job->loop_detection :
            (job->loop ? "hardware" : "none"),
        job->loop ? "true" : "false",
        job->streaming ? "true" : "false", job->stream_segments);

    if (job->stream_fingerprint_mask) {
        g_string_append_printf(json, "  \"source_fingerprint_version\": %u,\n",
                               AUDIO_STREAM_FINGERPRINT_VERSION);
        for (guint i = 0; i < AUDIO_STREAM_FINGERPRINT_TIER_COUNT; i++) {
            if (!(job->stream_fingerprint_mask & (1u << i))) {
                continue;
            }
            g_string_append_printf(
                json, "  \"source_fingerprint_%u\": \"%016" PRIx64 "\",\n",
                audio_stream_fingerprint_tiers[i],
                job->stream_fingerprints[i]);
        }
        /* Preserve V1 stream metadata for downgrade/backward compatibility. */
        if (job->streaming) {
            g_string_append_printf(json,
                                   "  \"stream_fingerprint_version\": %u,\n",
                                   AUDIO_STREAM_FINGERPRINT_VERSION);
            for (guint i = 0; i < AUDIO_STREAM_FINGERPRINT_TIER_COUNT; i++) {
                if (!(job->stream_fingerprint_mask & (1u << i))) {
                    continue;
                }
                g_string_append_printf(
                    json, "  \"stream_fingerprint_%u\": \"%016" PRIx64 "\",\n",
                    audio_stream_fingerprint_tiers[i],
                    job->stream_fingerprints[i]);
            }
        }
    }

    g_string_append_printf(json,
                           "  \"retrigger_mode\": \"%s\"\n"
                           "}\n",
                           restart ? "restart" : "auto");
    g_file_set_contents(job->json_path, json->str, json->len, NULL);
    g_string_free(json, TRUE);
}

static void audio_dump_job_free(AudioDumpJob *job)
{
    if (!job) {
        return;
    }
    g_free(job->wav_path);
    g_free(job->json_path);
    g_free(job->dump_dir);
    g_free(job->pcm);
    g_free(job);
}

static void stream_writer_free(gpointer p)
{
    AudioStreamWriter *w = p;
    if (!w) {
        return;
    }
    if (w->file) {
        fclose(w->file);
    }
    if (w->tmp_path) {
        g_remove(w->tmp_path);
    }
    if (w->segment_records) {
        g_array_unref(w->segment_records);
    }
    g_free(w->tmp_path);
    g_free(w->dump_dir);
    g_free(w);
}

static AudioStreamWriter *stream_writer_begin(const AudioDumpJob *job)
{
    if (!job->dump_dir || !job->dump_dir[0] ||
        job->channels < 1 || job->channels > 2 || job->sample_rate == 0) {
        return NULL;
    }

    AudioStreamWriter *w = g_new0(AudioStreamWriter, 1);
    w->dump_dir = g_strdup(job->dump_dir);
    w->sample_rate = job->sample_rate;
    w->channels = job->channels;
    g_strlcpy(w->source_format,
              job->source_format[0] ? job->source_format : "stream_unknown",
              sizeof(w->source_format));
    w->tmp_path = g_strdup_printf("%s%c.xemu-stream-%016" PRIx64 ".tmp.wav",
                                  w->dump_dir, G_DIR_SEPARATOR,
                                  job->stream_session);
    w->file = g_fopen(w->tmp_path, "wb+");
    w->segment_records = g_array_new(FALSE, FALSE,
                                     sizeof(AudioStreamSegmentRecord));
    if (!w->file || !w->segment_records ||
        !write_pcm16_wav_header(w->file, w->channels, w->sample_rate, 0)) {
        stream_writer_free(w);
        return NULL;
    }
    return w;
}

static bool stream_writer_ranges_equal(const AudioStreamWriter *w,
                                       uint64_t a_frame, uint64_t b_frame,
                                       uint64_t frames);
static bool stream_writer_job_matches_record(const AudioStreamWriter *w,
                                             const AudioDumpJob *job,
                                             const AudioStreamSegmentRecord *r);
static void stream_writer_try_enable_loop_suppression(AudioStreamWriter *w);

static bool stream_writer_append(AudioStreamWriter *w, const AudioDumpJob *job)
{
    if (!w || !w->file || !job->pcm || job->frames == 0 ||
        job->channels != w->channels) {
        return false;
    }

    uint64_t add_bytes = (uint64_t)job->frames * w->channels * sizeof(int16_t);
    if (add_bytes > SIZE_MAX) {
        return false;
    }
    AudioStreamSegmentRecord record = {
        .start_frame = w->frames,
        .frames = job->frames,
        .pcm_hash = XXH3_64bits(job->pcm, (size_t)add_bytes),
        .segment_signature = job->stream_segment_signature,
        .has_signal = false,
    };
    size_t sample_count = (size_t)job->frames * w->channels;
    for (size_t i = 0; i < sample_count; i++) {
        if (job->pcm[i] > 2 || job->pcm[i] < -2) {
            record.has_signal = true;
            break;
        }
    }

    if (UINT64_MAX - w->observed_frames < job->frames) {
        return false;
    }
    w->observed_frames += job->frames;
    w->saw_wrap_hint |= job->stream_wrap_hint;
    if (w->segments < UINT32_MAX) {
        w->segments++;
    }

    if (w->loop_suppression_active && w->loop_cycle_record_count != 0) {
        uint32_t expected_index = w->loop_cycle_first_record +
            w->loop_cycle_phase;
        if (expected_index < w->segment_records->len) {
            AudioStreamSegmentRecord *expected = &g_array_index(
                w->segment_records, AudioStreamSegmentRecord, expected_index);
            if (record.frames == expected->frames &&
                record.pcm_hash == expected->pcm_hash &&
                record.segment_signature == expected->segment_signature &&
                stream_writer_job_matches_record(w, job, expected)) {
                w->loop_cycle_phase++;
                if (w->loop_cycle_phase == w->loop_cycle_record_count) {
                    w->loop_cycle_phase = 0;
                    if (w->loop_repetitions_suppressed < UINT32_MAX) {
                        w->loop_repetitions_suppressed++;
                    }
                }
                return true;
            }
        }

        /* The loop exited after zero or more already-proven segments of the
         * next traversal.  Those matching segments are repetition, not authored
         * outro.  Drop them and resume retention at the first genuinely new
         * segment.  If capture instead ends mid-traversal, publish() likewise
         * leaves the suppressed prefix out so the canonical asset still ends
         * after exactly one complete loop body. */
        w->loop_suppression_active = false;
        w->loop_cycle_phase = 0;
    }

    uint64_t total_bytes = (w->frames + job->frames) *
                           w->channels * sizeof(int16_t);
    if (total_bytes > UINT32_MAX - 36) {
        fprintf(stderr,
                "mcpx: audio-io: streaming dump exceeded classic WAV limit\n");
        return false;
    }
    if (fseek(w->file, 0, SEEK_END) != 0 ||
        fwrite(job->pcm, 1, (size_t)add_bytes, w->file) != (size_t)add_bytes) {
        return false;
    }

    record.start_frame = w->frames;
    g_array_append_val(w->segment_records, record);
    w->frames += job->frames;
    stream_writer_try_enable_loop_suppression(w);
    return true;
}

typedef struct AudioLoopDetection {
    bool found;
    uint64_t loop_start;
    uint64_t loop_end;
    uint64_t repeated_end;
    uint32_t repetitions;
} AudioLoopDetection;

static bool stream_writer_ranges_equal(const AudioStreamWriter *w,
                                       uint64_t a_frame, uint64_t b_frame,
                                       uint64_t frames)
{
    if (!w || !w->tmp_path || frames == 0 || a_frame > w->frames ||
        b_frame > w->frames || frames > w->frames - a_frame ||
        frames > w->frames - b_frame) {
        return false;
    }
    FILE *fa = g_fopen(w->tmp_path, "rb");
    FILE *fb = g_fopen(w->tmp_path, "rb");
    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return false;
    }

    uint64_t bytes = frames * w->channels * sizeof(int16_t);
    uint64_t a_off = 44u + a_frame * w->channels * sizeof(int16_t);
    uint64_t b_off = 44u + b_frame * w->channels * sizeof(int16_t);
    bool equal = audio_file_seek_set(fa, a_off) &&
                 audio_file_seek_set(fb, b_off);
    uint8_t ba[32768], bb[32768];
    while (equal && bytes != 0) {
        size_t take = (size_t)MIN(bytes, (uint64_t)sizeof(ba));
        if (fread(ba, 1, take, fa) != take ||
            fread(bb, 1, take, fb) != take ||
            memcmp(ba, bb, take) != 0) {
            equal = false;
            break;
        }
        bytes -= take;
    }
    fclose(fa);
    fclose(fb);
    return equal;
}

static bool stream_writer_job_matches_record(const AudioStreamWriter *w,
                                             const AudioDumpJob *job,
                                             const AudioStreamSegmentRecord *r)
{
    if (!w || !w->file || !w->tmp_path || !job || !job->pcm || !r ||
        job->frames != r->frames) {
        return false;
    }
    uint64_t bytes = (uint64_t)r->frames * w->channels * sizeof(int16_t);
    uint64_t off = 44u + r->start_frame * w->channels * sizeof(int16_t);
    if (bytes > SIZE_MAX || fflush(w->file) != 0) {
        return false;
    }
    FILE *f = g_fopen(w->tmp_path, "rb");
    if (!f || !audio_file_seek_set(f, off)) {
        if (f) fclose(f);
        return false;
    }
    const uint8_t *candidate = (const uint8_t *)job->pcm;
    uint8_t buf[32768];
    uint64_t done = 0;
    bool equal = true;
    while (done < bytes) {
        size_t take = (size_t)MIN(bytes - done, (uint64_t)sizeof(buf));
        if (fread(buf, 1, take, f) != take ||
            memcmp(buf, candidate + done, take) != 0) {
            equal = false;
            break;
        }
        done += take;
    }
    fclose(f);
    return equal;
}

static bool stream_writer_record_sequences_equal(const AudioStreamWriter *w,
                                                 uint32_t a, uint32_t b,
                                                 uint32_t count)
{
    if (!w || !w->segment_records || count == 0 ||
        a > w->segment_records->len || b > w->segment_records->len ||
        count > w->segment_records->len - a ||
        count > w->segment_records->len - b) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const AudioStreamSegmentRecord *ra = &g_array_index(
            w->segment_records, AudioStreamSegmentRecord, a + i);
        const AudioStreamSegmentRecord *rb = &g_array_index(
            w->segment_records, AudioStreamSegmentRecord, b + i);
        if (ra->frames != rb->frames || ra->pcm_hash != rb->pcm_hash ||
            ra->segment_signature != rb->segment_signature) {
            return false;
        }
    }
    return true;
}

static void stream_writer_try_enable_loop_suppression(AudioStreamWriter *w)
{
    if (!w || !w->file || w->loop_suppression_active ||
        !w->segment_records || w->segment_records->len < 3) {
        return;
    }
    uint32_t n = w->segment_records->len;
    uint32_t max_period_records = n / 3;
    for (uint32_t p = 1; p <= max_period_records; p++) {
        uint32_t first = n - 3 * p;
        uint32_t second = first + p;
        uint32_t third = second + p;
        if (!stream_writer_record_sequences_equal(w, first, second, p) ||
            !stream_writer_record_sequences_equal(w, first, third, p)) {
            continue;
        }
        bool signal = false;
        for (uint32_t i = 0; i < p; i++) {
            if (g_array_index(w->segment_records, AudioStreamSegmentRecord,
                              first + i).has_signal) {
                signal = true;
                break;
            }
        }
        if (!signal) {
            continue;
        }
        uint64_t cycle_start = g_array_index(
            w->segment_records, AudioStreamSegmentRecord, first).start_frame;
        uint64_t cycle_second = g_array_index(
            w->segment_records, AudioStreamSegmentRecord, second).start_frame;
        uint64_t cycle_third = g_array_index(
            w->segment_records, AudioStreamSegmentRecord, third).start_frame;
        uint64_t period = cycle_second - cycle_start;
        if (period < AUDIO_LOGICAL_MIN_LOOP_FRAMES ||
            cycle_third - cycle_second != period ||
            period > w->frames - cycle_third) {
            continue;
        }
        if (!stream_writer_ranges_equal(w, cycle_start, cycle_second, period) ||
            !stream_writer_ranges_equal(w, cycle_start, cycle_third, period)) {
            continue;
        }

        w->loop_suppression_active = true;
        w->loop_cycle_first_record = first;
        w->loop_cycle_record_count = p;
        w->loop_cycle_phase = 0;
        fprintf(stderr,
                "mcpx: audio-io: exact stream loop proven; suppressing later "
                "repetitions (%" PRIu64 " frames, %u retained segments/cycle)\n",
                period, p);
        return;
    }
}

static bool stream_writer_read_frame(FILE *f, const AudioStreamWriter *w,
                                     uint64_t frame, int16_t out[2])
{
    if (!f || !w || frame >= w->frames) {
        return false;
    }
    uint64_t off = 44u + frame * w->channels * sizeof(int16_t);
    if (!audio_file_seek_set(f, off)) {
        return false;
    }
    out[0] = 0;
    out[1] = 0;
    if (fread(&out[0], sizeof(int16_t), 1, f) != 1) {
        return false;
    }
    if (w->channels == 2 &&
        fread(&out[1], sizeof(int16_t), 1, f) != 1) {
        return false;
    }
    return true;
}

static uint64_t stream_writer_extend_loop_backward(const AudioStreamWriter *w,
                                                   uint64_t start,
                                                   uint64_t period)
{
    if (!w || !w->tmp_path || start == 0 || period == 0) {
        return start;
    }
    FILE *f = g_fopen(w->tmp_path, "rb");
    if (!f) {
        return start;
    }
    while (start > 0) {
        int16_t a[2], b[2];
        if (!stream_writer_read_frame(f, w, start - 1, a) ||
            !stream_writer_read_frame(f, w, start + period - 1, b) ||
            a[0] != b[0] || (w->channels == 2 && a[1] != b[1])) {
            break;
        }
        start--;
    }
    fclose(f);
    return start;
}

static bool stream_writer_detect_exact_loop(AudioStreamWriter *w,
                                            AudioLoopDetection *out)
{
    memset(out, 0, sizeof(*out));
    if (!w || !w->file || !w->segment_records ||
        w->segment_records->len < 2 || w->frames < 2 * AUDIO_LOGICAL_MIN_LOOP_FRAMES) {
        return false;
    }
    if (fflush(w->file) != 0) {
        return false;
    }

    /* Group equal decoded segments to avoid an O(n^2) scan on long-running
     * ambiences. The segment hash only generates candidates; every accepted
     * cycle is then byte-compared from the temporary PCM file. */
    GHashTable *occurrences = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                                     g_free,
                                                     (GDestroyNotify)g_array_unref);
    for (guint i = 0; i < w->segment_records->len; i++) {
        AudioStreamSegmentRecord *r = &g_array_index(
            w->segment_records, AudioStreamSegmentRecord, i);
        if (!r->has_signal) {
            continue;
        }
        uint64_t key_value = r->pcm_hash ^ r->segment_signature ^
            ((uint64_t)r->frames * UINT64_C(0x9e3779b185ebca87));
        GArray *indices = g_hash_table_lookup(occurrences, &key_value);
        if (!indices) {
            uint64_t *key = g_new(uint64_t, 1);
            *key = key_value;
            indices = g_array_new(FALSE, FALSE, sizeof(uint32_t));
            g_hash_table_insert(occurrences, key, indices);
        }
        uint32_t index = i;
        g_array_append_val(indices, index);
    }

    uint64_t best_removed = 0;
    uint64_t best_period = 0;
    GHashTableIter it;
    gpointer value;
    g_hash_table_iter_init(&it, occurrences);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        GArray *indices = value;
        for (guint a = 0; a + 1 < indices->len; a++) {
            uint32_t i = g_array_index(indices, uint32_t, a);
            AudioStreamSegmentRecord *ri = &g_array_index(
                w->segment_records, AudioStreamSegmentRecord, i);
            /* Trying the nearest few equal-segment occurrences is enough to
             * cover descriptor reuse inside a longer cycle without exploding
             * candidate work on a one-segment loop repeated thousands of times. */
            guint limit = MIN(indices->len, a + 5);
            for (guint b = a + 1; b < limit; b++) {
                uint32_t j = g_array_index(indices, uint32_t, b);
                AudioStreamSegmentRecord *rj = &g_array_index(
                    w->segment_records, AudioStreamSegmentRecord, j);
                if (ri->frames != rj->frames || ri->pcm_hash != rj->pcm_hash ||
                    ri->segment_signature != rj->segment_signature ||
                    rj->start_frame <= ri->start_frame) {
                    continue;
                }
                uint64_t period = rj->start_frame - ri->start_frame;
                if (period < AUDIO_LOGICAL_MIN_LOOP_FRAMES ||
                    period > w->frames - rj->start_frame) {
                    continue;
                }
                if (!stream_writer_ranges_equal(w, ri->start_frame,
                                                rj->start_frame, period)) {
                    continue;
                }

                uint64_t loop_start = stream_writer_extend_loop_backward(
                    w, ri->start_frame, period);
                uint64_t loop_end = loop_start + period;
                if (loop_end > w->frames ||
                    !stream_writer_ranges_equal(w, loop_start, loop_end,
                                                period)) {
                    continue;
                }

                uint32_t repetitions = 2;
                uint64_t next_start = loop_start + 2 * period;
                while (next_start <= w->frames &&
                       period <= w->frames - next_start &&
                       stream_writer_ranges_equal(w, loop_start, next_start,
                                                  period)) {
                    repetitions++;
                    if (period > UINT64_MAX - next_start) {
                        break;
                    }
                    next_start += period;
                }
                uint64_t repeated_end =
                    loop_start + (uint64_t)repetitions * period;
                if (repeated_end > w->frames) {
                    repeated_end = w->frames;
                }
                uint64_t removed = (uint64_t)(repetitions - 1) * period;
                if (removed > best_removed ||
                    (removed == best_removed && period > best_period) ||
                    (removed == best_removed && period == best_period &&
                     (!out->found || loop_start < out->loop_start))) {
                    out->found = true;
                    out->loop_start = loop_start;
                    out->loop_end = loop_end;
                    out->repeated_end = repeated_end;
                    out->repetitions = repetitions;
                    best_removed = removed;
                    best_period = period;
                }
            }
        }
    }
    g_hash_table_destroy(occurrences);
    return out->found;
}

typedef struct AudioLoopWindowSeen {
    uint32_t offsets[AUDIO_LOGICAL_SEEN_OFFSETS];
    uint8_t count;
} AudioLoopWindowSeen;

static bool pcm_frames_equal(const int16_t *pcm, unsigned int channels,
                             uint64_t a, uint64_t b, uint64_t frames)
{
    if (!pcm || channels < 1 || channels > 2) {
        return false;
    }
    uint64_t samples = frames * channels;
    if (samples > SIZE_MAX / sizeof(*pcm)) {
        return false;
    }
    return memcmp(&pcm[a * channels], &pcm[b * channels],
                  (size_t)samples * sizeof(*pcm)) == 0;
}

static bool pcm_frame_equal(const int16_t *pcm, unsigned int channels,
                            uint64_t a, uint64_t b)
{
    if (pcm[a * channels] != pcm[b * channels]) {
        return false;
    }
    return channels == 1 ||
           pcm[a * 2 + 1] == pcm[b * 2 + 1];
}

static bool stream_writer_detect_pcm_loop(AudioStreamWriter *w,
                                          AudioLoopDetection *out)
{
    memset(out, 0, sizeof(*out));
    if (!w || !w->file || !w->tmp_path || w->channels < 1 ||
        w->channels > 2 || w->frames < 2 * AUDIO_LOGICAL_MIN_LOOP_FRAMES) {
        return false;
    }
    uint64_t data_bytes = w->frames * w->channels * sizeof(int16_t);
    if (data_bytes == 0 || data_bytes > AUDIO_LOGICAL_PCM_ANALYSIS_MAX_BYTES ||
        data_bytes > SIZE_MAX) {
        return false;
    }
    if (fflush(w->file) != 0) {
        return false;
    }
    FILE *f = g_fopen(w->tmp_path, "rb");
    int16_t *pcm = g_try_malloc((size_t)data_bytes);
    if (!f || !pcm || fseek(f, 44, SEEK_SET) != 0 ||
        fread(pcm, 1, (size_t)data_bytes, f) != (size_t)data_bytes) {
        if (f) fclose(f);
        g_free(pcm);
        return false;
    }
    fclose(f);

    GHashTable *seen = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                              g_free, g_free);
    uint64_t frame_tokens[AUDIO_CONSUMED_WINDOW_FRAMES] = { 0 };
    uint64_t rolling = 0;
    uint32_t write_pos = 0;
    uint32_t valid = 0;
    const uint64_t base = UINT64_C(0x100000001b3);
    const uint64_t channel_tag =
        (uint64_t)w->channels * UINT64_C(0xd6e8feb86659fd93);
    uint64_t best_removed = 0;
    uint64_t best_period = 0;

    for (uint64_t frame = 0; frame < w->frames; frame++) {
        uint64_t token = audio_consumed_frame_token(
            &pcm[frame * w->channels], w->channels);
        if (valid < AUDIO_CONSUMED_WINDOW_FRAMES) {
            rolling = rolling * base + token;
            valid++;
        } else {
            rolling -= frame_tokens[write_pos] * AUDIO_CONSUMED_REMOVE_POW;
            rolling = rolling * base + token;
        }
        frame_tokens[write_pos] = token;
        write_pos = (write_pos + 1) % AUDIO_CONSUMED_WINDOW_FRAMES;
        if (valid < AUDIO_CONSUMED_WINDOW_FRAMES) {
            continue;
        }

        uint64_t window_start = frame + 1 - AUDIO_CONSUMED_WINDOW_FRAMES;
        uint64_t anchor_hash = rolling ^ channel_tag;
        if ((anchor_hash & AUDIO_LOGICAL_LOOP_ANCHOR_MASK) != 0 ||
            !audio_consumed_window_has_signal(
                &pcm[window_start * w->channels], w->channels)) {
            continue;
        }
        uint64_t verify = audio_stream_fingerprint(
            &pcm[window_start * w->channels], AUDIO_CONSUMED_WINDOW_FRAMES,
            w->channels);
        AudioLoopWindowSeen *history = g_hash_table_lookup(seen, &verify);
        if (history) {
            for (uint8_t h = 0; h < history->count; h++) {
                uint64_t prior = history->offsets[h];
                if (window_start <= prior) {
                    continue;
                }
                uint64_t period = window_start - prior;
                if (period < AUDIO_LOGICAL_PCM_MIN_LOOP_FRAMES ||
                    period > w->frames - window_start) {
                    continue;
                }
                if (!pcm_frames_equal(pcm, w->channels, prior, window_start,
                                      period)) {
                    continue;
                }

                uint64_t loop_start = prior;
                while (loop_start > 0 &&
                       pcm_frame_equal(pcm, w->channels, loop_start - 1,
                                       loop_start + period - 1)) {
                    loop_start--;
                }
                uint64_t loop_end = loop_start + period;
                if (loop_end > w->frames || period > w->frames - loop_end ||
                    !pcm_frames_equal(pcm, w->channels, loop_start, loop_end,
                                      period)) {
                    continue;
                }

                uint32_t repetitions = 2;
                uint64_t next_start = loop_start + 2 * period;
                while (next_start <= w->frames &&
                       period <= w->frames - next_start &&
                       pcm_frames_equal(pcm, w->channels, loop_start,
                                        next_start, period)) {
                    repetitions++;
                    if (period > UINT64_MAX - next_start) {
                        break;
                    }
                    next_start += period;
                }
                uint64_t repeated_end =
                    loop_start + (uint64_t)repetitions * period;
                /* With no structural packet boundary, a waveform that is
                 * periodic from frame 0 through EOF has no provable authored
                 * loop length (a tone can repeat every few samples). Leave
                 * that case intact; hardware/segment evidence may still prove
                 * its actual loop above. PCM-only compaction requires an intro
                 * or an outro boundary. */
                if (loop_start == 0 && repeated_end == w->frames) {
                    continue;
                }
                uint64_t removed = (uint64_t)(repetitions - 1) * period;
                if (removed > best_removed ||
                    (removed == best_removed && period > best_period) ||
                    (removed == best_removed && period == best_period &&
                     (!out->found || loop_start < out->loop_start))) {
                    out->found = true;
                    out->loop_start = loop_start;
                    out->loop_end = loop_end;
                    out->repeated_end = repeated_end;
                    out->repetitions = repetitions;
                    best_removed = removed;
                    best_period = period;
                }
            }
        }

        if (!history) {
            uint64_t *key = g_new(uint64_t, 1);
            *key = verify;
            history = g_new0(AudioLoopWindowSeen, 1);
            g_hash_table_insert(seen, key, history);
        }
        if (window_start <= UINT32_MAX) {
            if (history->count < AUDIO_LOGICAL_SEEN_OFFSETS) {
                history->offsets[history->count++] = (uint32_t)window_start;
            } else {
                memmove(&history->offsets[0], &history->offsets[1],
                        (AUDIO_LOGICAL_SEEN_OFFSETS - 1) * sizeof(uint32_t));
                history->offsets[AUDIO_LOGICAL_SEEN_OFFSETS - 1] =
                    (uint32_t)window_start;
            }
        }
    }

    g_hash_table_destroy(seen);
    g_free(pcm);
    return out->found;
}

static bool stream_writer_copy_range(FILE *src, FILE *dst,
                                     const AudioStreamWriter *w,
                                     uint64_t start_frame, uint64_t frames,
                                     XXH3_state_t *hash_state,
                                     int16_t *prefix_pcm,
                                     uint32_t *prefix_frames)
{
    if (!src || !dst || !w || !hash_state || start_frame > w->frames ||
        frames > w->frames - start_frame) {
        return false;
    }
    uint64_t off = 44u + start_frame * w->channels * sizeof(int16_t);
    if (!audio_file_seek_set(src, off)) {
        return false;
    }
    uint8_t buf[65536];
    uint64_t bytes = frames * w->channels * sizeof(int16_t);
    while (bytes != 0) {
        size_t take = (size_t)MIN(bytes, (uint64_t)sizeof(buf));
        if (fread(buf, 1, take, src) != take ||
            fwrite(buf, 1, take, dst) != take ||
            XXH3_64bits_update(hash_state, buf, take) == XXH_ERROR) {
            return false;
        }
        if (*prefix_frames < AUDIO_STREAM_MATCH_MAX_FRAMES) {
            size_t frame_bytes = w->channels * sizeof(int16_t);
            uint32_t available_frames = (uint32_t)(take / frame_bytes);
            uint32_t copy_frames = MIN(available_frames,
                AUDIO_STREAM_MATCH_MAX_FRAMES - *prefix_frames);
            memcpy(&prefix_pcm[(size_t)*prefix_frames * w->channels], buf,
                   (size_t)copy_frames * frame_bytes);
            *prefix_frames += copy_frames;
        }
        bytes -= take;
    }
    return true;
}

static bool reserve_dump_hash(uint64_t hash)
{
    char key[AUDIO_HASH_KEY_LEN];
    hash_key(hash, key);

    bool should_dump = false;
    g_mutex_lock(&g_index_lock);
    bool already_dumped = g_dumped_set &&
                          g_hash_table_contains(g_dumped_set, key);
    bool already_replaced = g_replace_index &&
                            g_hash_table_contains(g_replace_index, key);
    if (!already_dumped &&
        !(g_config.audio.dump_skip_replaced && already_replaced)) {
        if (!g_dumped_set) {
            g_dumped_set = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
        }
        g_hash_table_add(g_dumped_set, g_strdup(key));
        should_dump = true;
    }
    g_mutex_unlock(&g_index_lock);
    return should_dump;
}

static void release_dump_hash(uint64_t hash)
{
    char key[AUDIO_HASH_KEY_LEN];
    hash_key(hash, key);
    g_mutex_lock(&g_index_lock);
    if (g_dumped_set) {
        g_hash_table_remove(g_dumped_set, key);
    }
    g_mutex_unlock(&g_index_lock);
}

static bool stream_writer_publish(AudioStreamWriter *w)
{
    if (!w || !w->file || w->frames == 0) {
        return false;
    }
    /* A partial traversal after loop proof is still duplicate loop material.
     * Do not restore it when capture stops: resource-oriented output keeps the
     * intro, one complete loop body, and only genuinely new post-loop audio. */
    w->loop_cycle_phase = 0;
    if (fflush(w->file) != 0) {
        return false;
    }

    AudioLoopDetection loop = { 0 };
    AudioLoopDetection segment_loop = { 0 };
    AudioLoopDetection pcm_loop = { 0 };
    stream_writer_detect_exact_loop(w, &segment_loop);
    if (segment_loop.found) {
        loop = segment_loop;
    } else if (w->saw_wrap_hint) {
        /* PCM-only boundary refinement is authorized only when Xbox source
         * progression itself wrapped. Exact repeated musical phrases in
         * distinct packets are therefore never reclassified as loops. */
        stream_writer_detect_pcm_loop(w, &pcm_loop);
        if (pcm_loop.found) {
            loop = pcm_loop;
        }
    }
    const char *loop_method = !loop.found ? "none" :
        (pcm_loop.found && loop.loop_start == pcm_loop.loop_start &&
         loop.loop_end == pcm_loop.loop_end &&
         loop.repeated_end == pcm_loop.repeated_end
             ? "exact_pcm_cycle" : "exact_stream_cycle");
    uint64_t removed_frames = loop.found
        ? loop.repeated_end - loop.loop_end : 0;
    uint64_t logical_frames = w->frames - removed_frames;
    if (logical_frames == 0 || logical_frames > UINT32_MAX) {
        return false;
    }
    uint64_t data64 = logical_frames * w->channels * sizeof(int16_t);
    uint64_t riff64 = 36u + data64 + (loop.found ? 68u : 0u);
    if (data64 > UINT32_MAX || riff64 > UINT32_MAX) {
        return false;
    }

    g_autofree char *logical_tmp = g_strdup_printf("%s.logical", w->tmp_path);
    FILE *src = g_fopen(w->tmp_path, "rb");
    FILE *dst = g_fopen(logical_tmp, "wb");
    XXH3_state_t *logical_hash = XXH3_createState();
    int16_t *prefix_pcm = g_try_malloc_n(
        (size_t)AUDIO_STREAM_MATCH_MAX_FRAMES * w->channels,
        sizeof(*prefix_pcm));
    uint32_t prefix_frames = 0;
    bool ok = src && dst && logical_hash && prefix_pcm &&
              XXH3_64bits_reset(logical_hash) != XXH_ERROR &&
              write_pcm16_wav_header_ex(dst, w->channels, w->sample_rate,
                                        (uint32_t)data64, loop.found);
    if (ok) {
        if (loop.found) {
            ok = stream_writer_copy_range(src, dst, w, 0, loop.loop_end,
                                          logical_hash, prefix_pcm,
                                          &prefix_frames);
            if (ok && loop.repeated_end < w->frames) {
                ok = stream_writer_copy_range(src, dst, w, loop.repeated_end,
                                              w->frames - loop.repeated_end,
                                              logical_hash, prefix_pcm,
                                              &prefix_frames);
            }
        } else {
            ok = stream_writer_copy_range(src, dst, w, 0, w->frames,
                                          logical_hash, prefix_pcm,
                                          &prefix_frames);
        }
    }
    if (ok && loop.found) {
        ok = write_wav_smpl_chunk(dst, w->sample_rate,
                                  (uint32_t)loop.loop_start,
                                  (uint32_t)loop.loop_end);
    }
    if (src) fclose(src);
    if (dst && fclose(dst) != 0) ok = false;
    if (!ok) {
        if (dst) { /* fclose above already attempted */ }
        if (logical_hash) XXH3_freeState(logical_hash);
        g_free(prefix_pcm);
        g_remove(logical_tmp);
        return false;
    }

    uint64_t hash = XXH3_64bits_digest(logical_hash);
    XXH3_freeState(logical_hash);
    uint8_t channel_tag = w->channels;
    hash ^= fast_hash(&channel_tag, sizeof(channel_tag));
    if (!reserve_dump_hash(hash)) {
        g_free(prefix_pcm);
        g_remove(logical_tmp);
        return true;
    }

    char key[AUDIO_HASH_KEY_LEN];
    hash_key(hash, key);
    g_autofree char *wav_path = g_strdup_printf("%s%c%s.wav", w->dump_dir,
                                                 G_DIR_SEPARATOR, key);
    g_autofree char *json_path = g_strdup_printf("%s%c%s.json", w->dump_dir,
                                                  G_DIR_SEPARATOR, key);
    if (g_file_test(wav_path, G_FILE_TEST_EXISTS) ||
        g_rename(logical_tmp, wav_path) != 0) {
        release_dump_hash(hash);
        g_free(prefix_pcm);
        g_remove(logical_tmp);
        return false;
    }

    AudioDumpJob metadata = { 0 };
    metadata.hash = hash;
    metadata.frames = (uint32_t)logical_frames;
    metadata.observed_frames = w->observed_frames ? w->observed_frames : w->frames;
    metadata.sample_rate = w->sample_rate;
    metadata.channels = w->channels;
    metadata.streaming = true;
    metadata.stream_segments = w->segments;
    metadata.loop = loop.found;
    metadata.loop_start = loop.found ? (uint32_t)loop.loop_start : 0;
    metadata.loop_end = loop.found ? (uint32_t)loop.loop_end : 0;
    metadata.loop_repetitions_observed = loop.found
        ? MIN((uint64_t)UINT32_MAX,
              (uint64_t)loop.repetitions + w->loop_repetitions_suppressed)
        : 0;
    g_strlcpy(metadata.loop_detection, loop_method,
              sizeof(metadata.loop_detection));
    metadata.wav_path = wav_path;
    metadata.json_path = json_path;
    for (guint i = 0; i < AUDIO_STREAM_FINGERPRINT_TIER_COUNT; i++) {
        uint32_t tier_frames = audio_stream_fingerprint_tiers[i];
        if (prefix_frames < tier_frames) {
            continue;
        }
        metadata.stream_fingerprints[i] =
            audio_stream_fingerprint(prefix_pcm, tier_frames, w->channels);
        metadata.stream_fingerprint_mask |= 1u << i;
    }
    g_free(prefix_pcm);
    g_strlcpy(metadata.source_format, w->source_format,
              sizeof(metadata.source_format));
    write_metadata_json(&metadata);

    if (loop.found) {
        fprintf(stderr,
                "mcpx: audio-io: logical stream extraction %016" PRIx64
                " preserved intro=%" PRIu64 ", loop=%" PRIu64
                "..%" PRIu64 ", outro=%" PRIu64
                " frames; collapsed %u observed loop traversals to one\n",
                hash, loop.loop_start, loop.loop_start, loop.loop_end,
                logical_frames - loop.loop_end,
                metadata.loop_repetitions_observed);
    }
    return true;
}

static gpointer dump_worker(gpointer unused)
{
    GHashTable *streams = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                                 g_free, stream_writer_free);
    for (;;) {
        gpointer p = g_async_queue_pop(g_dump_queue);
        if (p == AUDIO_DUMP_SENTINEL) {
            break;
        }
        AudioDumpJob *job = p;
        switch (job->kind) {
        case AUDIO_DUMP_JOB_STATIC:
            if (write_pcm16_wav(job)) {
                write_metadata_json(job);
            } else {
                fprintf(stderr, "mcpx: audio-io: failed to dump %s\n",
                        job->wav_path ? job->wav_path : "(unknown)");
            }
            break;
        case AUDIO_DUMP_JOB_STREAM_BEGIN: {
            AudioStreamWriter *old = g_hash_table_lookup(streams,
                                                         &job->stream_session);
            if (old) {
                g_hash_table_remove(streams, &job->stream_session);
            }
            AudioStreamWriter *w = stream_writer_begin(job);
            if (w) {
                uint64_t *key = g_new(uint64_t, 1);
                *key = job->stream_session;
                g_hash_table_insert(streams, key, w);
            }
            break;
        }
        case AUDIO_DUMP_JOB_STREAM_APPEND: {
            AudioStreamWriter *w = g_hash_table_lookup(streams,
                                                       &job->stream_session);
            if (w && !stream_writer_append(w, job)) {
                g_hash_table_remove(streams, &job->stream_session);
            }
            break;
        }
        case AUDIO_DUMP_JOB_STREAM_END: {
            AudioStreamWriter *w = g_hash_table_lookup(streams,
                                                       &job->stream_session);
            if (w && !stream_writer_publish(w)) {
                fprintf(stderr, "mcpx: audio-io: failed to finalize streaming dump\n");
            }
            g_hash_table_remove(streams, &job->stream_session);
            break;
        }
        case AUDIO_DUMP_JOB_STREAM_CANCEL:
            g_hash_table_remove(streams, &job->stream_session);
            break;
        }
        audio_dump_job_free(job);
    }
    g_hash_table_destroy(streams);
    return NULL;
}

static void ensure_dump_worker(void)
{
    /* Several VP workers can discover new sounds in the same APU frame. */
    g_mutex_lock(&g_dump_worker_lock);
    if (!g_dump_thread && !g_dump_worker_stopping) {
        if (!g_dump_queue) {
            g_dump_queue = g_async_queue_new();
        }
        g_dump_thread = g_thread_new("xemu.audio-dump", dump_worker, NULL);
    }
    g_mutex_unlock(&g_dump_worker_lock);
}

static void queue_dump(uint64_t hash, const int16_t *pcm, uint32_t frames,
                       unsigned int channels, uint32_t sample_rate, bool loop,
                       uint32_t loop_start, const char *source_format)
{
    if (!g_config.audio.dump_enabled || !pcm || frames == 0) {
        return;
    }

    char key[AUDIO_HASH_KEY_LEN];
    hash_key(hash, key);
    char *dump_dir = NULL;
    bool should_dump = false;

    g_mutex_lock(&g_index_lock);
    bool already_dumped = g_dumped_set &&
                          g_hash_table_contains(g_dumped_set, key);
    bool already_replaced = g_replace_index &&
                            g_hash_table_contains(g_replace_index, key);
    if (g_paths_valid && g_dump_dir && !already_dumped &&
        !(g_config.audio.dump_skip_replaced && already_replaced)) {
        if (!g_dumped_set) {
            g_dumped_set = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
        }
        /* Mark before queueing so repeated voices never enqueue duplicates. */
        g_hash_table_add(g_dumped_set, g_strdup(key));
        dump_dir = g_strdup(g_dump_dir);
        should_dump = true;
    }
    g_mutex_unlock(&g_index_lock);
    if (!should_dump) {
        return;
    }

    size_t sample_count = (size_t)frames * channels;
    int16_t *copy = g_try_malloc_n(sample_count, sizeof(*copy));
    if (!copy) {
        g_mutex_lock(&g_index_lock);
        if (g_dumped_set) {
            g_hash_table_remove(g_dumped_set, key);
        }
        g_mutex_unlock(&g_index_lock);
        g_free(dump_dir);
        return;
    }
    memcpy(copy, pcm, sample_count * sizeof(*copy));

    AudioDumpJob *job = g_new0(AudioDumpJob, 1);
    job->kind = AUDIO_DUMP_JOB_STATIC;
    job->pcm = copy;
    job->frames = frames;
    job->sample_rate = sample_rate;
    job->channels = channels;
    job->hash = hash;
    job->loop = loop;
    job->loop_start = loop ? MIN(loop_start, frames - 1) : 0;
    job->loop_end = loop ? frames : 0;
    job->observed_frames = frames;
    job->loop_repetitions_observed = loop ? 1 : 0;
    g_strlcpy(job->loop_detection, loop ? "hardware_lbo_ebo" : "none",
              sizeof(job->loop_detection));
    for (guint i = 0; i < AUDIO_STREAM_FINGERPRINT_TIER_COUNT; i++) {
        uint32_t tier_frames = audio_stream_fingerprint_tiers[i];
        if (frames < tier_frames) {
            break;
        }
        job->stream_fingerprints[i] =
            audio_stream_fingerprint(pcm, tier_frames, channels);
        job->stream_fingerprint_mask |= 1u << i;
    }
    g_strlcpy(job->source_format, source_format ? source_format : "unknown",
              sizeof(job->source_format));
    job->wav_path = g_strdup_printf("%s%c%s.wav", dump_dir,
                                    G_DIR_SEPARATOR, key);
    job->json_path = g_strdup_printf("%s%c%s.json", dump_dir,
                                     G_DIR_SEPARATOR, key);
    g_free(dump_dir);

    ensure_dump_worker();
    g_async_queue_push(g_dump_queue, job);
}


static void queue_stream_job(AudioDumpJobKind kind, uint64_t session,
                             const int16_t *pcm, uint32_t frames,
                             unsigned int channels, uint32_t sample_rate,
                             const char *source_format, const char *dump_dir,
                             uint64_t segment_signature, bool wrap_hint)
{
    AudioDumpJob *job = g_new0(AudioDumpJob, 1);
    job->kind = kind;
    job->stream_session = session;
    job->stream_segment_signature = segment_signature;
    job->stream_wrap_hint = wrap_hint;
    job->frames = frames;
    job->channels = channels;
    job->sample_rate = sample_rate;
    if (source_format) {
        g_strlcpy(job->source_format, source_format, sizeof(job->source_format));
    }
    if (dump_dir) {
        job->dump_dir = g_strdup(dump_dir);
    }
    if (pcm && frames && channels) {
        size_t count = (size_t)frames * channels;
        job->pcm = g_try_malloc_n(count, sizeof(*job->pcm));
        if (!job->pcm) {
            audio_dump_job_free(job);
            return;
        }
        memcpy(job->pcm, pcm, count * sizeof(*job->pcm));
    }
    ensure_dump_worker();
    if (!g_dump_queue) {
        audio_dump_job_free(job);
        return;
    }
    g_async_queue_push(g_dump_queue, job);
}

/* Forward declarations: stream matching is implemented before the shared
 * asset/replacement materialization helpers below. */
static AudioAsset *find_or_create_asset(uint64_t hash, uint32_t source_rate,
                                        uint32_t frames, bool loop,
                                        uint32_t loop_start,
                                        const char *source_format);
static AudioReplacement *select_asset_replacement(AudioAsset *asset,
                                                   uint64_t hash,
                                                   unsigned int channels);
static AudioReplacement *select_asset_replacement_mode(AudioAsset *asset,
                                                        uint64_t hash,
                                                        unsigned int channels,
                                                        bool source_rate);

static void finish_stream_dump_locked(AudioStreamCapture *sc, bool cancel)
{
    if (!sc->active) {
        return;
    }
    queue_stream_job(cancel ? AUDIO_DUMP_JOB_STREAM_CANCEL
                            : AUDIO_DUMP_JOB_STREAM_END,
                     sc->session, NULL, 0, 0, 0, NULL, NULL, 0, false);
    sc->active = false;
    sc->session = 0;
}

static void reset_stream_capture_locked(AudioStreamCapture *sc, bool cancel)
{
    finish_stream_dump_locked(sc, cancel);
    XXH3_state_t *match_hash_state = sc->match_hash_state;
    memset(sc, 0, sizeof(*sc));
    sc->match_hash_state = match_hash_state;
    if (match_hash_state) {
        XXH3_64bits_reset(match_hash_state);
    }
}

static void finish_all_stream_dumps(bool cancel)
{
    g_mutex_lock(&g_stream_capture_lock);
    for (unsigned int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        finish_stream_dump_locked(&g_stream_captures[v], cancel);
    }
    g_mutex_unlock(&g_stream_capture_lock);
}

static void reset_all_stream_captures(bool cancel)
{
    g_mutex_lock(&g_stream_capture_lock);
    for (unsigned int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        reset_stream_capture_locked(&g_stream_captures[v], cancel);
    }
    g_mutex_unlock(&g_stream_capture_lock);
}

static void audio_stream_match_generation_changed(void)
{
    g_mutex_lock(&g_stream_capture_lock);
    for (unsigned int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        AudioStreamCapture *sc = &g_stream_captures[v];
        bool was_identified = sc->replacement_identified;
        sc->replacement_identified = false;
        sc->matched_source_hash = 0;
        sc->match_tried_mask = 0;

        /* Before identification we retain the incremental prefix and can safely
         * retry it against the new immutable index. After identification,
         * replacement-only steady state intentionally stops snapshotting later
         * SSL segments. A mid-stream index reload therefore no longer has a
         * contiguous source prefix; do not fabricate one by appending the
         * current later packet. The stream becomes matchable again on its next
         * normal voice/logical-stream reset. */
        sc->match_exhausted = was_identified;
    }
    g_mutex_unlock(&g_stream_capture_lock);
}

void xemu_audio_packs_finish_stream_dumps(void)
{
    finish_all_stream_dumps(false);
}

void xemu_audio_packs_cancel_stream_dumps(void)
{
    finish_all_stream_dumps(true);
}

static bool lookup_stream_source_hash(uint64_t fingerprint,
                                      uint64_t *source_hash)
{
    bool found = false;
    g_mutex_lock(&g_index_lock);
    AudioStreamMatchEntry *entry = g_stream_match_index ?
        g_hash_table_lookup(g_stream_match_index, &fingerprint) : NULL;
    if (entry && !entry->ambiguous) {
        if (source_hash) {
            *source_hash = entry->source_hash;
        }
        found = true;
    }
    g_mutex_unlock(&g_index_lock);
    return found;
}

static bool lookup_static_source_hash(uint64_t fingerprint,
                                      uint64_t *out_hash)
{
    if (!fingerprint || !out_hash || qatomic_read(&g_static_match_count) == 0) {
        return false;
    }

    bool found = false;
    g_mutex_lock(&g_index_lock);
    AudioStreamMatchEntry *entry = g_static_match_index ?
        g_hash_table_lookup(g_static_match_index, &fingerprint) : NULL;
    if (entry && !entry->ambiguous) {
        *out_hash = entry->source_hash;
        found = true;
    }
    g_mutex_unlock(&g_index_lock);
    return found;
}

static bool match_static_source_prefix(const int16_t *pcm, uint32_t frames,
                                       unsigned int channels,
                                       uint64_t *out_hash)
{
    if (!pcm || !out_hash || frames < audio_stream_fingerprint_tiers[0] ||
        channels < 1 || channels > 2 ||
        qatomic_read(&g_static_match_count) == 0) {
        return false;
    }

    /* Longest available unique prefix wins. Shared silence/codec priming at
     * short tiers therefore cannot steal a match from a more specific prefix. */
    for (gint i = AUDIO_STREAM_FINGERPRINT_TIER_COUNT - 1; i >= 0; i--) {
        uint32_t tier_frames = audio_stream_fingerprint_tiers[i];
        if (frames < tier_frames) {
            continue;
        }
        uint64_t fingerprint =
            audio_stream_fingerprint(pcm, tier_frames, channels);
        uint64_t source_hash = 0;
        if (lookup_static_source_hash(fingerprint, &source_hash)) {
            *out_hash = source_hash;
            return true;
        }
    }
    return false;
}

static uint32_t lookup_source_frames(uint64_t hash)
{
    char key[AUDIO_HASH_KEY_LEN];
    hash_key(hash, key);

    uint32_t frames = 0;
    g_mutex_lock(&g_index_lock);
    AudioReplacementIndexGroup *group =
        (g_paths_valid && g_replace_index) ?
        g_hash_table_lookup(g_replace_index, key) : NULL;
    if (group) {
        frames = group->source_frames;
    }
    g_mutex_unlock(&g_index_lock);
    return frames;
}


typedef struct AudioConsumedMatchResult {
    uint64_t source_hash;
    uint32_t source_offset;
    uint64_t window_start;
} AudioConsumedMatchResult;

static int16_t audio_consumed_float_to_s16(float v, bool exact_s16)
{
    if (v >= 1.0f) {
        return INT16_MAX;
    }
    if (v <= -1.0f) {
        return INT16_MIN;
    }
    /* Xbox ADPCM enters voice_get_samples() as an exact signed-16 decoder
     * output normalized by 32768. The dump path intentionally stores those
     * decoder samples verbatim, so reconstruct the same int16 here. Other
     * PCM widths follow the dump path's historical 32767 float quantization. */
    return (int16_t)lrintf(v * (exact_s16 ? 32768.0f : 32767.0f));
}

static bool lookup_consumed_window(uint64_t anchor, uint64_t verify_hash,
                                   uint64_t *source_hash,
                                   uint32_t *source_offset)
{
    if (!verify_hash || !source_hash || !source_offset ||
        qatomic_read(&g_consumed_window_unique_count) == 0) {
        return false;
    }

    bool found = false;
    g_mutex_lock(&g_index_lock);
    AudioConsumedWindowBucket *bucket = g_consumed_window_index ?
        g_hash_table_lookup(g_consumed_window_index, &anchor) : NULL;
    if (bucket && bucket->entries) {
        for (guint i = 0; i < bucket->entries->len; i++) {
            AudioConsumedWindowEntry *entry = &g_array_index(
                bucket->entries, AudioConsumedWindowEntry, i);
            if (entry->verify_hash == verify_hash && !entry->ambiguous) {
                *source_hash = entry->source_hash;
                *source_offset = entry->source_offset;
                found = true;
                break;
            }
        }
    }
    g_mutex_unlock(&g_index_lock);
    return found;
}

static void audio_consumed_copy_ordered_window(const AudioConsumedVoiceState *cm,
                                               int16_t *out_pcm)
{
    for (uint32_t i = 0; i < AUDIO_CONSUMED_WINDOW_FRAMES; i++) {
        uint32_t ring = (cm->write_pos + i) % AUDIO_CONSUMED_WINDOW_FRAMES;
        out_pcm[(size_t)i * cm->channels] = cm->pcm[ring * 2];
        if (cm->channels == 2) {
            out_pcm[(size_t)i * 2 + 1] = cm->pcm[ring * 2 + 1];
        }
    }
}

static bool audio_consumed_feed(unsigned int voice, float samples[][2],
                                int count, unsigned int channels,
                                bool exact_s16,
                                uint64_t prior_source_hash,
                                uint32_t *prior_source_last_offset,
                                AudioConsumedMatchResult *out)
{
    if (voice >= MCPX_HW_MAX_VOICES || !samples || count <= 0 || !out ||
        channels < 1 || channels > 2 ||
        qatomic_read(&g_consumed_window_unique_count) == 0) {
        return false;
    }

    AudioConsumedVoiceState *cm = &g_consumed_voices[voice];
    if (cm->channels && cm->channels != channels) {
        reset_consumed_voice_state(voice);
        cm = &g_consumed_voices[voice];
    }
    cm->channels = channels;

    const uint64_t base = UINT64_C(0x100000001b3);
    const uint64_t remove_pow = AUDIO_CONSUMED_REMOVE_POW;
    const uint64_t channel_tag =
        (uint64_t)channels * UINT64_C(0xd6e8feb86659fd93);
    bool matched = false;

    for (int frame = 0; frame < count; frame++) {
        int16_t left = audio_consumed_float_to_s16(samples[frame][0],
                                                   exact_s16);
        int16_t right = channels == 2
            ? audio_consumed_float_to_s16(samples[frame][1], exact_s16) : 0;
        int16_t packed[2] = { left, right };
        uint64_t token = audio_consumed_frame_token(packed, channels);
        uint32_t pos = cm->write_pos;

        if (cm->valid_frames < AUDIO_CONSUMED_WINDOW_FRAMES) {
            cm->rolling_hash *= base;
            cm->rolling_hash += token;
            cm->valid_frames++;
        } else {
            uint64_t old_token = cm->frame_tokens[pos];
            cm->rolling_hash -= old_token * remove_pow;
            cm->rolling_hash *= base;
            cm->rolling_hash += token;
        }
        cm->frame_tokens[pos] = token;
        cm->pcm[pos * 2] = left;
        cm->pcm[pos * 2 + 1] = right;
        cm->write_pos = (pos + 1) % AUDIO_CONSUMED_WINDOW_FRAMES;
        cm->total_frames++;

        if (cm->valid_frames < AUDIO_CONSUMED_WINDOW_FRAMES) {
            continue;
        }
        /* Once this block has produced its first accepted logical-source
         * transition, keep consuming the remainder into the rolling window but
         * defer any later transition to the next callback. This preserves an
         * exact continuous source history without trying to perform two custom
         * replacement switches inside one 32-frame MCPX source block. */
        if (matched) {
            continue;
        }
        uint64_t anchor = cm->rolling_hash ^ channel_tag;
        if ((anchor & AUDIO_CONSUMED_ANCHOR_MASK) != 0 ||
            !audio_consumed_anchor_maybe_present(anchor)) {
            continue;
        }

        int16_t ordered[AUDIO_CONSUMED_WINDOW_FRAMES * 2];
        audio_consumed_copy_ordered_window(cm, ordered);
        if (!audio_consumed_window_has_signal(ordered, channels)) {
            continue;
        }
        uint64_t verify = audio_stream_fingerprint(
            ordered, AUDIO_CONSUMED_WINDOW_FRAMES, channels);
        uint64_t source_hash = 0;
        uint32_t source_offset = 0;
        if (!lookup_consumed_window(anchor, verify, &source_hash,
                                    &source_offset)) {
            continue;
        }
        if (prior_source_hash != 0 && source_hash == prior_source_hash &&
            prior_source_last_offset) {
            /* While looking for the next logical sound in one persistent ring,
             * landmarks from the tail of the source we just replaced must not
             * restart that replacement. A genuine replay/loop of the same
             * source is visible as its unique landmark offsets wrapping
             * backwards. */
            if (source_offset >= *prior_source_last_offset) {
                *prior_source_last_offset = source_offset;
                continue;
            }
        }

        out->source_hash = source_hash;
        out->source_offset = source_offset;
        out->window_start = cm->total_frames - AUDIO_CONSUMED_WINDOW_FRAMES;
        matched = true;
    }
    return matched;
}

static bool prepare_consumed_replacement(unsigned int voice, uint64_t hash,
                                         unsigned int channels,
                                         uint32_t observed_source_rate,
                                         bool streaming, bool loop,
                                         uint32_t loop_start,
                                         const char *source_format)
{
    if (voice >= MCPX_HW_MAX_VOICES || channels < 1 || channels > 2 ||
        !g_config.audio.replace_enabled) {
        return false;
    }

    uint32_t source_frames = lookup_source_frames(hash);
    if (source_frames == 0) {
        return false;
    }
    AudioAsset *asset = find_or_create_asset(hash, observed_source_rate,
                                             source_frames, loop, loop_start,
                                             source_format);
    AudioReplacement *replacement = select_asset_replacement_mode(
        asset, hash, channels, true);
    if (!replacement || replacement->sample_rate != asset->canonical_rate) {
        return false;
    }

    AudioVoiceState *vs = &g_voices[voice];
    vs->prepared = true;
    vs->replacement_active = true;
    vs->passthrough_source = true;
    vs->streaming = streaming;
    vs->asset = asset;
    vs->replacement = replacement;
    vs->cursor = 0;
    vs->stream_source_cursor = 0;
    vs->replacement_loop_start = replacement->frames > 0
        ? MIN(loop_start, replacement->frames - 1) : 0;
    vs->replacement_rate_scale = 1.0f;
    vs->observed_source_rate = observed_source_rate;
    vs->source_channels = channels;
    g_strlcpy(vs->observed_source_format,
              source_format ? source_format : "",
              sizeof(vs->observed_source_format));
    vs->finished = false;
    return true;
}

static bool audio_consumed_override_passthrough(AudioVoiceState *vs,
                                                float samples[][2], int count,
                                                int start_frame,
                                                uint64_t start_cursor)
{
    if (!vs || !samples || count <= 0 || start_frame >= count ||
        !vs->passthrough_source || !vs->replacement_active || !vs->asset ||
        !vs->replacement) {
        return false;
    }

    AudioReplacement *r = vs->replacement;
    vs->cursor = start_cursor;
    for (int i = MAX(0, start_frame); i < count; i++) {
        if (vs->cursor >= r->frames && vs->asset->loop && r->frames > 0) {
            vs->cursor = MIN((uint64_t)vs->replacement_loop_start,
                             (uint64_t)r->frames - 1);
        }

        if (vs->cursor < r->frames) {
            if (r->channels == 2) {
                samples[i][0] = r->samples[vs->cursor * 2];
                samples[i][1] = r->samples[vs->cursor * 2 + 1];
            } else {
                float mono = r->samples[vs->cursor];
                samples[i][0] = mono;
                samples[i][1] = mono;
            }
            vs->cursor++;
        } else {
            /* Preserve the replacement invariant: once a logical source has
             * been identified, a shorter custom WAV must not leak the original
             * source back through. Mark the replacement complete once; the
             * consumed matcher has continued tracking the untouched native PCM
             * in parallel, so a persistent software ring can identify the next
             * logical sound without losing the rolling history at this edge. */
            if (!vs->finished) {
                vs->finished = true;
            }
            samples[i][0] = 0.0f;
            samples[i][1] = 0.0f;
        }
        vs->stream_source_cursor++;
    }
    return true;
}

bool xemu_audio_packs_process_consumed_source(unsigned int voice,
                                              float samples[][2], int count)
{
    /* This hook exists solely for replacement identity/substitution. With
     * replacement disabled, do not touch per-voice matcher state or atomics
     * for every consumed native source block. */
    if (!g_config.audio.replace_enabled ||
        voice >= MCPX_HW_MAX_VOICES || !samples || count <= 0) {
        return false;
    }
    AudioVoiceState *vs = &g_voices[voice];

    bool active_stream_override =
        vs->streaming && vs->replacement_active && !vs->passthrough_source;

    /* Native/static exact-hash replacement has already supplied these samples
     * inside voice_get_samples(); never feed replacement audio back into the
     * source matcher. */
    if (vs->replacement_active && !vs->passthrough_source && !vs->streaming) {
        return false;
    }

    uint32_t event_seq = qatomic_read(&g_guest_cbo_event_seq[voice]);
    if (vs->passthrough_source && event_seq != vs->guest_cbo_event_seen) {
        /* A real guest SetCurrentPosition/reuse event changes the source-ring
         * mapping. Stop substituting immediately and reacquire identity from
         * the newly consumed PCM rather than guessing a cursor. */
        vs->guest_cbo_event_seen = event_seq;
        vs->replacement_active = false;
        vs->passthrough_source = false;
        vs->replacement = NULL;
        reset_consumed_voice_state(voice);
    }

    bool active_passthrough =
        vs->passthrough_source && vs->replacement_active && vs->replacement;
    bool matcher_available =
        g_config.audio.replace_enabled &&
        qatomic_read(&g_consumed_window_unique_count) != 0 &&
        vs->source_channels >= 1 && vs->source_channels <= 2;
    if (!matcher_available) {
        if (active_stream_override) {
            return xemu_audio_packs_stream_override_samples(voice, samples,
                                                             count);
        }
        return active_passthrough ?
            audio_consumed_override_passthrough(vs, samples, count, 0,
                                                 vs->cursor) : false;
    }

    /* Even while a passthrough replacement is playing, keep observing the
     * untouched native source block that arrived at this hook. This is what
     * makes the matcher independent of guest voice boundaries: a software ring
     * may begin the next logical sound before the previous custom WAV ends and
     * may never issue VOICE_OFF or SetCurrentPosition between them. */
    AudioConsumedVoiceState *cm = &g_consumed_voices[voice];
    uint64_t block_start = cm->total_frames;
    AudioConsumedMatchResult match = { 0 };
    bool exact_s16 =
        strcmp(vs->observed_source_format, "xbox_adpcm") == 0;
    uint64_t prior_source_hash = (active_passthrough || active_stream_override)
        ? vs->consumed_source_hash : 0;
    uint32_t *prior_source_last_offset =
        (active_passthrough || active_stream_override)
        ? &vs->consumed_last_seen_offset : NULL;
    if (!audio_consumed_feed(voice, samples, count, vs->source_channels,
                             exact_s16,
                             prior_source_hash, prior_source_last_offset,
                             &match)) {
        if (active_stream_override) {
            return xemu_audio_packs_stream_override_samples(voice, samples,
                                                             count);
        }
        return active_passthrough ?
            audio_consumed_override_passthrough(vs, samples, count, 0,
                                                 vs->cursor) : false;
    }

    /* The verified window can begin part-way through this already-fetched
     * native block. If a previous passthrough replacement was active, preserve
     * it for the prefix that still belongs to the old logical source. The new
     * replacement takes over exactly at the matched window boundary. */
    int64_t delta = (int64_t)match.window_start - (int64_t)block_start;
    int start_frame = delta >= 0
        ? (int)MIN((uint64_t)count, (uint64_t)delta) : 0;
    uint64_t elapsed_before_block = delta < 0 ? (uint64_t)(-delta) : 0;
    if (active_stream_override) {
        /* Feed/match above saw the untouched guest stream. Preserve the old
         * exact/prefix stream replacement for this block first; if the new
         * consumed match materializes successfully, its suffix overwrite below
         * takes over at the verified transition point. */
        xemu_audio_packs_stream_override_samples(voice, samples, count);
    } else if (active_passthrough && start_frame > 0) {
        audio_consumed_override_passthrough(vs, samples, start_frame, 0,
                                             vs->cursor);
    }

    bool streaming = vs->streaming;
    /* A consumed-window match is deliberately transport-agnostic. Hardware
     * ring/SSL looping belongs to the native source reader; looping the custom
     * WAV here would turn a reused backing ring into an endlessly repeating
     * voice line. Repeated/looped logical sources are reacquired from the
     * consumed PCM when their landmark offsets wrap/restart. */
    bool loop = false;
    uint32_t loop_start = 0;
    char source_format[24];
    g_strlcpy(source_format,
              (vs->asset && vs->asset->source_format[0])
                  ? vs->asset->source_format
                  : (streaming ? "consumed_stream" : "consumed_buffer"),
              sizeof(source_format));
    uint32_t source_rate = vs->observed_source_rate;
    if (!source_rate) {
        source_rate = lookup_reference_rate(match.source_hash);
    }
    if (!prepare_consumed_replacement(voice, match.source_hash,
                                      vs->source_channels,
                                      MAX(1u, source_rate), streaming, loop,
                                      loop_start, source_format)) {
        if (active_stream_override) {
            return true;
        }
        return active_passthrough
            ? audio_consumed_override_passthrough(vs, samples, count,
                                                   start_frame, vs->cursor)
            : false;
    }
    vs = &g_voices[voice];
    vs->guest_cbo_event_seen = event_seq;
    vs->consumed_source_hash = match.source_hash;
    vs->consumed_match_offset = match.source_offset;
    vs->consumed_last_seen_offset = match.source_offset;

    /* The source-window offset is identity evidence first, but it can also be
     * a trustworthy playback cursor when a replacement deliberately preserves
     * the source layout. Detect that case from duration. A roughly same-length
     * remaster starts at the verified source offset; a much shorter/longer clean
     * replacement (the common reused-ring case) starts at frame 0 at the first
     * verified landmark instead of requiring the custom WAV to reproduce the
     * game's entire backing-ring layout. */
    uint64_t cursor = 0;
    uint32_t source_frames = lookup_source_frames(match.source_hash);
    bool layout_preserving = streaming && source_frames > 0 &&
        vs->replacement &&
        (uint64_t)vs->replacement->frames * 4 >= (uint64_t)source_frames * 3 &&
        (uint64_t)vs->replacement->frames * 4 <= (uint64_t)source_frames * 5;
    if (layout_preserving) {
        cursor = MIN((uint64_t)match.source_offset,
                     (uint64_t)vs->replacement->frames);
    }
    if (elapsed_before_block != 0) {
        cursor = MIN(cursor + elapsed_before_block,
                     (uint64_t)vs->replacement->frames);
    }

    fprintf(stderr,
            "mcpx: audio-io: consumed-source window %s %016" PRIx64
            " at source frame %u (voice %u, %s, block cursor %" PRIu64 ")\n",
            (active_passthrough || active_stream_override) ?
                "switched to" : "matched",
            match.source_hash, match.source_offset, voice,
            streaming ? "SSL/stream" : "resident/ring", cursor);
    return audio_consumed_override_passthrough(vs, samples, count, start_frame,
                                               cursor);
}

static bool prepare_stream_replacement(unsigned int voice, uint64_t hash,
                                       uint32_t observed_source_rate,
                                       unsigned int channels,
                                       uint64_t source_frame_offset,
                                       const char *source_format)
{
    if (voice >= MCPX_HW_MAX_VOICES || channels < 1 || channels > 2 ||
        !g_config.audio.replace_enabled) {
        return false;
    }

    AudioVoiceState *vs = &g_voices[voice];
    if (vs->replacement_active && vs->streaming && vs->asset &&
        vs->asset->hash == hash) {
        return true;
    }

    uint32_t source_frames = lookup_source_frames(hash);
    if (source_frames == 0) {
        uint64_t minimum_frames = source_frame_offset == UINT64_MAX
            ? UINT64_MAX : source_frame_offset + 1;
        source_frames = (uint32_t)MIN((uint64_t)UINT32_MAX,
                                     MAX(UINT64_C(1), minimum_frames));
    }
    AudioAsset *asset = find_or_create_asset(hash, observed_source_rate,
                                             source_frames, false, 0,
                                             source_format);
    AudioReplacement *replacement =
        select_asset_replacement_mode(asset, hash, channels, true);
    if (!replacement || replacement->sample_rate != asset->canonical_rate) {
        /* A matchable stream must have source-rate metadata, and its preload is
         * normalized to that rate. Reject rather than perturbing guest SSL/CBO
         * timing if a malformed pack somehow violates that invariant. */
        return false;
    }

    uint32_t seen = qatomic_read(&g_guest_cbo_event_seq[voice]);
    memset(vs, 0, sizeof(*vs));
    reset_consumed_voice_state(voice);
    vs->prepared = true;
    vs->streaming = true;
    vs->replacement_active = true;
    vs->asset = asset;
    vs->replacement = replacement;
    vs->guest_cbo_event_seen = seen;
    vs->stream_source_cursor = source_frame_offset;
    /* Stream replacements were pre-resampled to the source canonical rate
     * during index construction. Keep cursor/CBO progression 1:1 with the
     * original SSL stream; native pitch/envelope/filter processing then sees
     * the replacement exactly where the source samples would have been. */
    vs->cursor = MIN(source_frame_offset, (uint64_t)replacement->frames);
    vs->replacement_rate_scale = 1.0f;
    vs->observed_source_rate = observed_source_rate;
    vs->source_channels = channels;
    g_strlcpy(vs->observed_source_format,
              source_format ? source_format : "",
              sizeof(vs->observed_source_format));
    vs->consumed_source_hash = hash;
    vs->consumed_match_offset = (uint32_t)MIN(source_frame_offset,
                                               (uint64_t)UINT32_MAX);
    vs->consumed_last_seen_offset = vs->consumed_match_offset;

    fprintf(stderr,
            "mcpx: audio-io: stream replacement %016" PRIx64
            " matched at source frame %" PRIu64
            " (%u ch, %u Hz -> %u frames @ %u Hz)\n",
            hash, source_frame_offset, channels, asset->canonical_rate,
            replacement->frames, replacement->sample_rate);
    return true;
}

bool xemu_audio_packs_stream_segment_needed(unsigned int voice,
                                            uint64_t segment_signature,
                                            uint32_t live_cbo)
{
    if (voice >= MCPX_HW_MAX_VOICES ||
        !xemu_audio_packs_should_prepare_stream_voice()) {
        return false;
    }

    bool dump_stream = xemu_audio_packs_should_dump_streams();
    bool match_stream = g_config.audio.replace_enabled &&
                        (qatomic_read(&g_stream_match_count) != 0 ||
                         qatomic_read(&g_consumed_window_unique_count) != 0);

    g_mutex_lock(&g_stream_capture_lock);
    AudioStreamCapture *sc = &g_stream_captures[voice];
    bool new_activation = !sc->have_segment ||
                          sc->last_segment_signature != segment_signature ||
                          live_cbo < sc->last_cbo;
    bool need_match = match_stream && !sc->replacement_identified &&
                      !sc->match_exhausted;
    bool needed = new_activation && (dump_stream || need_match);
    if (!needed && sc->have_segment) {
        sc->last_cbo = live_cbo;
    }
    g_mutex_unlock(&g_stream_capture_lock);
    return needed;
}

bool xemu_audio_packs_stream_append_segment(unsigned int voice,
                                            const int16_t *pcm,
                                            uint32_t frames,
                                            unsigned int channels,
                                            uint32_t sample_rate,
                                            uint64_t segment_signature,
                                            uint32_t live_cbo,
                                            const char *source_format)
{
    if (voice >= MCPX_HW_MAX_VOICES || !pcm || frames == 0 ||
        channels < 1 || channels > 2 || sample_rate == 0 ||
        !xemu_audio_packs_should_prepare_stream_voice()) {
        return false;
    }

    AudioVoiceState *voice_state = &g_voices[voice];
    if (!voice_state->replacement_active) {
        voice_state->prepared = true;
        voice_state->streaming = true;
        voice_state->source_channels = channels;
        voice_state->observed_source_rate = sample_rate;
        g_strlcpy(voice_state->observed_source_format,
                  source_format ? source_format : "",
                  sizeof(voice_state->observed_source_format));
        voice_state->guest_cbo_event_seen =
            qatomic_read(&g_guest_cbo_event_seq[voice]);
    }

    bool dump_stream = xemu_audio_packs_should_dump_streams();
    bool match_stream = g_config.audio.replace_enabled &&
                        qatomic_read(&g_stream_match_count) != 0;
    char *dump_dir = NULL;
    if (dump_stream) {
        g_mutex_lock(&g_index_lock);
        if (g_paths_valid && g_dump_dir) {
            dump_dir = g_strdup(g_dump_dir);
        }
        g_mutex_unlock(&g_index_lock);
        if (!dump_dir) {
            dump_stream = false;
        }
    }

    uint64_t fingerprints[AUDIO_STREAM_FINGERPRINT_TIER_COUNT] = { 0 };
    uint32_t fingerprints_ready = 0;
    uint64_t source_frame_offset = 0;
    bool reset_voice_replacement = false;

    g_mutex_lock(&g_stream_capture_lock);
    AudioStreamCapture *sc = &g_stream_captures[voice];
    sc->idle_pending = false;
    sc->idle_passes = 0;

    /*
     * Channel/source-format changes cannot be one logical source.  Pitch/rate
     * changes remain playback modulation and intentionally do not split a
     * stream.
     */
    bool format_changed = sc->have_segment &&
        (sc->channels != 0 && sc->channels != channels);
    if (!format_changed && sc->have_segment && sc->source_format[0]) {
        format_changed =
            g_strcmp0(sc->source_format, source_format ? source_format : "") != 0;
    }
    if (format_changed) {
        reset_stream_capture_locked(sc, false);
        reset_voice_replacement = true;
    }

    bool same_segment = sc->have_segment &&
                        sc->last_segment_signature == segment_signature;
    bool wrapped = same_segment && live_cbo < sc->last_cbo;
    if (same_segment && !wrapped) {
        sc->last_cbo = live_cbo;
        g_mutex_unlock(&g_stream_capture_lock);
        g_free(dump_dir);
        if (reset_voice_replacement) {
            AudioVoiceState *vs = &g_voices[voice];
            if (vs->streaming) {
                uint32_t seen = qatomic_read(&g_guest_cbo_event_seq[voice]);
                memset(vs, 0, sizeof(*vs));
                vs->guest_cbo_event_seen = seen;
                reset_consumed_voice_state(voice);
            }
        }
        return false;
    }

    source_frame_offset = sc->source_frames_seen;
    if (UINT64_MAX - sc->source_frames_seen < frames) {
        sc->source_frames_seen = UINT64_MAX;
    } else {
        sc->source_frames_seen += frames;
    }
    sc->last_segment_signature = segment_signature;
    sc->last_cbo = live_cbo;
    sc->have_segment = true;
    if (!sc->channels) {
        sc->channels = channels;
    }
    if (!sc->sample_rate) {
        sc->sample_rate = sample_rate;
    }
    if (!sc->source_format[0]) {
        g_strlcpy(sc->source_format,
                  source_format ? source_format : "stream_unknown",
                  sizeof(sc->source_format));
    }

    if (dump_stream) {
        if (!sc->active) {
            uint64_t session = ++g_stream_session_sequence;
            if (session == 0) {
                session = ++g_stream_session_sequence;
            }
            sc->active = true;
            sc->session = session;
            queue_stream_job(AUDIO_DUMP_JOB_STREAM_BEGIN, sc->session,
                             NULL, 0, channels, sample_rate, sc->source_format,
                             dump_dir, 0, false);
        }
        queue_stream_job(AUDIO_DUMP_JOB_STREAM_APPEND, sc->session,
                         pcm, frames, channels, sample_rate, sc->source_format,
                         NULL, segment_signature, wrapped);
    }

    if (match_stream && !sc->replacement_identified && !sc->match_exhausted) {
        stream_match_feed_locked(sc, pcm, frames, channels);
        uint32_t ready = sc->match_fingerprint_mask & ~sc->match_tried_mask;
        for (guint i = 0; i < AUDIO_STREAM_FINGERPRINT_TIER_COUNT; i++) {
            uint32_t bit = 1u << i;
            if (!(ready & bit)) {
                continue;
            }
            fingerprints[i] = sc->match_fingerprints[i];
            fingerprints_ready |= bit;
            sc->match_tried_mask |= bit;
        }
        if (sc->match_hash_frames >= AUDIO_STREAM_MATCH_MAX_FRAMES &&
            sc->match_tried_mask ==
                ((1u << AUDIO_STREAM_FINGERPRINT_TIER_COUNT) - 1u)) {
            sc->match_exhausted = true;
        }
    }
    g_mutex_unlock(&g_stream_capture_lock);
    g_free(dump_dir);

    if (reset_voice_replacement) {
        AudioVoiceState *vs = &g_voices[voice];
        if (vs->streaming) {
            uint32_t seen = qatomic_read(&g_guest_cbo_event_seq[voice]);
            memset(vs, 0, sizeof(*vs));
            vs->guest_cbo_event_seen = seen;
            reset_consumed_voice_state(voice);
        }
    }

    uint64_t matched_hash = 0;
    if (fingerprints_ready) {
        /* Longest available prefix wins. Short ambiguous/silent prefixes are
         * therefore naturally deferred until more decoded source is known. */
        for (gint i = AUDIO_STREAM_FINGERPRINT_TIER_COUNT - 1; i >= 0; i--) {
            if (!(fingerprints_ready & (1u << i))) {
                continue;
            }
            if (lookup_stream_source_hash(fingerprints[i], &matched_hash)) {
                break;
            }
        }
    }

    uint64_t playback_frame_offset = source_frame_offset;
    uint64_t cbo_offset = MIN((uint64_t)live_cbo, (uint64_t)frames);
    if (UINT64_MAX - playback_frame_offset < cbo_offset) {
        playback_frame_offset = UINT64_MAX;
    } else {
        playback_frame_offset += cbo_offset;
    }

    if (matched_hash &&
        prepare_stream_replacement(voice, matched_hash, sample_rate, channels,
                                   playback_frame_offset, source_format)) {
        g_mutex_lock(&g_stream_capture_lock);
        AudioStreamCapture *current = &g_stream_captures[voice];
        current->replacement_identified = true;
        current->matched_source_hash = matched_hash;
        current->match_exhausted = true;
        /* Once a unique prefix proves this is an already-replaced source, the
         * user's skip-replaced policy can stop the background stream dump
         * immediately instead of writing the remainder only to discard it at
         * publish time. */
        if (g_config.audio.dump_skip_replaced) {
            finish_stream_dump_locked(current, true);
        }
        g_mutex_unlock(&g_stream_capture_lock);
    }
    return dump_stream || fingerprints_ready != 0;
}

void xemu_audio_packs_stream_voice_idle(unsigned int voice)
{
    if (voice >= MCPX_HW_MAX_VOICES) {
        return;
    }

    bool expire = false;
    g_mutex_lock(&g_stream_capture_lock);
    AudioStreamCapture *sc = &g_stream_captures[voice];
    if (sc->have_segment || sc->active || sc->replacement_identified) {
        sc->idle_pending = true;
        if (sc->idle_passes < UINT32_MAX) {
            sc->idle_passes++;
        }
        expire = sc->idle_passes >= AUDIO_STREAM_IDLE_GRACE_PASSES;
        if (expire) {
            reset_stream_capture_locked(sc, false);
        }
    }
    g_mutex_unlock(&g_stream_capture_lock);

    /*
     * Persist streams commonly expose a briefly empty A/B SSL list while the
     * guest refills it.  Do not split the capture or restart a replacement on
     * that ordinary producer gap.  Only a sustained idle period (or the normal
     * voice-reset hook) closes the logical stream.
     */
    if (expire) {
        AudioVoiceState *vs = &g_voices[voice];
        if (vs->streaming) {
            uint32_t seen = qatomic_read(&g_guest_cbo_event_seq[voice]);
            memset(vs, 0, sizeof(*vs));
            vs->guest_cbo_event_seen = seen;
            reset_consumed_voice_state(voice);
        }
    }
}

bool xemu_audio_packs_stream_override_samples(unsigned int voice,
                                              float samples[][2],
                                              int count)
{
    if (voice >= MCPX_HW_MAX_VOICES || !samples || count <= 0) {
        return false;
    }

    AudioVoiceState *vs = &g_voices[voice];
    if (!vs->streaming || !vs->replacement_active || !vs->asset ||
        !vs->replacement) {
        return false;
    }

    AudioReplacement *r = vs->replacement;
    int produced = 0;
    while (produced < count && vs->cursor < r->frames) {
        uint64_t available = (uint64_t)r->frames - vs->cursor;
        int take = MIN((uint64_t)(count - produced), available);
        if (r->channels == 2) {
            memcpy(&samples[produced][0], &r->samples[vs->cursor * 2],
                   (size_t)take * 2 * sizeof(float));
        } else {
            for (int i = 0; i < take; i++) {
                float mono = r->samples[vs->cursor + i];
                samples[produced + i][0] = mono;
                samples[produced + i][1] = mono;
            }
        }
        vs->cursor += take;
        produced += take;
    }

    /*
     * The guest stream remains authoritative for lifetime/SSL notifications.
     * If a custom WAV is shorter, never leak the original source back through:
     * emit silence until the guest stream ends. A longer replacement is
     * naturally clipped when the guest stream finishes.
     */
    if (produced < count) {
        memset(&samples[produced][0], 0,
               (size_t)(count - produced) * 2 * sizeof(float));
        vs->finished = true;
    }
    if (UINT64_MAX - vs->stream_source_cursor < (uint64_t)count) {
        vs->stream_source_cursor = UINT64_MAX;
    } else {
        vs->stream_source_cursor += (uint64_t)count;
    }
    return true;
}

static GPtrArray *build_asset_variants(uint64_t hash)
{
    char key[AUDIO_HASH_KEY_LEN];
    hash_key(hash, key);
    GPtrArray *variants = NULL;

    /* The replacement index already owns stable path strings for the current
     * generation. Build the per-asset variant objects directly from it instead
     * of first allocating a second temporary array of duplicated paths and then
     * duplicating those strings again. */
    g_mutex_lock(&g_index_lock);
    if (g_paths_valid && g_replace_index) {
        AudioReplacementIndexGroup *group =
            g_hash_table_lookup(g_replace_index, key);
        if (group) {
            const guint count = (group->variants && group->variants->len > 0)
                ? group->variants->len : (group->single_path ? 1u : 0u);
            if (count > 0) {
                variants = g_ptr_array_new_full(count, free_replacement_variant);
                if (group->variants && group->variants->len > 0) {
                    for (guint i = 0; i < group->variants->len; ++i) {
                        AudioReplacementIndexVariant *src =
                            g_ptr_array_index(group->variants, i);
                        AudioReplacementVariant *dst =
                            g_new0(AudioReplacementVariant, 1);
                        dst->path = g_strdup(src->path);
                        g_ptr_array_add(variants, dst);
                    }
                } else {
                    AudioReplacementVariant *dst =
                        g_new0(AudioReplacementVariant, 1);
                    dst->path = g_strdup(group->single_path);
                    g_ptr_array_add(variants, dst);
                }
            }
        }
    }
    g_mutex_unlock(&g_index_lock);
    return variants;
}

static char *lookup_replacement_policy_path(uint64_t hash)
{
    char key[AUDIO_HASH_KEY_LEN];
    hash_key(hash, key);
    char *sample_path = NULL;

    /* Copy one representative WAV path while holding the immutable-index lock;
     * filesystem I/O happens after unlocking.  A base <hash>.json applies to
     * the whole numbered variant pool. */
    g_mutex_lock(&g_index_lock);
    if (g_paths_valid && g_replace_index) {
        AudioReplacementIndexGroup *group =
            g_hash_table_lookup(g_replace_index, key);
        if (group) {
            if (group->variants && group->variants->len > 0) {
                AudioReplacementIndexVariant *v =
                    g_ptr_array_index(group->variants, 0);
                sample_path = g_strdup(v->path);
            } else if (group->single_path) {
                sample_path = g_strdup(group->single_path);
            }
        }
    }
    g_mutex_unlock(&g_index_lock);

    if (!sample_path) {
        return NULL;
    }
    char *dir = g_path_get_dirname(sample_path);
    char *json_path = g_strdup_printf("%s%c%s.json", dir,
                                      G_DIR_SEPARATOR, key);
    g_free(dir);
    g_free(sample_path);
    return json_path;
}

static char *lookup_replacement_root_policy_path(uint64_t hash)
{
    char key[AUDIO_HASH_KEY_LEN];
    hash_key(hash, key);
    char *path = NULL;

    g_mutex_lock(&g_index_lock);
    if (g_paths_valid && g_replace_dir) {
        path = g_strdup_printf("%s%c%s.json", g_replace_dir,
                               G_DIR_SEPARATOR, key);
    }
    g_mutex_unlock(&g_index_lock);
    return path;
}

static char *lookup_dump_policy_path(uint64_t hash)
{
    char key[AUDIO_HASH_KEY_LEN];
    hash_key(hash, key);
    char *path = NULL;

    g_mutex_lock(&g_index_lock);
    if (g_paths_valid && g_dump_dir) {
        path = g_strdup_printf("%s%c%s.json", g_dump_dir,
                               G_DIR_SEPARATOR, key);
    }
    g_mutex_unlock(&g_index_lock);
    return path;
}

static bool lookup_restart_on_retrigger(uint64_t hash)
{
    bool restart = false;

    /* Pack-local metadata wins, so a replacement pack remains self-contained
     * when copied to another machine.  The dump-side metadata is a convenient
     * fallback for local editing. */
    char *path = lookup_replacement_policy_path(hash);
    if (path) {
        bool found = read_retrigger_mode_file(path, &restart);
        g_free(path);
        if (found) {
            return restart;
        }
    }

    path = lookup_replacement_root_policy_path(hash);
    if (path) {
        bool found = read_retrigger_mode_file(path, &restart);
        g_free(path);
        if (found) {
            return restart;
        }
    }

    path = lookup_dump_policy_path(hash);
    if (path) {
        bool found = read_retrigger_mode_file(path, &restart);
        g_free(path);
        if (found) {
            return restart;
        }
    }
    return false;
}

static void ensure_asset_retrigger_policy(AudioAsset *asset, uint64_t hash)
{
    g_mutex_lock(&g_asset_lock);
    if (asset->retrigger_policy_checked) {
        g_mutex_unlock(&g_asset_lock);
        return;
    }
    g_mutex_unlock(&g_asset_lock);

    /* The sidecar is read only once per decoded asset generation, never in the
     * per-sample path.  Reload Audio Replacements bumps the generation and
     * safely invalidates assets at the next VP frame boundary. */
    bool restart = lookup_restart_on_retrigger(hash);

    g_mutex_lock(&g_asset_lock);
    if (!asset->retrigger_policy_checked) {
        asset->restart_on_retrigger = restart;
        asset->retrigger_policy_checked = true;
    }
    g_mutex_unlock(&g_asset_lock);
}

static AudioReplacement *load_replacement(const char *path,
                                          unsigned int channels,
                                          uint32_t target_rate)
{
    /* Replacement WAVs are predecoded before index publication. Voice workers
     * only clone/channel-adapt resident float samples here. */
    return materialize_preloaded_replacement(path, channels, target_rate);
}

static AudioAsset *find_or_create_asset(uint64_t hash, uint32_t source_rate,
                                        uint32_t frames, bool loop,
                                        uint32_t loop_start,
                                        const char *source_format)
{
    /* Most voice starts hit an already-known asset. Check the cache before
     * consulting replacement/dump metadata so repeated sound effects do one
     * hash-table lookup instead of path formatting, index locking and possible
     * sidecar I/O on every trigger. */
    g_mutex_lock(&g_asset_lock);
    if (!g_asset_cache) {
        g_asset_cache = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                              g_free, free_asset);
    }
    AudioAsset *a = g_hash_table_lookup(g_asset_cache, &hash);
    if (a) {
        g_mutex_unlock(&g_asset_lock);
        return a;
    }
    g_mutex_unlock(&g_asset_lock);

    /* Only a true cache miss needs the stable pitch reference. This may read a
     * sidecar and intentionally stays outside g_asset_lock. */
    uint32_t reference_rate = lookup_reference_rate(hash);

    /* Another voice worker may have created the same asset while metadata was
     * being resolved. Recheck under the cache lock before publishing ours. */
    g_mutex_lock(&g_asset_lock);
    a = g_hash_table_lookup(g_asset_cache, &hash);
    if (!a) {
        guint64 *key = g_new(guint64, 1);
        *key = hash;
        a = g_new0(AudioAsset, 1);
        a->hash = hash;
        a->canonical_rate = MAX(1u, reference_rate ? reference_rate : source_rate);
        if (reference_rate) {
            fprintf(stderr,
                    "mcpx: audio-io: %016" PRIx64
                    " pitch reference %u Hz (metadata; observed %u Hz)\n",
                    hash, a->canonical_rate, source_rate);
        }
        a->source_frames = frames;
        a->loop = loop;
        a->loop_start = MIN(loop_start, frames ? frames - 1 : 0);
        g_strlcpy(a->source_format,
                  source_format ? source_format : "unknown",
                  sizeof(a->source_format));
        g_hash_table_insert(g_asset_cache, key, a);
    }
    g_mutex_unlock(&g_asset_lock);
    return a;
}

static void ensure_asset_replacement_index(AudioAsset *asset, uint64_t hash)
{
    g_mutex_lock(&g_asset_lock);
    if (asset->replacements_checked) {
        g_mutex_unlock(&g_asset_lock);
        return;
    }
    g_mutex_unlock(&g_asset_lock);

    GPtrArray *variants = build_asset_variants(hash);

    /* Two workers may reach first use together.  Building the tiny path list
     * twice is cheaper than blocking audio on file I/O; only one list wins. */
    g_mutex_lock(&g_asset_lock);
    if (!asset->replacements_checked) {
        asset->replacements_checked = true;
        asset->replacements = variants;
        variants = NULL;
    }
    g_mutex_unlock(&g_asset_lock);
    if (variants) {
        g_ptr_array_free(variants, TRUE);
    }
}

static guint choose_replacement_variant(uint64_t hash, guint count)
{
    if (count <= 1) {
        return 0;
    }

    /* SplitMix64-style mixing gives a cheap, well-distributed choice without
     * a shared RNG lock.  Selection happens once per hardware voice start,
     * never per sample or per APU frame. */
    uint32_t seq = qatomic_fetch_inc(&g_variant_sequence) + 1;
    uint64_t x = hash ^ ((uint64_t)seq * UINT64_C(0x9e3779b97f4a7c15));
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return (guint)(x % count);
}

static AudioReplacement *load_asset_variant(AudioAsset *asset, guint index,
                                            unsigned int channels,
                                            bool source_rate)
{
    char *path = NULL;
    AudioReplacement *ready = NULL;

    g_mutex_lock(&g_asset_lock);
    if (!asset->replacements || index >= asset->replacements->len) {
        g_mutex_unlock(&g_asset_lock);
        return NULL;
    }
    AudioReplacementVariant *variant =
        g_ptr_array_index(asset->replacements, index);

    /* All file/decode work is already complete. If another voice worker is
     * materializing this same resident WAV, wait only for its memory copy so a
     * concurrent first trigger cannot incorrectly see the replacement absent. */
    bool *load_in_progress = source_rate
        ? &variant->source_rate_load_in_progress : &variant->load_in_progress;
    bool *load_failed = source_rate
        ? &variant->source_rate_load_failed : &variant->load_failed;
    AudioReplacement **slot = source_rate
        ? &variant->source_rate_audio : &variant->audio;
    while (*load_in_progress && !*slot) {
        g_cond_wait(&g_asset_load_cond, &g_asset_lock);
    }
    if (*slot) {
        ready = *slot;
    } else if (!*load_failed) {
        *load_in_progress = true;
        path = g_strdup(variant->path);
    }
    g_mutex_unlock(&g_asset_lock);

    if (ready || !path) {
        return ready;
    }

    AudioReplacement *loaded = load_replacement(
        path, channels, source_rate ? asset->canonical_rate : 0);
    g_free(path);

    g_mutex_lock(&g_asset_lock);
    variant = g_ptr_array_index(asset->replacements, index);
    load_in_progress = source_rate
        ? &variant->source_rate_load_in_progress : &variant->load_in_progress;
    load_failed = source_rate
        ? &variant->source_rate_load_failed : &variant->load_failed;
    slot = source_rate ? &variant->source_rate_audio : &variant->audio;
    if (loaded && !*slot) {
        *slot = loaded;
        loaded = NULL;
        fprintf(stderr,
                "mcpx: audio-io: replacement %016" PRIx64
                " variant %u/%u (%u frames @ %u Hz -> %u frames @ %u Hz%s)\n",
                asset->hash, index + 1, asset->replacements->len,
                asset->source_frames, asset->canonical_rate,
                (*slot)->frames, (*slot)->sample_rate,
                source_rate ? ", source-rate passthrough" : "");
    } else if (!loaded && !*slot) {
        *load_failed = true;
    }
    *load_in_progress = false;
    ready = *slot;
    g_cond_broadcast(&g_asset_load_cond);
    g_mutex_unlock(&g_asset_lock);
    free_replacement(loaded);
    return ready;
}

static AudioReplacement *select_asset_replacement_mode(AudioAsset *asset,
                                                        uint64_t hash,
                                                        unsigned int channels,
                                                        bool source_rate)
{
    ensure_asset_replacement_index(asset, hash);

    g_mutex_lock(&g_asset_lock);
    guint count = asset->replacements ? asset->replacements->len : 0;
    g_mutex_unlock(&g_asset_lock);
    if (count == 0) {
        return NULL;
    }

    guint start = choose_replacement_variant(hash, count);
    for (guint attempt = 0; attempt < count; attempt++) {
        guint index = (start + attempt) % count;
        AudioReplacement *r = load_asset_variant(asset, index, channels,
                                                  source_rate);
        if (r) {
            return r;
        }
    }
    return NULL;
}

static AudioReplacement *select_asset_replacement(AudioAsset *asset,
                                                   uint64_t hash,
                                                   unsigned int channels)
{
    return select_asset_replacement_mode(asset, hash, channels, false);
}

void xemu_audio_packs_init(void)
{
    for (unsigned int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        if (g_stream_captures[v].match_hash_state) {
            XXH3_freeState(g_stream_captures[v].match_hash_state);
        }
    }
    memset(g_stream_captures, 0, sizeof(g_stream_captures));
    unsigned int hash_state_failures = 0;
    for (unsigned int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        g_stream_captures[v].match_hash_state = XXH3_createState();
        if (g_stream_captures[v].match_hash_state) {
            XXH3_64bits_reset(g_stream_captures[v].match_hash_state);
        } else {
            hash_state_failures++;
        }
    }
    if (hash_state_failures) {
        fprintf(stderr,
                "mcpx: audio-io: warning: %u stream matcher hash state(s) "
                "could not be allocated\n",
                hash_state_failures);
    }
    clear_apu_cache();
    g_apu_generation = current_generation();
}

void xemu_audio_packs_finalize(void)
{
    /* Finish active streamed voices before stopping the asynchronous writer. */
    reset_all_stream_captures(false);
    for (unsigned int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        if (g_stream_captures[v].match_hash_state) {
            XXH3_freeState(g_stream_captures[v].match_hash_state);
            g_stream_captures[v].match_hash_state = NULL;
        }
    }
    clear_apu_cache();
    if (g_asset_cache) {
        g_hash_table_destroy(g_asset_cache);
        g_asset_cache = NULL;
    }

    g_mutex_lock(&g_dump_worker_lock);
    GThread *dump_thread = g_dump_thread;
    GAsyncQueue *dump_queue = g_dump_queue;
    if (dump_thread) {
        g_dump_worker_stopping = true;
        g_async_queue_push(dump_queue, AUDIO_DUMP_SENTINEL);
    }
    g_mutex_unlock(&g_dump_worker_lock);

    if (dump_thread) {
        g_thread_join(dump_thread);
    }

    g_mutex_lock(&g_dump_worker_lock);
    g_dump_thread = NULL;
    if (g_dump_queue) {
        g_async_queue_unref(g_dump_queue);
        g_dump_queue = NULL;
    }
    g_dump_worker_stopping = false;
    g_mutex_unlock(&g_dump_worker_lock);
}

void xemu_audio_packs_reset(void)
{
    reset_all_stream_captures(false);
    clear_apu_cache();
    g_apu_generation = current_generation();
}

void xemu_audio_packs_voice_reset(unsigned int voice)
{
    if (voice < MCPX_HW_MAX_VOICES) {
        g_mutex_lock(&g_stream_capture_lock);
        reset_stream_capture_locked(&g_stream_captures[voice], false);
        g_mutex_unlock(&g_stream_capture_lock);
        uint32_t seen = qatomic_read(&g_guest_cbo_event_seq[voice]);
        memset(&g_voices[voice], 0, sizeof(g_voices[voice]));
        g_voices[voice].guest_cbo_event_seen = seen;
        reset_consumed_voice_state(voice);
    }
}

void xemu_audio_packs_note_guest_cbo_write(unsigned int voice, uint32_t guest_cbo)
{
    if (voice >= MCPX_HW_MAX_VOICES) {
        return;
    }
    /* Publish the value first, then the sequence.  Sequentially-consistent
     * qatomic operations make the worker-side pair stable without taking the
     * asset mutex from the guest FE/MMIO path. */
    qatomic_set(&g_guest_cbo_event_value[voice], guest_cbo);
    qatomic_inc(&g_guest_cbo_event_seq[voice]);
}

void xemu_audio_packs_voice_mark_unsupported(unsigned int voice)
{
    if (voice < MCPX_HW_MAX_VOICES) {
        g_voices[voice].prepared = true;
        g_voices[voice].replacement_active = false;
    }
}

bool xemu_audio_packs_prepare_static_voice(unsigned int voice,
                                        const int16_t *pcm,
                                        uint32_t frames,
                                        unsigned int channels,
                                        uint32_t observed_source_rate,
                                        bool loop,
                                        uint32_t loop_start,
                                        const char *source_format)
{
    if (voice >= MCPX_HW_MAX_VOICES || !pcm || frames == 0 ||
        channels < 1 || channels > 2 ||
        (!g_config.audio.dump_enabled && !g_config.audio.replace_enabled)) {
        return false;
    }

    AudioVoiceState *vs = &g_voices[voice];
    uint32_t current_cbo_seq = qatomic_read(&g_guest_cbo_event_seq[voice]);
    bool retry_reused_buffer = vs->prepared && !vs->replacement_active &&
                               qatomic_read(&g_static_match_count) != 0 &&
                               current_cbo_seq != vs->guest_cbo_event_seen;
    if (vs->prepared && !retry_reused_buffer) {
        return vs->replacement_active;
    }
    /* A first activation or explicit reuse/seek establishes a new consumed
     * timeline. Never let rolling landmarks straddle unrelated ring epochs. */
    reset_consumed_voice_state(voice);

    size_t bytes = (size_t)frames * channels * sizeof(int16_t);
    uint64_t hash = fast_hash((const uint8_t *)pcm, bytes);
    /* Keep mono/stereo identities distinct without making pitch part of ID. */
    uint8_t channel_tag = channels;
    hash ^= fast_hash(&channel_tag, sizeof(channel_tag));

    AudioAsset *asset = find_or_create_asset(hash, observed_source_rate,
                                             frames, loop, loop_start,
                                             source_format);
    if (g_config.audio.dump_enabled && qatomic_read(&g_dump_static_enabled)) {
        queue_dump(hash, pcm, frames, channels, asset->canonical_rate,
                   loop, loop_start, source_format);
    }

    vs->prepared = true;
    vs->asset = asset;
    vs->cursor = 0;
    vs->guest_cbo = 0;
    vs->finished = false;
    vs->observed_source_rate = observed_source_rate;
    vs->source_channels = channels;
    g_strlcpy(vs->observed_source_format,
              source_format ? source_format : "",
              sizeof(vs->observed_source_format));
    /* CBO writes that happened as part of initial voice setup are baseline,
     * not retriggers.  Only later guest writes are actionable. */
    vs->guest_cbo_event_seen = current_cbo_seq;

    AudioReplacement *replacement = NULL;
    AudioAsset *playback_asset = asset;
    uint64_t playback_hash = hash;
    bool prefix_passthrough = false;
    if (g_config.audio.replace_enabled) {
        replacement = select_asset_replacement(asset, hash, channels);

        /* Some games (notably late Xbox titles using software-fed DirectSound
         * rings) reuse a large resident backing buffer. Unrelated tail/ring
         * bytes can change between activations, so the full-buffer hash differs
         * even though the actually-starting sound is byte-identical. Fall back
         * to the longest unique source prefix from known replacement sources. */
        if (!replacement && qatomic_read(&g_static_match_count) != 0) {
            uint64_t matched_hash = 0;
            if (match_static_source_prefix(pcm, frames, channels,
                                           &matched_hash) &&
                matched_hash != hash) {
                uint32_t matched_frames = lookup_source_frames(matched_hash);
                playback_asset = find_or_create_asset(
                    matched_hash, observed_source_rate,
                    matched_frames ? matched_frames : frames, false, 0,
                    source_format);
                replacement = select_asset_replacement_mode(
                    playback_asset, matched_hash, channels, true);
                if (replacement &&
                    replacement->sample_rate == playback_asset->canonical_rate) {
                    playback_hash = matched_hash;
                    prefix_passthrough = true;
                    fprintf(stderr,
                            "mcpx: audio-io: resident/static prefix matched "
                            "%016" PRIx64 " -> %016" PRIx64
                            " (full backing-buffer identity changed)\n",
                            hash, matched_hash);
                } else {
                    replacement = NULL;
                    playback_asset = asset;
                }
            }
        }
    }

    if (replacement && playback_asset != asset) {
        vs->asset = playback_asset;
    }

    /* A randomized choice is made once when this hardware voice starts.  A
     * looping voice keeps that same choice for every loop until the guest
     * stops/reuses the voice. */
    vs->replacement = replacement;
    vs->replacement_active = replacement != NULL;
    vs->passthrough_source = prefix_passthrough;
    if (prefix_passthrough) {
        vs->consumed_source_hash = playback_hash;
        vs->consumed_match_offset = 0;
        vs->consumed_last_seen_offset = 0;
    }
    if (vs->replacement_active) {
        ensure_asset_retrigger_policy(playback_asset, playback_hash);

        uint64_t mapped_loop = (uint64_t)playback_asset->loop_start *
                               replacement->sample_rate /
                               MAX(1u, playback_asset->canonical_rate);
        vs->replacement_loop_start =
            mapped_loop < replacement->frames ? (uint32_t)mapped_loop : 0;
        vs->replacement_rate_scale = prefix_passthrough ? 1.0f :
            (replacement->sample_rate > 0
                ? (float)playback_asset->canonical_rate /
                  (float)replacement->sample_rate
                : 1.0f);

        /*
         * Hardware retrigger/seek handling is always automatic through guest
         * CBO events.  The optional restart policy adds one extra behavior:
         * a new playback of the same source hash bumps a shared epoch so older
         * instances on other hardware voices self-terminate.  This avoids
         * cross-worker AudioVoiceState writes.
         */
        if (playback_asset->restart_on_retrigger) {
            vs->retrigger_epoch = qatomic_fetch_inc(&playback_asset->retrigger_epoch) + 1;
        } else {
            vs->retrigger_epoch = qatomic_read(&playback_asset->retrigger_epoch);
        }
    }
    return vs->replacement_active;
}

bool xemu_audio_packs_voice_prepared(unsigned int voice)
{
    return voice < MCPX_HW_MAX_VOICES && g_voices[voice].prepared;
}

bool xemu_audio_packs_static_voice_retry_needed(unsigned int voice)
{
    if (voice >= MCPX_HW_MAX_VOICES ||
        qatomic_read(&g_static_match_count) == 0) {
        return false;
    }
    AudioVoiceState *vs = &g_voices[voice];
    if (!vs->prepared || vs->replacement_active || vs->streaming) {
        return false;
    }
    /* Software-fed resident buffers are often reused without another VOICE_ON.
     * A later guest SetCurrentPosition/CBO update is a strong, cheap signal that
     * the source backing store may now contain a different logical sound. Retry
     * identity exactly once per guest event; the prepare path consumes the seq. */
    return qatomic_read(&g_guest_cbo_event_seq[voice]) !=
           vs->guest_cbo_event_seen;
}

bool xemu_audio_packs_voice_has_replacement(unsigned int voice)
{
    /* Passthrough replacement must never short-circuit voice_get_samples():
     * the guest's real source reader/CBO/SSL machinery remains authoritative
     * and substitution happens only after those samples were consumed. */
    return g_config.audio.replace_enabled &&
           voice < MCPX_HW_MAX_VOICES &&
           g_voices[voice].replacement_active &&
           !g_voices[voice].passthrough_source;
}

bool xemu_audio_packs_voice_apply_guest_retrigger(unsigned int voice,
                                                uint32_t live_guest_cbo)
{
    if (voice >= MCPX_HW_MAX_VOICES) {
        return false;
    }

    AudioVoiceState *vs = &g_voices[voice];
    if (vs->streaming) {
        return false;
    }
    if (!vs->replacement_active || !vs->asset || !vs->replacement ||
        vs->finished) {
        /* Consume stale setup events so they cannot become a seek/retrigger
         * later if replacement state changes while this voice stays allocated. */
        vs->guest_cbo_event_seen = qatomic_read(&g_guest_cbo_event_seq[voice]);
        return false;
    }

    /* The sequence is the cheap steady-state discriminator. Most service
     * passes see no FE CBO write, so avoid two extra atomic reads unless the
     * sequence actually changed. Direct guest rewinds remain detectable even
     * without an explicit event. */
    uint32_t seq_before = qatomic_read(&g_guest_cbo_event_seq[voice]);
    bool explicit_guest_update = seq_before != vs->guest_cbo_event_seen;
    bool direct_guest_rewind =
        !explicit_guest_update && live_guest_cbo < vs->guest_cbo &&
        vs->guest_cbo != 0;
    if (!explicit_guest_update && !direct_guest_rewind) {
        return false;
    }

    uint32_t event_cbo = 0;
    if (explicit_guest_update) {
        event_cbo = qatomic_read(&g_guest_cbo_event_value[voice]);
        uint32_t seq_after = qatomic_read(&g_guest_cbo_event_seq[voice]);
        if (seq_after != seq_before) {
            /* An FE write raced this read; use the newest stable pair next frame. */
            return false;
        }
        vs->guest_cbo_event_seen = seq_after;
    }

    /*
     * Automatic hardware-driven behavior:
     *
     * SET_VOICE_BUF_CBO is the guest's SetCurrentPosition operation.  Mirror it
     * for every replacement, not only assets with a JSON policy.  A write to
     * zero is the common rapid-fire retrigger pattern and gets a fresh random
     * variant.  Other writes are treated as seeks within the current variant.
     * Initial setup writes are already excluded by guest_cbo_event_seen when a
     * voice is prepared/reset.
     */
    uint32_t target_cbo = explicit_guest_update ? event_cbo : live_guest_cbo;
    bool restart_from_beginning = target_cbo == 0;

    AudioReplacement *replacement = vs->replacement;
    if (restart_from_beginning && !vs->asset->loop) {
        /* A discrete sound retrigger gets a new randomized variant.  A true
         * hardware-looping voice keeps its chosen variant stable across a
         * guest rewind so ambience/music does not randomly change each loop. */
        AudioReplacement *fresh = select_asset_replacement(
            vs->asset, vs->asset->hash, vs->replacement->channels);
        if (fresh) {
            replacement = fresh;
        }
    }

    vs->replacement = replacement;
    vs->replacement_active = true;
    vs->finished = false;

    uint64_t mapped_cursor = (uint64_t)target_cbo * replacement->sample_rate /
                             MAX(1u, vs->asset->canonical_rate);
    /* Allow cursor==frames: the next sample request will complete the voice in
     * the normal path.  Never let a malformed guest seek overflow the buffer. */
    vs->cursor = MIN(mapped_cursor, (uint64_t)replacement->frames);
    vs->guest_cbo = target_cbo;

    uint64_t mapped_loop = (uint64_t)vs->asset->loop_start *
                           replacement->sample_rate /
                           MAX(1u, vs->asset->canonical_rate);
    vs->replacement_loop_start =
        mapped_loop < replacement->frames ? (uint32_t)mapped_loop : 0;
    vs->replacement_rate_scale =
        replacement->sample_rate > 0
            ? (float)vs->asset->canonical_rate / (float)replacement->sample_rate
            : 1.0f;

    /* "restart" is now only an optional stronger policy: when requested, a
     * restart at the beginning also invalidates older instances of the same
     * source hash on other hardware voices.  Automatic same-voice guest CBO
     * handling above does not need the sidecar. */
    if (restart_from_beginning && vs->asset->restart_on_retrigger) {
        vs->retrigger_epoch =
            qatomic_fetch_inc(&vs->asset->retrigger_epoch) + 1;
    }
    return true;
}

int xemu_audio_packs_voice_get_samples(unsigned int voice, float samples[][2],
                                    int requested)
{
    if (voice >= MCPX_HW_MAX_VOICES || requested <= 0) {
        return -1;
    }
    AudioVoiceState *vs = &g_voices[voice];
    if (vs->streaming || !vs->replacement_active || !vs->asset ||
        !vs->replacement) {
        return -1;
    }

    AudioReplacement *r = vs->replacement;

    /* Optional forced-monophonic policy.  Ordinary automatic retriggers/seeks
     * are handled from the guest's hardware CBO writes and do not need this. */
    if (vs->asset->restart_on_retrigger &&
        vs->retrigger_epoch != qatomic_read(&vs->asset->retrigger_epoch)) {
        vs->finished = true;
        return 0;
    }

    uint64_t logical_end = r->frames;
    if (logical_end == 0) {
        vs->finished = true;
        return 0;
    }

    int produced = 0;
    while (produced < requested) {
        if (vs->cursor >= logical_end) {
            if (vs->asset->loop) {
                /* Loop within the selected replacement.  In restart mode a
                 * newer trigger still takes precedence over this loop. */
                vs->cursor = MIN((uint64_t)vs->replacement_loop_start,
                                 logical_end - 1);
            } else {
                vs->finished = true;
                break;
            }
        }

        uint64_t available = logical_end - vs->cursor;
        int take = MIN((uint64_t)(requested - produced), available);
        if (r->channels == 2) {
            /* float[2] output and interleaved stereo replacement storage are
             * layout-identical, so copy the contiguous run in one operation. */
            memcpy(&samples[produced][0], &r->samples[vs->cursor * 2],
                   (size_t)take * 2 * sizeof(float));
        } else {
            for (int i = 0; i < take; i++) {
                float mono = r->samples[vs->cursor + i];
                samples[produced + i][0] = mono;
                samples[produced + i][1] = mono;
            }
        }
        vs->cursor += take;
        produced += take;
    }

    if (!vs->asset->loop && vs->cursor >= logical_end) {
        vs->finished = true;
    }

    if (vs->asset->canonical_rate > 0 && r->sample_rate > 0) {
        uint64_t mapped = vs->cursor * vs->asset->canonical_rate /
                          r->sample_rate;
        if (vs->asset->source_frames > 0) {
            mapped = MIN(mapped, (uint64_t)vs->asset->source_frames - 1);
        }
        vs->guest_cbo = (uint32_t)mapped;
    }
    return produced;
}

float xemu_audio_packs_voice_rate_scale(unsigned int voice)
{
    /*
     * The native VP still owns every live Xbox pitch change.  This factor only
     * converts the replacement WAV's encoded sample rate into the source
     * asset's stable reference domain.  Because the reference no longer
     * changes with the first playback of a process, later pitch bends (including
     * slow-motion/bullet-time changes) remain relative to the same anchor.
     */
    if (!g_config.audio.replace_enabled || voice >= MCPX_HW_MAX_VOICES) {
        return 1.0f;
    }
    AudioVoiceState *vs = &g_voices[voice];
    if (!vs->replacement_active || !vs->asset || !vs->replacement ||
        vs->asset->canonical_rate == 0 ||
        vs->replacement->sample_rate == 0) {
        return 1.0f;
    }
    return vs->replacement_rate_scale;
}

uint32_t xemu_audio_packs_voice_guest_cbo(unsigned int voice)
{
    return voice < MCPX_HW_MAX_VOICES ? g_voices[voice].guest_cbo : 0;
}

bool xemu_audio_packs_voice_finished(unsigned int voice)
{
    return voice < MCPX_HW_MAX_VOICES &&
           !g_voices[voice].streaming && g_voices[voice].finished;
}
