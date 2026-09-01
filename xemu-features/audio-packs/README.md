# Audio Packs

## Purpose

Source-level MCPX/APU audio dumping and WAV replacement before guest voice processing. Dumping and replacement cover resident/static DirectSound voices, software-fed/reused or circular resident buffers, and packetized streaming SSL voices (commonly music, dialogue, ambience, and other long-form audio). Normal dumping is loop-aware: proven repeated traversals are compacted to one canonical loop while unique intros and outros are retained, with loop points written both to JSON and to a standard RIFF `smpl` chunk. Exact whole-buffer and stream-prefix matching remain fast paths; a transport-agnostic consumed-source-window matcher additionally identifies source audio from the decoded PCM actually passing through MCPX, so replacement does not depend on a logical sound beginning at buffer/packet frame 0. Replacement preserves guest-controlled pitch, volume, envelopes and filters, native CBO/SSL/ring consumption and notifier timing, and preloaded randomized replacement variants. Replacement pitch anchoring prefers the source dump/pack JSON `sample_rate`, so a title entering slow motion or another pitch-modified state does not redefine the asset's normal reference rate on the next run.

## Build gate

- Meson: `xemu_feature_audio_packs`
- Config macro: `CONFIG_XEMU_FEATURE_AUDIO_PACKS`
- Default in this custom fork: ON

## Public API

`audio-packs.h` owns lifecycle, path/index, replacement/dump and per-voice state APIs. `audio-packs-apu.h` is the narrow APU preparation bridge. `frontend.hh` owns Settings UI.

## Files owned

- `audio-packs-apu.c`
- `audio-packs-apu.h`
- `audio-packs.c`
- `audio-packs.h`
- `frontend.cc`
- `frontend.hh`
- `STREAM_REPLACEMENT_NATIVE_BRIDGE.patch`
- `apply-stream-replacement-bridge.sh`

## Exact Xemu hook sites

- `hw/xbox/mcpx/apu/apu.c` — init/reset/finalize lifecycle.
- `hw/xbox/mcpx/apu/vp/vp.c` — voice reset/reuse, guest CBO writes, replacement sample/rate/end queries, frame-boundary synchronization, plus the minimal source-fetch observation/substitution bridge used by packetized stream replacement.
- `ui/xui/main-menu.cc` — one settings-panel hook.
- `hw/xbox/mcpx/apu/meson.build` and `ui/xui/meson.build` — conditional source inclusion.

## Dependencies

Uses existing MCPX/APU data structures, SDL WAV/audio conversion, GLib containers/threads and Xemu settings. It has no required dependency on the other optional features.

## Dump categories

The Audio settings expose independent source-dump toggles beneath the master `Dump source audio` switch:

- **Static / buffered voices** — resident DirectSound buffers, typically sound effects. Hardware-looped buffers use the Xbox LBO/EBO source semantics directly, so one source traversal is dumped with the exact loop start/end retained.
- **Streaming SSL voices** — successive MCPX SSL segments are decoded with the same source rules as the VP and stitched into one logical WAV, commonly covering music, dialogue, ambience, and other long-form streams. Exact repeated decoded cycles are compacted to one loop traversal while any unique prefix (intro) and suffix (outro) remain in the file. The same path supports packetized U8/S16/S24/S32 PCM and Xbox ADPCM streams.

Multipass/mixbin voices are internal processing passes rather than source assets and are intentionally excluded; they contain already-mixed intermediate audio rather than an independently identifiable source asset.

## Threading model

Replacement WAV preload uses a GLib thread pool (up to four workers). Dumping uses one background `xemu.audio-dump` worker. Static sources are queued as complete PCM jobs. Streaming SSL segments are copied from the source-fetch feature hook and appended by the same background worker to a temporary WAV. After three complete bit-exact stream cycles are proven, later complete repetitions are suppressed online so an indefinitely looping ambience cannot grow the temporary file forever; the writer still watches for non-loop data and resumes retention immediately so a real outro is not lost. On voice completion/reuse/reset, the worker performs conservative exact cycle compaction, writes loop metadata, content-hashes the **logical published PCM**, and publishes the file under the normal `<hash>.wav` naming convention. Stream replacement WAVs are also resampled during preload to the source stream's canonical JSON `sample_rate`; runtime substitution can therefore remain one source frame for one guest SSL frame and never changes CBO/SSL completion/notifier timing. APU voice workers never perform filesystem I/O or replacement-WAV conversion.

## Logical sound extraction and archival output

The normal dump path is intended to be usable both for replacement packs and for faithful source-audio extraction (for example, preparing assets for an archival/resource site). There is no separate lossy cleanup pass. Audio Packs only removes repetition when the source relationship is proven exactly.

- **Static hardware loops:** Xbox loop state supplies the authoritative loop start (LBO) and source end (EBO). The WAV contains the source once; the prefix before LBO is the intro and the LBO..EBO region is the loop.
- **Streamed loops:** the background writer looks for complete decoded packet/segment cycles that recur bit-for-bit. A candidate hash is only a lookup accelerator; the complete PCM cycle is byte-compared before it is accepted. A bounded PCM-window fallback can refine/prove an internal loop when packet boundaries do not line up, but it will not collapse an unbounded periodic waveform whose authored loop length cannot be proven.
- **Online suppression:** three exact consecutive cycles are required before subsequent repetitions are discarded during capture. If the stream leaves the loop part-way through another traversal, the already-proven repeated prefix stays discarded and retention resumes at the first genuinely new segment, so loop material cannot be mislabeled as part of the outro. If capture simply stops mid-repeat, the canonical dump still ends after the one complete loop traversal.
- **Ambiguous/repeated musical content:** is kept. If exact loop structure cannot be established safely, the WAV is published without trimming instead of guessing.

A loop-aware dump contains one canonical representation: `[intro][one loop traversal][outro]`. Looping WAVs include a standard RIFF `smpl` chunk with a forward loop (`start = loop_start_frame`, inclusive `end = loop_end_frame - 1`) so compatible audio editors can see the loop directly. The JSON sidecar records `loop_start_frame`, `loop_end_frame`, `intro_frames`, `loop_frames`, `outro_frames`, `observed_frames_before_loop_compaction`, `loop_repetitions_observed`, `loop_detection`, and `wav_loop_metadata`.

The filename hash for a newly compacted stream is calculated from the **published logical PCM**, not the discarded duplicate traversals. This makes the dump itself the stable source template for replacements and avoids making pack identity depend on how long the user happened to leave a loop playing. Existing older dump/replacement hashes remain usable; this only changes the identity of newly published streams whose repeated loop traversals are actually compacted.

### Texture-pack-like replacement workflow

1. Enable **Dump source audio** and play the sound once (or let a loop repeat long enough to prove itself).
2. The dump directory receives `<hash>.wav` and `<hash>.json`. For loops, the WAV/JSON already contain the canonical intro/loop/outro structure and loop points.
3. Copy that pair into the title's replacement directory. Keep the original JSON; edit/replace the WAV while retaining the `<hash>` stem. Optional randomized variants remain `<hash>_1.wav`, `<hash>_2.wav`, and so on.
4. Use **Reload audio replacements**. Exact static/stream identities remain fast paths and the consumed-source-window matcher covers reused/ring/arbitrary-offset transports.

This is deliberately analogous to a texture replacement pack: the emulator supplies a deterministic source identity and metadata, the modder replaces the corresponding asset, and guest timing/voice semantics remain owned by the emulator.

## Hot-path behavior

Disabled public hooks are inline false/no-op. When built but inactive, dump/replacement gates early-out. Voice workers do not open files or decode WAVs. For streaming voices, the bridge checks the current SSL descriptor/CBO signature and only decodes a segment when a new activation/refill is observed; repeated service calls for the same active segment stay cheap. Observation also occurs at the actual source-fetch boundary, so a short segment that advances inside one libsamplerate callback cannot be skipped between the normal 1500-Hz voice-service passes. The consumed matcher maintains one 16-frame rolling window per active hardware voice. Only roughly 1/16 windows become content-defined candidates, a 1-Mbit bloom filter rejects almost all non-landmarks before any lock, and an exact XXH3 verification hash is required before a source is accepted. All source-landmark tables and replacement WAV materializations are built off the APU worker before publication.

## Build-disabled behavior

No audio-pack implementation objects are linked. MCPX voice hooks compile to neutral defaults; ordinary Xbox audio processing is unchanged.

## Porting only this feature

Copy `xemu-features/audio-packs/`, add the Meson option/config-host flag, conditionally compile its frontend/APU sources, and apply only the integration sites above. `apply-stream-replacement-bridge.sh` applies the small `vp.c` bridge idempotently from the repository root; all matching, fingerprinting, replacement state, dumping, and policy logic remains feature-owned under `xemu-features/`.

The neutral public-header contract should be retained when porting so unrelated core code does not need `#ifdef` forests.


### Pitch-reference behavior

Replacement hashes remain pitch-independent. For a replacement `<hash>.wav`, audio-packs now looks for `<hash>.json` beside the replacement, at the replacement root, and finally in the local source dump directory. When that metadata contains `sample_rate`, it is used as the stable source-rate reference. The Xbox VP continues to apply the live pitch register and pitch-envelope modulation every frame, so runtime changes such as slow motion/bullet time remain audible instead of being baked out of the replacement.

Streaming dumps also keep one capture open when only the hardware pitch changes. The first segment's sample rate remains the WAV reference; subsequent pitch changes are treated as playback modulation rather than a new stream format.


## Resident / software-fed buffer replacement

Late Xbox titles can reuse a large DirectSound backing buffer for many logical sounds. In that pattern, bytes outside the audible sound (unused tail data, ring contents, or another producer-owned region) can change between activations. The traditional exact static identity hashes the complete decoded buffer, so those unrelated changes can make the same audible sound appear to be a different asset.

Audio Packs now keeps exact whole-buffer matching as the zero-ambiguity fast path, then falls back to tiered source-prefix identity for unmatched resident/static voices. Prefixes use the same 16-through-65536-frame tiers as stream matching. Short shared/silent prefixes are marked ambiguous and ignored; the longest available unique prefix wins. This makes replacement robust to backing-buffer reuse without guessing between two sources that genuinely begin alike.

New static dumps write portable `source_fingerprint_*` fields into `<hash>.json`. Existing dumps do not need to be recreated locally: `Reload audio replacements` derives missing source fingerprints from the original dumped `<hash>.wav` on the non-APU index-build path. The APU worker never opens or scans source files.

A successful fallback is reported as:

`mcpx: audio-io: resident/static prefix matched <current-full-hash> -> <replacement-source-hash> (full backing-buffer identity changed)`

This path is intended for engines that behave like static DirectSound at the MCPX register level while using those buffers as software-managed audio pools/rings.

## Transport-agnostic consumed-source-window matching

Some Xbox engines do not expose a logical sound as either one immutable DirectSound buffer or one packetized SSL stream. They keep a persistent hardware voice alive and continuously rewrite a circular/ring source region, so a voice line can begin at an arbitrary backing-buffer offset and the game may not issue `VOICE_OFF` or `SetCurrentPosition` between successive logical sounds. Whole-buffer and frame-0 prefix identities cannot solve that transport pattern.

When replacement sources are reloaded, Audio Packs scans each original dumped `<hash>.wav` that has a replacement and builds sparse **content-defined landmarks throughout the decoded source**, not only at frame 0. Runtime matching happens immediately after Xemu's native source fetch, before pitch/envelope/filter/DSP processing. Each hardware voice carries a 16-frame rolling PCM16 window. A window is considered only when its content-defined anchor passes the sparse landmark mask and bloom filter; the final match additionally requires an exact portable XXH3 fingerprint and must map uniquely to one source hash and source offset. Repeated/silent windows are therefore rejected rather than guessed.

This makes identity independent of transport and alignment: the same matcher can recognize a replacement source inside a static resident buffer, an SSL packet sequence, or an arbitrary point in a reused circular buffer. Native Xbox source consumption remains authoritative underneath the substitution, so guest CBO progression, SSL transitions, ring wrap, pitch modulation and notifier timing are not taken over by Audio Packs.

While a passthrough replacement is playing, Audio Packs **continues observing the untouched native source blocks**. A different source landmark can therefore switch to the next replacement even if the game reuses the same hardware voice before the previous custom WAV ends. Replays/loops of the same source are detected when unique source landmark offsets wrap backwards; monotonically advancing landmarks from the tail of the current source do not spuriously restart it. This removes the need for a guest `VOICE_OFF`/CBO boundary between logical sounds.

Alignment is transport-aware. Resident/circular-buffer consumed matches always start a clean replacement at frame 0 when the first verified landmark is recognized; the source offset identifies where the sound was found inside the backing ring, not where a custom clip should begin. For true SSL/stream matches, a replacement that remains roughly the same duration as the dumped source (75%-125%) is treated as a layout-compatible remaster and begins at the verified source offset. A substantially different-length streamed replacement begins at frame 0 instead. Only the short landmark-detection window can precede takeover.

Consumed/passthrough replacements never take over hardware looping. The native source reader continues to loop or refill the real backing store, and repeated logical occurrences are identified from the consumed PCM. This prevents a hardware-looped software ring from turning one replaced voice line into an infinite custom-WAV loop. If a custom WAV ends before another source is identified, Audio Packs outputs silence rather than leaking the original source, while continuing to search the native consumed PCM for the next logical sound.

The arbitrary-offset index currently uses the original source dump WAVs in the configured dump directory when **Reload audio replacements** runs. No source WAV is opened or scanned on an APU worker. A successful match is logged as:

`mcpx: audio-io: consumed-source window matched <hash> at source frame <offset> (voice <n>, resident/ring|SSL/stream, block cursor <n>)`

A transition on an already-active passthrough voice is logged as `consumed-source window switched to ...`.

Older local dumps with incomplete JSON metadata can still participate when the original source WAV is present: reload derives missing source sample rate, frame count, and channel count from that WAV before building source-rate passthrough materializations. Xbox ADPCM consumed matching also reconstructs the decoder's exact signed-16 samples (rather than applying the ordinary PCM re-quantization), preserving compatibility with existing ADPCM dump identities without requiring a redump.

## Stream replacement identity

A complete streamed source keeps the normal `<hash>.wav` identity: the hash is computed from the fully stitched decoded PCM, just like the stream dumper. That complete hash is only knowable when the stream ends, so real-time replacement uses deterministic **early source fingerprints** stored in the dump JSON. Fingerprints are tiered at 16 through 65536 decoded source frames (about 1.37 seconds at 48 kHz). Ambiguous short prefixes (for example, shared silence) are marked ambiguous and ignored until a longer unique tier identifies the source.

New stream dumps write these fingerprint fields automatically. Existing pre-fingerprint dumps remain usable: when the source `<hash>.wav` is still present in the dump directory, `Reload audio replacements` derives the tiers from that original dump once on the non-APU preload path. For a portable/shared pack, copy the original source `<hash>.json` beside the replacement WAV (or to the replacement root). A modified replacement WAV cannot reconstruct the original source fingerprint by itself; the JSON is the portable identity metadata.

Once a stream is identified, replacement starts at the corresponding current source-frame/CBO position rather than restarting the custom WAV at frame zero if matching happened after playback had already advanced. A short custom WAV yields silence after its end rather than leaking the original source back in; a longer custom WAV is clipped when the guest ends the source stream, because the guest remains authoritative for stream lifetime and notifications.

## Packetized-stream coverage

The stream observer follows the hardware SSL list/segment state at every actual source fetch, not only the outer voice pass. It therefore covers A/B list transitions, multiple segment advances inside one resampler callback, descriptors refilled in place, persistent streams with brief producer underflow, and live pitch modulation without splitting one logical source. The decoded formats mirror the existing VP path: mono/stereo PCM container formats and Xbox ADPCM.
