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

#define AUDIO_HASH_KEY_LEN 17
#define AUDIO_DUMP_SENTINEL ((gpointer)(uintptr_t)1)

bool xemu_audio_packs_enabled(void)
{
    return g_config.audio.dump_enabled || g_config.audio.replace_enabled;
}

bool xemu_audio_packs_should_prepare_voice(void)
{
    if (g_config.audio.dump_enabled) {
        return true;
    }
    return g_config.audio.replace_enabled &&
           xemu_audio_packs_replacements_available();
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
} AudioReplacementVariant;

typedef struct AudioReplacementIndexVariant {
    uint32_t number;
    char *path;
} AudioReplacementIndexVariant;

typedef struct AudioReplacementIndexGroup {
    char *single_path;           /* <hash>.wav */
    GPtrArray *variants;         /* AudioReplacementIndexVariant* */
} AudioReplacementIndexGroup;

typedef struct AudioAsset {
    uint64_t hash;
    uint32_t canonical_rate;     /* First observed playback rate for this PCM. */
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
    AudioAsset *asset;
    AudioReplacement *replacement;
    uint64_t cursor;
    uint32_t guest_cbo;
    uint32_t replacement_loop_start;
    uint32_t retrigger_epoch;
    uint32_t guest_cbo_event_seen;
} AudioVoiceState;

typedef struct AudioDumpJob {
    char *wav_path;
    char *json_path;
    int16_t *pcm;
    uint32_t frames;
    uint32_t sample_rate;
    unsigned int channels;
    uint64_t hash;
    bool loop;
    uint32_t loop_start;
    char source_format[24];
} AudioDumpJob;

/* UI thread swaps immutable path/index objects under this short mutex. */
static GMutex g_index_lock;
static char *g_dump_dir;
static char *g_replace_dir;
static GHashTable *g_replace_index; /* 16-hex stem -> AudioReplacementIndexGroup* */
/* Absolute WAV path -> AudioPreloadedWav. Built outside voice workers and
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
    if (!g_config.audio.dump_enabled && !g_config.audio.replace_enabled) {
        return;
    }

    uint32_t generation = current_generation();
    if (generation == g_apu_generation) {
        return;
    }
    clear_apu_cache();
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

static GHashTable *build_replacement_index(const char *dir)
{
    GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, free_index_group);
    scan_replacement_wavs_recursive(dir, table);
    return table;
}

static AudioPreloadedWav *preload_wav_file(const char *path)
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
        .freq = src_spec.freq,
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
    w->sample_rate = src_spec.freq;
    w->channels = warm_channels;
    return w;
}

typedef struct AudioPreloadJob {
    char *path;
    AudioPreloadedWav *wav;
} AudioPreloadJob;

static void free_preload_job(gpointer p)
{
    AudioPreloadJob *job = p;
    if (!job) {
        return;
    }
    g_free(job->path);
    free_preloaded_wav(job->wav);
    g_free(job);
}

static void preload_job_worker(gpointer data, gpointer user_data)
{
    (void)user_data;
    AudioPreloadJob *job = data;
    job->wav = preload_wav_file(job->path);
}

static void add_preload_job(GPtrArray *jobs, GHashTable *seen,
                            const char *path)
{
    if (!path || !path[0] || g_hash_table_contains(seen, path)) {
        return;
    }
    g_hash_table_add(seen, g_strdup(path));
    AudioPreloadJob *job = g_new0(AudioPreloadJob, 1);
    job->path = g_strdup(path);
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
        /* Match runtime selection semantics exactly: numbered variants take
         * precedence over an unsuffixed singleton when both are present. */
        if (group->variants && group->variants->len > 0) {
            for (guint i = 0; i < group->variants->len; i++) {
                AudioReplacementIndexVariant *v =
                    g_ptr_array_index(group->variants, i);
                add_preload_job(jobs, seen, v->path);
            }
        } else {
            add_preload_job(jobs, seen, group->single_path);
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
        g_hash_table_insert(cache, g_strdup(job->path), job->wav);
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
    const char *path, unsigned int channels)
{
    if (!path || channels < 1 || channels > 2) {
        return NULL;
    }

    g_mutex_lock(&g_index_lock);
    AudioPreloadedWav *w = g_preload_cache ?
        g_hash_table_lookup(g_preload_cache, path) : NULL;
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
                                   GHashTable *preloads)
{
    GHashTable *old_replacements;
    GHashTable *old_preloads;

    g_mutex_lock(&g_index_lock);
    old_replacements = g_replace_index;
    old_preloads = g_preload_cache;
    g_replace_index = replacements;
    g_preload_cache = preloads;
    qatomic_set(&g_replacement_count, replacements ?
                (uint32_t)g_hash_table_size(replacements) : 0);
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

    GHashTable *new_replacements = build_replacement_index(new_replace);
    GHashTable *new_preloads = build_preload_cache(new_replacements);
    GHashTable *new_dumps = build_dump_set(new_dump);

    char *old_dump;
    char *old_replace;
    GHashTable *old_replacements;
    GHashTable *old_preloads;
    GHashTable *old_dumps;
    g_mutex_lock(&g_index_lock);
    old_dump = g_dump_dir;
    old_replace = g_replace_dir;
    old_replacements = g_replace_index;
    old_preloads = g_preload_cache;
    old_dumps = g_dumped_set;
    g_dump_dir = g_strdup(new_dump);
    g_replace_dir = g_strdup(new_replace);
    g_replace_index = new_replacements;
    g_preload_cache = new_preloads;
    g_dumped_set = new_dumps;
    qatomic_set(&g_replacement_count, new_replacements ?
                (uint32_t)g_hash_table_size(new_replacements) : 0);
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
    g_mutex_lock(&g_index_lock);
    if (g_paths_valid) {
        replace_dir = g_strdup(g_replace_dir);
    }
    g_mutex_unlock(&g_index_lock);
    if (!replace_dir) {
        return;
    }

    GHashTable *replacements = build_replacement_index(replace_dir);
    GHashTable *preloads = build_preload_cache(replacements);
    g_free(replace_dir);
    swap_replacement_index(replacements, preloads);
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

static bool write_pcm16_wav(const AudioDumpJob *job)
{
    if (job->channels < 1 || job->channels > 2 || job->frames == 0) {
        return false;
    }
    uint64_t data64 = (uint64_t)job->frames * job->channels * sizeof(int16_t);
    if (data64 > UINT32_MAX - 36) {
        return false;
    }
    uint32_t data_bytes = (uint32_t)data64;
    FILE *f = g_fopen(job->wav_path, "wb");
    if (!f) {
        return false;
    }

    bool ok = fwrite("RIFF", 1, 4, f) == 4 &&
              write_u32(f, 36 + data_bytes) &&
              fwrite("WAVEfmt ", 1, 8, f) == 8 &&
              write_u32(f, 16) && write_u16(f, 1) &&
              write_u16(f, job->channels) &&
              write_u32(f, job->sample_rate) &&
              write_u32(f, job->sample_rate * job->channels * 2) &&
              write_u16(f, job->channels * 2) && write_u16(f, 16) &&
              fwrite("data", 1, 4, f) == 4 && write_u32(f, data_bytes) &&
              fwrite(job->pcm, 1, data_bytes, f) == data_bytes;
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

    g_autofree char *json = g_strdup_printf(
        "{\n"
        "  \"hash\": \"%016" PRIx64 "\",\n"
        "  \"source_format\": \"%s\",\n"
        "  \"channels\": %u,\n"
        "  \"sample_rate\": %u,\n"
        "  \"frames\": %u,\n"
        "  \"duration_seconds\": %.9f,\n"
        "  \"looping\": %s,\n"
        "  \"loop_start_frame\": %u,\n"
        "  \"retrigger_mode\": \"%s\"\n"
        "}\n",
        job->hash, job->source_format, job->channels, job->sample_rate,
        job->frames, (double)job->frames / MAX(1u, job->sample_rate),
        job->loop ? "true" : "false", job->loop_start,
        restart ? "restart" : "auto");
    g_file_set_contents(job->json_path, json, -1, NULL);
}

static gpointer dump_worker(gpointer unused)
{
    for (;;) {
        gpointer p = g_async_queue_pop(g_dump_queue);
        if (p == AUDIO_DUMP_SENTINEL) {
            break;
        }
        AudioDumpJob *job = p;
        if (write_pcm16_wav(job)) {
            write_metadata_json(job);
        } else {
            fprintf(stderr, "mcpx: audio-io: failed to dump %s\n",
                    job->wav_path);
        }
        g_free(job->wav_path);
        g_free(job->json_path);
        g_free(job->pcm);
        g_free(job);
    }
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
    job->pcm = copy;
    job->frames = frames;
    job->sample_rate = sample_rate;
    job->channels = channels;
    job->hash = hash;
    job->loop = loop;
    job->loop_start = MIN(loop_start, frames - 1);
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

static GPtrArray *lookup_replacement_paths(uint64_t hash)
{
    char key[AUDIO_HASH_KEY_LEN];
    hash_key(hash, key);
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);

    g_mutex_lock(&g_index_lock);
    if (g_paths_valid && g_replace_index) {
        AudioReplacementIndexGroup *group =
            g_hash_table_lookup(g_replace_index, key);
        if (group) {
            /* Numbered variants deliberately take precedence.  This makes it
             * safe to leave an old <hash>.wav in place while introducing
             * <hash>_1.wav, <hash>_2.wav, ... as a random pool. */
            if (group->variants && group->variants->len > 0) {
                for (guint i = 0; i < group->variants->len; i++) {
                    AudioReplacementIndexVariant *v =
                        g_ptr_array_index(group->variants, i);
                    g_ptr_array_add(paths, g_strdup(v->path));
                }
            } else if (group->single_path) {
                g_ptr_array_add(paths, g_strdup(group->single_path));
            }
        }
    }
    g_mutex_unlock(&g_index_lock);

    if (paths->len == 0) {
        g_ptr_array_free(paths, TRUE);
        return NULL;
    }
    return paths;
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
                                          unsigned int channels)
{
    /* Replacement WAVs are predecoded before index publication. Voice workers
     * only clone/channel-adapt resident float samples here. */
    return materialize_preloaded_replacement(path, channels);
}

static AudioAsset *find_or_create_asset(uint64_t hash, uint32_t source_rate,
                                        uint32_t frames, bool loop,
                                        uint32_t loop_start,
                                        const char *source_format)
{
    g_mutex_lock(&g_asset_lock);

    /* Lifecycle init creates this before workers run; keep a defensive path. */
    if (!g_asset_cache) {
        g_asset_cache = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                              g_free, free_asset);
    }
    AudioAsset *a = g_hash_table_lookup(g_asset_cache, &hash);
    if (!a) {
        guint64 *key = g_new(guint64, 1);
        *key = hash;
        a = g_new0(AudioAsset, 1);
        a->hash = hash;
        a->canonical_rate = MAX(1u, source_rate);
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

    GPtrArray *paths = lookup_replacement_paths(hash);
    GPtrArray *variants = NULL;
    if (paths) {
        variants = g_ptr_array_new_with_free_func(free_replacement_variant);
        for (guint i = 0; i < paths->len; i++) {
            AudioReplacementVariant *v = g_new0(AudioReplacementVariant, 1);
            v->path = g_strdup(g_ptr_array_index(paths, i));
            g_ptr_array_add(variants, v);
        }
        g_ptr_array_free(paths, TRUE);
    }

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
                                            unsigned int channels)
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
    while (variant->load_in_progress && !variant->audio) {
        g_cond_wait(&g_asset_load_cond, &g_asset_lock);
    }
    if (variant->audio) {
        ready = variant->audio;
    } else if (!variant->load_failed) {
        variant->load_in_progress = true;
        path = g_strdup(variant->path);
    }
    g_mutex_unlock(&g_asset_lock);

    if (ready || !path) {
        return ready;
    }

    AudioReplacement *loaded = load_replacement(path, channels);
    g_free(path);

    g_mutex_lock(&g_asset_lock);
    variant = g_ptr_array_index(asset->replacements, index);
    if (loaded && !variant->audio) {
        variant->audio = loaded;
        loaded = NULL;
        fprintf(stderr,
                "mcpx: audio-io: replacement %016" PRIx64
                " variant %u/%u (%u frames @ %u Hz -> %u frames @ %u Hz)\n",
                asset->hash, index + 1, asset->replacements->len,
                asset->source_frames, asset->canonical_rate,
                variant->audio->frames, variant->audio->sample_rate);
    } else if (!loaded && !variant->audio) {
        variant->load_failed = true;
    }
    variant->load_in_progress = false;
    ready = variant->audio;
    g_cond_broadcast(&g_asset_load_cond);
    g_mutex_unlock(&g_asset_lock);
    free_replacement(loaded);
    return ready;
}

static AudioReplacement *select_asset_replacement(AudioAsset *asset,
                                                   uint64_t hash,
                                                   unsigned int channels)
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
        AudioReplacement *r = load_asset_variant(asset, index, channels);
        if (r) {
            return r;
        }
    }
    return NULL;
}

void xemu_audio_packs_init(void)
{
    clear_apu_cache();
    g_apu_generation = current_generation();
}

void xemu_audio_packs_finalize(void)
{
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
    clear_apu_cache();
    g_apu_generation = current_generation();
}

void xemu_audio_packs_voice_reset(unsigned int voice)
{
    if (voice < MCPX_HW_MAX_VOICES) {
        uint32_t seen = qatomic_read(&g_guest_cbo_event_seq[voice]);
        memset(&g_voices[voice], 0, sizeof(g_voices[voice]));
        g_voices[voice].guest_cbo_event_seen = seen;
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
    if (vs->prepared) {
        return vs->replacement_active;
    }

    size_t bytes = (size_t)frames * channels * sizeof(int16_t);
    uint64_t hash = fast_hash((const uint8_t *)pcm, bytes);
    /* Keep mono/stereo identities distinct without making pitch part of ID. */
    uint8_t channel_tag = channels;
    hash ^= fast_hash(&channel_tag, sizeof(channel_tag));

    AudioAsset *asset = find_or_create_asset(hash, observed_source_rate,
                                             frames, loop, loop_start,
                                             source_format);
    queue_dump(hash, pcm, frames, channels, asset->canonical_rate,
               loop, loop_start, source_format);

    vs->prepared = true;
    vs->asset = asset;
    vs->cursor = 0;
    vs->guest_cbo = 0;
    vs->finished = false;
    /* CBO writes that happened as part of initial voice setup are baseline,
     * not retriggers.  Only later guest writes are actionable. */
    vs->guest_cbo_event_seen = qatomic_read(&g_guest_cbo_event_seq[voice]);

    AudioReplacement *replacement = NULL;
    if (g_config.audio.replace_enabled) {
        replacement = select_asset_replacement(asset, hash, channels);
    }

    /* A randomized choice is made once when this hardware voice starts.  A
     * looping voice keeps that same choice for every loop until the guest
     * stops/reuses the voice. */
    vs->replacement = replacement;
    vs->replacement_active = replacement != NULL;
    if (vs->replacement_active) {
        ensure_asset_retrigger_policy(asset, hash);

        uint64_t mapped_loop = (uint64_t)asset->loop_start *
                               replacement->sample_rate /
                               MAX(1u, asset->canonical_rate);
        vs->replacement_loop_start =
            mapped_loop < replacement->frames ? (uint32_t)mapped_loop : 0;

        /*
         * Hardware retrigger/seek handling is always automatic through guest
         * CBO events.  The optional restart policy adds one extra behavior:
         * a new playback of the same source hash bumps a shared epoch so older
         * instances on other hardware voices self-terminate.  This avoids
         * cross-worker AudioVoiceState writes.
         */
        if (asset->restart_on_retrigger) {
            vs->retrigger_epoch = qatomic_fetch_inc(&asset->retrigger_epoch) + 1;
        } else {
            vs->retrigger_epoch = qatomic_read(&asset->retrigger_epoch);
        }
    }
    return vs->replacement_active;
}

bool xemu_audio_packs_voice_prepared(unsigned int voice)
{
    return voice < MCPX_HW_MAX_VOICES && g_voices[voice].prepared;
}

bool xemu_audio_packs_voice_has_replacement(unsigned int voice)
{
    return voice < MCPX_HW_MAX_VOICES && g_voices[voice].replacement_active;
}

bool xemu_audio_packs_voice_apply_guest_retrigger(unsigned int voice,
                                                uint32_t live_guest_cbo)
{
    if (voice >= MCPX_HW_MAX_VOICES) {
        return false;
    }

    AudioVoiceState *vs = &g_voices[voice];
    if (!vs->replacement_active || !vs->asset || !vs->replacement ||
        vs->finished) {
        /* Consume stale setup events so they cannot become a seek/retrigger
         * later if replacement state changes while this voice stays allocated. */
        vs->guest_cbo_event_seen = qatomic_read(&g_guest_cbo_event_seq[voice]);
        return false;
    }

    uint32_t seq_before = qatomic_read(&g_guest_cbo_event_seq[voice]);
    uint32_t event_cbo = qatomic_read(&g_guest_cbo_event_value[voice]);
    uint32_t seq_after = qatomic_read(&g_guest_cbo_event_seq[voice]);
    if (seq_after != seq_before) {
        /* An FE write raced this read; use the newest stable pair next frame. */
        return false;
    }

    bool explicit_guest_update = seq_after != vs->guest_cbo_event_seen;
    bool direct_guest_rewind =
        !explicit_guest_update && live_guest_cbo < vs->guest_cbo &&
        vs->guest_cbo != 0;

    vs->guest_cbo_event_seen = seq_after;
    if (!explicit_guest_update && !direct_guest_rewind) {
        return false;
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
    if (!vs->replacement_active || !vs->asset || !vs->replacement) {
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
        for (int i = 0; i < take; i++) {
            uint64_t frame = vs->cursor + i;
            samples[produced + i][0] = r->samples[frame * r->channels];
            if (r->channels > 1) {
                samples[produced + i][1] = r->samples[frame * r->channels + 1];
            } else {
                samples[produced + i][1] = samples[produced + i][0];
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
    if (voice >= MCPX_HW_MAX_VOICES) {
        return 1.0f;
    }
    AudioVoiceState *vs = &g_voices[voice];
    if (!vs->replacement_active || !vs->asset || !vs->replacement ||
        vs->asset->canonical_rate == 0 ||
        vs->replacement->sample_rate == 0) {
        return 1.0f;
    }
    return (float)vs->asset->canonical_rate /
           (float)vs->replacement->sample_rate;
}

uint32_t xemu_audio_packs_voice_guest_cbo(unsigned int voice)
{
    return voice < MCPX_HW_MAX_VOICES ? g_voices[voice].guest_cbo : 0;
}

bool xemu_audio_packs_voice_finished(unsigned int voice)
{
    return voice < MCPX_HW_MAX_VOICES && g_voices[voice].finished;
}
