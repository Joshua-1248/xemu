#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Deterministic guardrails for the adaptive set-associative CHD hunk cache.

This tests replacement/locality behavior rather than codec performance.  It is
kept independent of libchdr so it can run in the normal static suite.
"""
from __future__ import annotations

CACHE_TARGET = 16 * 1024 * 1024
WAYS = 4
INVALID = None


def geometry(hunkbytes: int, total_hunks: int) -> tuple[int, int, int]:
    wanted = max(1, CACHE_TARGET // hunkbytes)
    wanted = min(wanted, total_hunks)
    ways = min(wanted, WAYS)
    sets = wanted // ways
    return sets * ways, sets, ways


class Cache:
    def __init__(self, hunkbytes: int, total_hunks: int):
        self.entries, self.sets, self.ways = geometry(hunkbytes, total_hunks)
        self.tags = [INVALID] * self.entries
        self.stamps = [0] * self.entries
        self.clock = 0
        self.hits = 0
        self.misses = 0

    def access(self, hunk: int) -> bool:
        self.clock += 1
        base = (hunk % self.sets) * self.ways
        victim = base
        oldest = (1 << 64) - 1
        for way in range(self.ways):
            i = base + way
            if self.tags[i] == hunk:
                self.stamps[i] = self.clock
                self.hits += 1
                return True
            if self.tags[i] is INVALID:
                victim = i
                oldest = 0
                break
            if self.stamps[i] < oldest:
                oldest = self.stamps[i]
                victim = i
        self.tags[victim] = hunk
        self.stamps[victim] = self.clock
        self.misses += 1
        return False


def single_hunk_misses(trace: list[int]) -> int:
    current = INVALID
    misses = 0
    for h in trace:
        if h != current:
            misses += 1
            current = h
    return misses


def main() -> None:
    # Default chdman DVD hunks: 4 KiB -> 4096 cached hunks / 1024 sets / 4-way.
    assert geometry(4096, 1_000_000) == (4096, 1024, 4)
    # Largest accepted hunk: still retains a 16-hunk working set.
    assert geometry(1024 * 1024, 1_000_000) == (16, 4, 4)
    # Tiny images do not allocate beyond their logical hunk count.
    assert geometry(4096, 3) == (3, 1, 3)

    # Explicit collision/LRU test: four tags mapping to the same set survive;
    # touching the oldest replacement candidate changes which one is evicted.
    c = Cache(4096, 1_000_000)
    stride = c.sets
    colliders = [7 + stride * n for n in range(5)]
    for h in colliders[:4]:
        assert not c.access(h)
    assert c.access(colliders[0])
    assert not c.access(colliders[4])
    assert c.access(colliders[0])
    assert not c.access(colliders[1])  # it was the LRU victim

    # Sequential streaming does not create extra decode misses versus the old
    # one-hunk cache: every unique hunk is decoded exactly once.
    sequential = list(range(20_000))
    seq = Cache(4096, 1_000_000)
    for h in sequential:
        seq.access(h)
    assert seq.misses == single_hunk_misses(sequential) == len(sequential)

    # Locality-heavy filesystem/streaming trace: the old one-hunk cache must
    # re-decode nearly every access, while the bounded cache keeps the working
    # set resident after its first pass.
    working_set = list(range(512))
    locality = working_set * 200
    loc = Cache(4096, 1_000_000)
    for h in locality:
        loc.access(h)
    old_misses = single_hunk_misses(locality)
    assert loc.misses == len(working_set)
    assert loc.misses * 100 < old_misses  # >100x fewer decode misses

    # Mixed sequential stream + repeated metadata neighborhood.  This does not
    # claim a real-game speedup; it guards the intended locality benefit.
    mixed: list[int] = []
    for base in range(0, 8192, 64):
        mixed.extend(range(base, base + 64))
        mixed.extend(range(32))
    mix = Cache(4096, 1_000_000)
    for h in mixed:
        mix.access(h)
    old_mixed = single_hunk_misses(mixed)
    assert mix.misses < old_mixed

    print(
        "PASS: adaptive CHD hunk-cache policy "
        f"(locality misses {loc.misses:,} vs one-hunk {old_misses:,}; "
        f"mixed {mix.misses:,} vs {old_mixed:,})"
    )


if __name__ == "__main__":
    main()
