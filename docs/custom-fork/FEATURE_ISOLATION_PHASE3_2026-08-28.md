# Feature Isolation Phase 3 — renderer sidecars

## Intent

Phase 3 removes custom texture-pack state from existing NV2A renderer data
structures. This is a compatibility/upstreamability refactor: feature policy
may be arbitrarily rich, but the emulated hardware renderer should not carry
that policy in its normal objects.

## Core-object restoration

The OpenGL `TextureBinding` now contains only the upstream renderer fields:
reference/draw/hash/sampler addressing/filter state plus GL target/texture.
The Vulkan `TextureBinding` now contains only its upstream LRU/key/image/view/
allocation/sampler/dirty/hash/timestamp state.

Custom state moved to feature-owned sidecars keyed by the core binding pointer.
Sidecars are not allocated for ordinary bindings.

## OpenGL bridge

Core calls a small set of lifecycle and operation hooks:

- binding created/destroyed;
- sampler override query;
- replacement upload attempt;
- dump observation;
- dynamic-sidecar refresh;
- renderer cache synchronization.

Animation clocks, frame selection, procedural shader source compilation, GL
program/FBO/uniform state, hot reload and replacement policy are implemented
in the feature adapter.

## Vulkan bridge

Before image creation, the renderer asks the feature for a compact creation
plan containing only whether a replacement/shader is active and the dimensions
and mip count required to allocate the Vulkan image. After allocation, the
feature attaches its sidecar. On destruction, the renderer notifies the feature
before destroying image/view resources so dependent shader resources can be
released safely.

Replacement upload, animation refresh, procedural shader resources and dump
policy are feature-owned.

## Why a plan is still a core hook

A replacement can change the dimensions/format/usage flags of the host image,
so Vulkan must know those allocation facts before `vmaCreateImage`. Storing the
feature's long-lived state inside `TextureBinding` is unnecessary; a short-lived
creation plan is sufficient and keeps ownership outside the renderer.

## Next isolation phase

The next step is physical feature-root consolidation plus independent Meson
build gates/stubs so each custom feature can be omitted from the binary. That
will make compatibility builds able to contain exactly one selected feature or
no custom features at all.
