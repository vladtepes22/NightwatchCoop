# Memory fixes backported from visy (temporary improvement)

Part 1: texture cache collector (VRAM). Part 2: the rest of visy's memory list, including the
released-memory memset that makes RAM resident (see "Part 2").

Status: Part 2 done (`db3dd1be`). Part 1 done 2026-10-24 (branch `texture-gc-visy`): visy's
policy replaces `BB_GC_PRESSURE_PERCENT`, `BB_GC_PRESSURE_SECONDS` (now visy's
`BB_GC_PRESSURE_IDLE_SECONDS`, 5 s) and the integrated/forced-budget block. Kept, because visy's
version lacks them: the GPU hang guard (`BB_GC_RECENT_FRAMES`: memo hits do not touch the LRU),
the image graveyard (`BB_IMAGE_FREE_DELAY_FRAMES`), and Lantern's `StoreTiledImage` for
GPU-written tiled images (used past the budget instead of visy's unwritten drop). No
`VramShort()` exists in our tree, so visy's escalation on it is not ported. The launcher's
"VRAM budget" switch (`exp_vram_budget`) still sets the two removed variables: they do nothing now.

This is a **temporary divergence** from Lantern Engine. If Lantern (Awakenchan) ships a texture
cache collector that fixes the problems below, drop this patch and take theirs (see "Undoing").

## Why

Players on small and mid-size cards (4-8 GB) get stutter, flicker and "Stall during rendering"
when the texture collector runs. Evidence from a real log (RTX 3050 Laptop, 4 GB VRAM, 15.8 GB RAM,
Nightwatch 0.2.3 beta 1, `nightwatch-20261023_235541.log`, kept outside the repo):

- Marks on that card: pressure 1536 MiB, critical 3072 MiB (`Texture cache: memory pressure, 2934
  of 1536 MiB (critical 3072)`). Usage sat above pressure for the whole session, so every report
  (every 5 s) evicted 30-580 images, which were then re-created and re-uploaded.
- Our collector: `TextureCache::GarbageCollectImages()` in
  `engine/gpu/shadps4/video_core/texture_cache/texture_cache.cpp` (~line 1552). Its marks come from
  a Lantern/deadinside mix: a fixed percentage of the driver budget (`BB_GC_PRESSURE_PERCENT`,
  default 80, critical 10 points higher), special cases for integrated GPUs and a fixed forced
  budget (`BB_GC_BUDGET_MB`), `BB_GC_RECENT_FRAMES`, `BB_GC_PRESSURE_SECONDS`, `BB_GC_IDLE_SECONDS`.
  It compares usage before freed images are destroyed and counts the holes freed images leave in
  shared VMA blocks, so it stays "over" the mark and keeps evicting.

visy found the same thing on an 8 GB RX 5700 XT (flicker and ~0.5 s drops entering the Research Hall
or leaving a lift) and rewrote the policy. His version has shipped since visy 0.7.3.

Part 1 does not lower system RAM; it stops the texture cache thrashing. The RAM fix is in Part 2.

## What visy's change does

Sources (fetch the `visy` remote first):
- `90a18103` "texture cache: budget-derived marks, gradual LRU eviction, no write-back stalls"
- `a816985a` "drop the BB_GC_AB_FILE A/B aid and the 0.7.2 collector; CHANGES entry"
  (apply both: the first adds an A/B aid and keeps the old collector, the second removes both).

Behaviour (`gpu/shadps4/video_core/texture_cache/gc_policy.h`, new, header-only, 171 lines):
- Marks are a fixed reserve under the driver budget (VK_EXT_memory_budget, capped by the VRAM
  heaps): hard = budget - 5 % (160-384 MiB), gradual band 10 % (256-768 MiB) under that.
- Usage is measured once a frame, less VMA block slack and images pending destruction.
- LRU eviction with a per-frame quota (8/24/48/96 images by level); never images used in the last
  8-600 frames.
- GPU-written images are kept below the budget itself; write-back only one per frame, only past it.
- Cards up to 4 GB use the VRAM heap size as the budget (their driver budget leaves out what the
  other heaps hold).

## Files visy's commits touch (and what happens in our tree)

A dry run (`git diff 90a18103^ a816985a -- gpu tests docs`, `git apply --check --directory=engine`)
on 2026-10-24 gave:

| File (visy path, ours is under `engine/`) | Result |
|---|---|
| `gpu/shadps4/video_core/texture_cache/gc_policy.h` (new) | clean |
| `tests/test_gc_policy.cpp` (new, target `gc-policy-test`) | clean |
| `gpu/shadps4/core/libraries/videoout/driver.cpp` (frame log gc columns) | clean |
| `gpu/shadps4/video_core/texture_cache/texture_cache.h` | clean |
| `gpu/shadps4/video_core/texture_cache/texture_cache.cpp` | **conflicts** (our collector differs) |
| `gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp/.h` | **conflicts** at ~1203 (we already have `GetDeviceMemoryBudgetNow`, `GetDeviceMemoryUsage`) |
| `gpu/CMakeLists.txt` (adds `gc-policy-test`) | **conflicts**: our tests are in `gpu/cmake/SeamlessTests.cmake`, add it there |
| `gpu/shim/bbport_toggles.h` (gc stats atomics) | **conflicts** around line 210 |
| `docs/party/DEBUG_SWITCHES.md` | conflicts: add the new switches by hand |

## Plan

1. Branch from `lantern-0937` (or `port-0.9.37`; check which is current). Do not touch other work.
2. `git fetch visy`. Apply the clean files first: `gc_policy.h`, `test_gc_policy.cpp`,
   `texture_cache.h`, `driver.cpp`, using `git apply --3way --directory=engine` on the per-file
   diff, or copy the files from `visy/master` where our version is identical.
3. Register `gc-policy-test` in `engine/gpu/cmake/SeamlessTests.cmake` (same lines as visy's
   `gpu/CMakeLists.txt:326-329`). Build and run it first: `ninja -C out/gpu gc-policy-test &&
   out/gpu/gc-policy-test.exe`.
4. `texture_cache.cpp`: port visy's `GarbageCollectImages()` and its helpers, **replacing** our
   `bbport:` collector pieces rather than keeping both:
   - Remove: `BB_GC_PRESSURE_PERCENT` (~163-175), `BB_GC_PRESSURE_SECONDS` (~177-185),
     `BB_GC_RECENT_FRAMES` (~187-200), the integrated/forced-budget block (~1559-1580),
     `BB_GC_IDLE_SECONDS` (~1593-1610) and the old "memory pressure" print (~1739-1750), **only
     where visy's policy covers the same job**. Read each before deleting; if one has behaviour
     visy's version lacks (for example the 20 s idle rule for textures of an area left behind),
     keep it and say so in the commit.
   - Keep anything Lantern/shadPS4 owns that visy's diff does not touch.
   - Check callers of `VramShort()` / `NoteVramShort` in `buffer_cache.cpp` still work: the buffer
     cache's `EvictIdleCopies` and `ProcessIdleBlocks` read the texture cache's pressure state.
5. `vk_instance.cpp/.h`: keep our existing functions; add only what visy's commit adds
   (~15 lines, VRAM heap capped budget). Do not duplicate.
6. `bbport_toggles.h`: add visy's gc atomics next to ours; remove ones nothing reads any more.
7. Docs: add the new switches to `engine/docs/party/DEBUG_SWITCHES.md` and a CHANGES line.
8. Mark every changed block with a `bbport:` comment naming this document and visy's commit.

## Proof (smallest sufficient)

- `gc-policy-test` passes.
- `ninja -C out/gpu seamless-tests && cd out/gpu && ctest -j8` once at the end (all 55 plus the new
  test must pass).
- One headless smoke (~3 min; the change is in start-up/engine hooks territory): `tools/smoke-game.py`
  as in CLAUDE.md. Pass = "party tick seen: True", "New-game progression verified: True", then grep
  `session.log` for crash / exception / VK_ERROR. Headless does not draw, so this proves nothing about
  eviction behaviour.
- Visual proof needs a real card. Ask the user for a before/after log from the 4 GB RTX 3050 laptop
  (or any <= 8 GB card): in the new log the `Texture cache: memory pressure` lines should be rare
  or gone, and "images evicted" per report far lower than the 30-580 seen before. Do not claim an
  FPS or stutter result without that log.
- Never launch a visible game (project rule). Only the headless smoke.

## Build and release notes

- Any engine change gives a new party build ID (host and guests must match). `cmake out/gpu` only
  right before the release build.
- Never switch `BB_LTO` between builds of one tree.
- Commit as `vladtepes22 <345212603+vladtepes22@users.noreply.github.com>`. No Co-Authored-By
  trailer and no mention of Claude in commits, even though visy's own commits carry one: write a
  fresh commit, do not cherry-pick his message.
- Credit visy (commit hashes) in the code comment and the release notes.

## Part 2: the rest of visy's memory list (checked 2026-10-24)

visy sent a list of every memory fix in his tree. Checked against ours:

| visy commit | What | Ours | Action |
|---|---|---|---|
| (since b7001bc5) `win_mem_punch` | Released direct memory is dropped with `VirtualAlloc(MEM_RESET)`, nothing touched | **Different**: `win_mem_zero` (`src/win32_memory.c:104`) **memsets every released range** (non-lazy pool). Untouched pages get faulted in, so every release makes RAM resident. Called from `direct_release` (`runtime_memory.c:524` via `zero_phys` -> `punch`) and `flex_free` (`:278`). Matches visy's "the gap opens during the ~9 s Continue load". | **Do it, first priority for RAM** |
| 47cc3e1c (+97de7d51) | Pool falls back to SEC_RESERVE + commit-on-map when Windows refuses the full commit | Partial: we have the lazy pool only as opt-in `BB_POOL_LAZY=1` (`win32_memory.c:61`, `start/common.cpp:173`); a refusal does not switch to it | Do it: auto fallback on refusal, keep `BB_POOL_LAZY` as the forced switch |
| e4dbb97d | Wait out a commit refusal | **Have it** (`BB_POOL_COMMIT_WAIT`, our 1fe45969) | none |
| 6755d58e | Render resource cache 512 MiB in the port's extra direct memory (`BB_DMEM_EXTRA_MB`) | Absent (we enlarge render pools to 1024 KiB in the image patch instead) | Skip for beta 2 (different mechanism, larger port) |
| 90a18103 + a816985a | Texture collector | Absent | Part 1 above |
| aad0ada4 (c1991c89) | Idle VRAM copies leave VRAM on cards <= 8 GB | **Have it** (`BB_VRAM_IDLE_SECONDS`, `buffer_cache.cpp`) | none |
| f4dafbcc | Out of VRAM for a new image: free unused textures in rounds and retry | Absent: `image.cpp:179` asserts (crash) when the driver refuses even past the budget | Do it (small: `UniqueImage::TryCreate` + caller retry) |
| 4c4bb35a | VRAM full: allocate past the budget instead of stopping | Half: images yes (`image.cpp:163-176`), **buffers no** (`buffer_cache/buffer.cpp:~100` asserts) | Do the buffer half (+ `NoteOverBudget` log line) |
| bcdce7b2 / 0d52d159 | Idle depth targets freed without write-back (VRAM leak, PR #89) | Absent | Usually comes with visy's `GarbageCollectImages` in Part 1; check it is there after the port |

### The MEM_RESET change (RAM)

- Replace the non-lazy `memset` in `win_mem_zero` with visy's `win_mem_punch`:
  `VirtualAlloc(pool_view + phys, size, MEM_RESET, PAGE_READWRITE)` (on a reserved pool it fails on
  never-committed pages, which hold nothing; that is fine).
- **Risk:** MEM_RESET does not zero. A range the game releases and allocates again may hold old
  data. visy has shipped it since his first Windows port, so the game copes. Check whether Lantern
  added the memset on purpose: `git log -S"memset(pool_view" -- engine/src` and Lantern's own
  history (`awaken` remote). If it was a crash fix, say so in the report and keep zeroing only
  where needed (for example: zero only pages `QueryWorkingSetEx` reports resident).
- Keep the `lazy` branch (committed-only memset) or switch it to MEM_RESET too; same reasoning.

### Proof for Part 2

- ctest and the smoke as above (the smoke loads a new game, which exercises allocate/release).
- In the smoke's `session.log`, compare the `Memory: game ... MiB in RAM (... committed ...)` lines
  with a smoke run before the change (same command, same `--seconds`). RAM after load should drop.
  Report both numbers. Headless still loads the game's data, so this one is measurable here.

## What it does not fix

- **The reported number itself.** The log's "game 11045 MiB in RAM" may also be partly a double
  count: the runtime maps the pool twice (guest view plus `backing_base`, `src/runtime_memory.c:70,128`)
  and `QueryWorkingSet` may count shared pages per view. Not verified; the MEM_RESET change above is
  the real reduction either way.
- The "cleanup" line (`Guest memory: cleanup, ...`) is the buffer cache's idle copy release
  (`buffer_cache.cpp` `EvictIdleCopies`, default 180 s). Different code, unchanged here.
- Our direct memory pool is 5056 MiB by default (visy's is 6144); we only raise it to 9152 MiB for
  scenes above 1080p, Highest model detail or screen space reflections.

## Undoing

If Lantern fixes its collector: revert the branch/commit(s) of this backport and port their change
as usual (the normal "Porting a new Lantern beta" workflow). Keep `gc-policy-test` only if the
policy header stays. Remove this document in the same commit.
