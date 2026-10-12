# Silent Hill: The Short Message (PPSA10112) — memory + FPS plan

Written 2026-10-12. Read this first, then the newest entries at the end of `harness/LOG.md`.
Every finding goes into `harness/LOG.md` (newest at the end) the same day.

---

## 0. Ground rules (unchanged, non-negotiable)

1. Every new path is **opt-in behind an env var**. Defaults change only after the promotion bar in §7.
2. **Animal Well (PPSA22520) 3/3** after any change to shared code (45 s each; see §6.4).
3. **One commit per retained change.** Log-only commits are prefixed `harness:`.
4. **Native 4K output.** `KYTY_VIDEOOUT_RES` is a diagnostic only, never part of a result.
5. Claim **stage 2** only, until gameplay and visual checks pass.
6. Work in place in `/Users/aviral/Documents/ChatGPT/PS5/KytyPS5`, branch `codex/animal-well-compat`. Never re-clone.
7. **Disk:** `df` ≥ 20 GB before every run (`run_arm.sh` checks this). Trim frames (the script does this).
8. **Laptop must be idle during measured runs.** B/C on 2026-10-12 were invalidated by interactive use
   (intro FPS 13.9/18.6 vs 21.7 before any memory pressure). If the user is on the machine, run only
   smoke/crash checks, not FPS comparisons.
9. No destructive git operations (reset --hard, force push, deleting runs) without the user's OK.

---

## 1. What we know (measured, with sources)

### 1.1 FPS shape
- Intro about 22–27 fps, collapsing to about 1.5 fps in game. The collapse happens between 40 and 160 s
  (`shtsm-mem-A`: 21.7 → 15.3 → 4.1 → 1.7).
- In game (≥ 160 s) FPS is flat at 1.3–1.85 on the best flags (best480-4: 1.48 mean, 320–480 s).
- One run (`shtsm-b3-label-1`) reached **free walking at 3.9–5.8 fps**, so the phone-message scene is
  itself heavier than walking. Never compare FPS across different scenes.
- Lower resolution doesn't help: 720p gives about 1.9 fps on the menu.

### 1.2 Memory
- Peak footprint 22–24 GB on the 16 GB Air, swap 8–10 GB, free 10–12 %.
- **Breakdown at 250 s** (`harness/runs/shtsm-mem-C300/footprint-250s.txt`, 20 GB total):
  | Category | Size | Meaning |
  |---|---|---|
  | SYSTEM_MANAGED | 10 GB | guest memory backing (shm), i.e. what the game itself holds |
  | Owned physical footprint (graphics) | 7.9 GB | Metal resources: texture/buffer caches |
  | IOAccelerator (graphics) | 1.2 GB | driver-side GPU memory |
  | everything else | ~0.9 GB | malloc 0.6, Rosetta 0.15, page tables |
- Footprint jumps **7 → 17 GB in about 30 s** at the level load (63–95 s); GPU cache grows only about 2.5 GiB
  in that time, so most of the jump is the game filling its own pool.
- In game, footprint keeps growing about 3 GB over 160–300 s while the GPU cache is flat. Unknown cause
  (pool still streaming, or a leak). See P1.3.

### 1.3 Texture cache / eviction (`KYTY_EVICT_STATS=1`)
- GPU cache usage 7.8–8.8 GiB against an 8192 MiB budget. The budget formula (`src/graphics/host_gpu/vma.cpp:142`)
  gives the caches 8 GiB on 16 GB RAM.
- Default GC: `head40_blocked=40/40`, `freed=0` every line in game. It spends its whole budget on
  GPU-written tiled images it may not free.
- `KYTY_EVICT_SMART=1 KYTY_EVICT_ALIAS=1`: also frees about 0 (`skip_tiled` = everything scanned).
  With `KYTY_GPU_BUDGET_MB=4096` usage stayed at 7.8 GiB. **The budget can't be enforced under the
  current eviction rules.**
- Census in game: clean 2.1–3.1 GiB (all younger than 160 GC ticks, i.e. the working set, so not
  reclaimable), tiled_gpu 3.2 GiB, **idle160_tiled_gpu 1.4–1.6 GiB** (old, GPU-written, tiled: the
  only idle bulk). Rules in `TextureCache::ClassifyForEviction` (`textureCache.cpp:2587`); tiled
  GPU-written images are never downloaded, so never freed.

### 1.4 Memory ↔ FPS: NOT proven
- Over a whole run r(fps, footprint) ≈ −0.97, but that is confounded with the intro → in-game scene change.
- In game, FPS does not follow swap-in rate (125–3600 pages/s; r between −0.5 and +0.7 depending on run/window).
- Arm C was meant as the causal test but failed because the budget couldn't lower memory.

### 1.5 Where the time goes (profiles from batch 1–2, taken under swap, so re-take after P1)
- Guest GPU thread about 100 % busy in game and on the critical path; RenderThread waits on it about 47 %.
  - CommandScheduler::Wait (full drains): 36 % before batch 2, about 16 % after.
  - MoltenVK addCommand (PREFILL=2 inline encoding): about 20 %.
  - Pipeline lookup + SRT walk: about 13 %.
  - PrepareBda: 9 % → 3 % with BDA_SYNC_SKIP.
- `CommitBindings` overlap checks: about 500 pairs per draw, 2.2 billion over 4.4 M draws (`StateCache` line).
- RenderThread `_sigtramp`: 16.2 % (best) → 5.7 % with `KYTY_WIDE_STORE_NOTRAP=1`.

### 1.6 Input
- The Animal Well queue-backlog bug (`56852717`, `harness/INPUT_LAG_FIX.md`) is fixed in shared code.
- SH uses `scePadReadState`; the low-FPS tap loss is fixed by the latch in `55ecc06f` (on by default).
- Felt lag in SH is expected from FPS: 2–3 pipelined frames × 670 ms ≈ 1.3–2 s.
  **Not yet measured with a real controller** (see P2.0).

### 1.7 Tried and rejected (don't redo without new evidence)
GUEST_IMPORT (entry 34, c9401c87: war_wait hazards, 233/1000 MiB imported, no RAM saving, intro FPS
halved); AUDIO2_POLL_PACE (regression); SEM_SPIN (0 hits); GPU_BLOCKED_BACKOFF (neutral/worse);
MESH_BATCH (1.24×, below the bar); SUBMIT_THREAD (crash, see P3.1); PREFILL=0 (device loss, see P3.1);
SRT memo, state cache, upload batching, frames-in-flight (0–1 %); BDA_TABLE_GROW (−464 MiB, < 1 % FPS);
EVICT_SMART for memory (frees ≈ 0 in SH; harmless, keep opt-in).

---

## 2. Tools

All in `harness/memstudy/` (copied from the /tmp runner scripts so they survive reboots):

| File | Use |
|---|---|
| `run_arm.sh <name> <sec> [ENV=VAL...]` | One SH arm on the best480-4 route (BEST flags + J/F10 probe + stats). Samples memory, takes footprint breakdowns at 180 s and at 80 % of the run, prints FPS-vs-memory windows, trims frames. `MEMHOG_GIB=<n>` adds memory pressure. |
| `memsamp.sh <csv>` | 5 s sampler: kyty footprint/RSS, swap, free %, vm_stat swapins/swapouts/pageins. |
| `memcorr.py <run> [win]` | Joins FrameTiming FPS with mem.csv per window; prints correlations. |
| `memhog.py <GiB> [sec]` | Holds incompressible resident memory (pressure test). |
| `memmon.sh` | Safety kill: footprint > 24 GB, swap > 12 GB, free < 6 %. |
| `fpsw.py <run> [win]` | Plain FPS per window from FrameTiming. |
| `tr2.sh` | SH route wrapper (required MVK/mesh env + input probe). |

Build: `cmake --build _Build/macos-x86_64 --target kyty_emulator -j8` (about 1 min incremental).

**Standard run length: 280 s.** The game is in game by about 160 s; measure the **180–280 s** window and
check the last frame shows the same scene in every arm. Use 120 s only for crash/smoke checks.

---

## 3. Measurement protocol (to cut runs from 3 per idea to 1)

1. **Measure the mechanism, not just FPS.** Each change must have a counter that shows its cost went away
   (drains/2 s, encode ms, overlap pairs, trap hits, freed MiB). Counters are deterministic enough
   that **one run** proves the mechanism. FPS is the secondary readout.
2. **In-run A/B** for runtime-safe flags (P2.2): toggle every 20 s in the same run and scene, and compare the
   on/off windows. Removes run-to-run noise.
3. **Combine independent changes** in one run when each has its own counter.
4. **3 runs only for the final promotion check** (§7).
5. Report every run as: name, flags, length, exit/timed_out, stage, exceptions, last-frame scene,
   FPS per 40 s, the change's counter, peak footprint/swap.

---

## 4. Phase 1 — decide whether memory is worth fixing (gate)

### P1.1 Pressure test (no code; 2 runs × 280 s, laptop idle)
```sh
sh harness/memstudy/run_arm.sh shtsm-p1-ctl-1 280
MEMHOG_GIB=3 sh harness/memstudy/run_arm.sh shtsm-p1-hog3-1 280
```
- Same flags, same route; the only difference is about 3 GiB more pressure in the hog arm.
- Compare FPS 180–280 s and swap-in rate, and check both last frames are the same scene.
- **Decision:**
  - Hog arm FPS ≥ 25 % lower **and** swap-in clearly higher → **memory matters**. Do Phase 1b before Phase 2/3.
  - Hog arm FPS within ±10 % → **memory isn't the limit.** Skip Phase 1b (park it), go to Phase 2.
  - In between → one repeat pair (`-2`) and decide on both pairs.
- If memmon kills the hog arm (footprint > 24 GB / free < 6 %), retry with `MEMHOG_GIB=2`.

### P1.2 Clean profile (in the control run or one extra run)
At about 220 s: `sample <pid> 10 5 -mayDie -file /tmp/samp-<run>.txt`, then symbolicate with
`python3 /tmp/symb.py` (copy it into `harness/memstudy/` if it's still in /tmp). Record GPU-thread
shares as in §1.5. This replaces the swap-polluted batch-1 profile for ranking Phase 3.

### P1.3 Host growth in game (diagnostic, can share the control run)
Footprint grows about 3 GB over 160–300 s with the GPU cache flat. `run_arm.sh` saves `footprint-180s.txt` and `footprint.txt` (at 80 % of the run);
diff SYSTEM_MANAGED vs graphics vs malloc.
- SYSTEM_MANAGED growing → guest pool streaming (expected) or guest leak.
- MALLOC/graphics growing → emulator leak (like the old AvPlayer leak, 78019f9f). That would be a bug
  fix with direct value. Chase it with `KYTY_BUFFER_CACHE_STATS`, `heap`/`vmmap --summary`.

---

## 5. Phase 1b — memory reduction (only if P1.1 says memory matters)

Target: **no swapping, about 12–13 GB footprint.** 10 GB is not reachable at native 4K: the floor is about
10–12 GB (game's non-texture data 5–7 GB + 4K render targets 2–3 GB + driver/emulator about 2 GB). Realistic
with everything: **13–15 GB.**

Ordered by value per effort. Re-measure footprint after each step (one 280 s run, counters + footprint
breakdown); go to the next step only if still swapping.

### M1. Shrink Unreal's texture streaming pool (game patch; est. −2 to −4 GB; about 1 day)
- Reduces **both** copies (guest + GPU) at once.
- Mechanism: like the PPSA10112 back-buffer patch (`Loader::GamePatch::ApplyVideoOutResolution`,
  `src/loader/gamePatch.cpp`, called from `runtimeLinker.cpp`). It verifies surrounding bytes, then rewrites immediates.
- Steps:
  1. Find how the pool size is set in the eboot: search for the strings `r.Streaming.PoolSize`,
     `r.Streaming.LimitPoolSizeToVRAM`, `PoolSizeVRAMPercentage`, and the ini/`[TextureStreaming]`
     section; locate the `lea` of the cvar name plus the default immediate (same pattern as the resolution patch).
     Alternative: if the game reads `DeviceProfiles`/ini from the app dir, check whether an ini override is honoured
     (cheaper than patching code; check `guest-printf.log` for ini load lines).
  2. Opt-in `KYTY_UE_STREAMING_POOL_MB=<n>` for PPSA10112 only; log the rewrite like the resolution patch.
  3. Measure at 1024/1536/2048 MB: footprint breakdown, `EvictStats usage_mib`, FPS 180–280 s, and
     **visual check** (texture blur in frames vs the baseline frame at the same scene).
- Risk: lower texture detail up close. Acceptable as opt-in; record it in the status doc.

### M2. Make the GPU budget enforceable: evict idle GPU-written tiled images (est. −1.5 GB; 1–2 days)
- New class handling in `RunSmartReclaim` (`textureCache.cpp` after line 2742), behind
  `KYTY_EVICT_TILED_DOWNLOAD=1` (requires EVICT_SMART):
  idle ≥ age, `SafeToDownload()`, tiled → **download + retile to guest memory**, then free.
  Cap at 1–2 per call (like `kMaxDownloadsPerCall`) so drains don't return.
- Counters: add `tiled_dl_evict=<n>/<MiB>` and the drain time it cost to EvictStats; `thrash` must stay low.
- Accept if: usage tracks `KYTY_GPU_BUDGET_MB` (e.g. 6144) within 10 %, ReadbackStats drain time doesn't rise
  more than 10 %, no visual regressions, AW 3/3.
- Note: downloading writes the guest pages, which can **raise** SYSTEM_MANAGED if they weren't resident.
  Check the net footprint, not just GPU usage.

### M3. Dedup clean, game-loaded textures (est. −2 to −3 GB; multi-day, highest risk; last resort)
- Only after M1+M2, and only if still swapping.
- Idea: after a clean texture is uploaded, protect its guest pages and release their physical memory;
  on a CPU touch, restore from the GPU (download + retile) through the existing fault path.
- Blocker: guest memory is `shm`-backed (`src/kernel/memoryAddressSpace.inc:70`, MAP_SHARED, so the same
  physical memory can be mapped at several addresses). macOS can't discard shared pages in place. This needs a
  per-range backing change (e.g. remap those ranges to private anonymous memory + `MADV_FREE_REUSABLE`) and
  correct aliasing. Write a DESIGN.md first and get the user's OK before coding.
- Do **not** revive GUEST_IMPORT for this; its failure modes (war_wait, fb_demoted) are recorded in LOG.md.

### Not worth doing for memory
- EVICT_SMART alone; lowering KYTY_GPU_BUDGET_MB alone (not enforceable until M2); resolution changes.

---

## 6. Phase 2 — measurement infrastructure (do this regardless of the P1.1 result)

### P2.0 Real-controller input check (piggyback, no extra run)
In the P1.1 control run, with the user's DS4/DualSense connected: add `KYTY_INPUT_STATS=1`. Pass: host →
game sees button ≤ one frame interval. If several frames, open a separate input-bug investigation
(SH reads through `scePadReadState`; see `55ecc06f`).

### P2.1 GPU-thread frame timing counters (one commit; the most useful single tool)
`KYTY_GPU_THREAD_STATS=1`: every 2 s log per-category wall time on the guest GPU thread:
`drain_wait_us` (CommandScheduler::Wait, split by origin), `mvk_encode_us` (time inside vkCmd* /
vkEndCommandBuffer), `bind_us` (CommitBindings incl. overlap checks), `pipeline_us`, `bda_us`, `record_other_us`,
`idle_us`, plus draws/dispatches. Cheap (steady_clock deltas around existing scopes).
This turns every run into a profile and is the counter for P3.1–P3.4.

### P2.2 Runtime A/B toggle (one commit)
`KYTY_AB_TOGGLE=<FLAG_A,FLAG_B,...>` + `KYTY_AB_PERIOD_S=20`: the listed flags flip on/off every period
(log `ABToggle: on|off t=...`). Only for flags that are safe to switch between frames (pure per-draw choices like
SPEC_READ_HOIST, STATE_CACHE, BDA_SYNC_SKIP); flags read as `static const bool` need a small refactor to an atomic.
Extend `fpsw.py` to split FPS by toggle state.

### P2.3 Animal Well regression
```sh
for i in 1 2 3; do env MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2 KYTY_FRAME_STATS=1 KYTY_FRAME_DUMP=1 \
  KYTY_TIMEOUT_SECONDS=45 <new flags> ./harness/run.sh aw-regress-$i-<tag>; done
```
Pass: stage 2 with content in 3/3, no exception (scan.txt).

---

## 7. Phase 3 — FPS work on the GPU-thread critical path

Rank by the P1.2/P2.1 numbers; the order below is the current best guess. Each item: opt-in flag, counter,
one 280 s run (+ in-run A/B where possible), AW 3/3, one commit.

### P3.1 Move Metal encoding off the critical thread (est. 1.2–1.5×; about 20 % of the GPU thread)
- Two bugs block it:
  - `PREFILL=0`: device loss `kIOGPUCommandBufferCallbackErrorInvalidResource` at 31 s. Texture lifetime:
    `KYTY_RETIRE_ON_LAST_USE` (textureCache.cpp:382) fixes most of it but not every case. Find the remaining
    resource freed before its command buffer completes (see `work/mvk_crash_analysis.md` if present;
    Metal validation `MTL_DEBUG_LAYER=1` on a 60 s run).
  - `KYTY_SUBMIT_THREAD` (commandScheduler.cpp:501): null deref (0x20) in `SubmitAsync` at 33 s. A race
    with MoltenVK useResources/pool. Needs per-thread command pools or a fence-guarded handoff.
- Counter: `mvk_encode_us` on the GPU thread drops by ≥ 15 % of wall time.

### P3.2 Finish drain removal: wait for the producer, not the backlog (est. 1.2–1.4×)
- `READBACK_PRODUCER_TICK` (7fde5c50) records which submission wrote each buffer. Finish it: CPU faults
  and readbacks wait on that submission's fence only, and submit early when the producer isn't submitted yet.
- Counter: `drain_wait_us` and ReadbackStats GPU-wait per origin.
- Batch 2 (the biggest gain so far, +25–40 % in game) came from this family.

### P3.3 Per-draw bookkeeping (est. 1.1–1.2×)
- `CommitBindings` overlap: about 500 pairs per draw. Replace the linear scan with an interval index over
  written ranges (built once per call; SPEC_READ_HOIST already collects them).
- Pipeline lookup + SRT walk (about 13 %): better cache key / memo with a correct invalidation rule
  (SRT_MEMO hit rate was only 5–10 %; understand why before redoing it).
- Counter: `bind_us`, `pipeline_us`.

### P3.4 Traps (small, cheap)
- Promote `KYTY_WIDE_STORE_NOTRAP` after 2 more same-route runs (RenderThread `_sigtramp` share is the counter;
  target < 2 %). Identify the remaining 5.7 % (probably write-fault SIGSEGV: `KYTY_WRITE_FAULT_STATS`).

### P3.5 Parallel command processing (est. ≥ 2×; large; only if P2.1 still shows the GPU thread at ~100 % after P3.1–P3.3)
- Process independent PS5 command streams (graphics DCB vs async-compute ACBs) on separate threads with
  separate Vulkan command buffers; synchronise on the guest's own labels/semaphores.
- Prerequisite: thread-safety audit of BufferCache/TextureCache/CommandScheduler. DESIGN.md + the user's OK first.
- Optional enabler: a replay harness (record 30 s of in-game command buffers + a memory snapshot, replay the GPU
  thread offline) for deterministic, game-free perf iteration.

### Not doing
Native ARM build (guest x86 code needs Rosetta anyway); lower resolution; re-trying §1.7 items.

---

## 8. Promotion bar (opt-in → default)

A flag becomes default only with: mechanism counter confirmed; **3 same-route 280 s runs** with FPS
180–280 s ≥ the baseline mean (no regression beyond 5 %); no exception / device loss / visual regression
(frames at the same scene compared side by side); **Animal Well 3/3**; LOG.md entry; own commit.
Current candidates: `KYTY_WIDE_STORE_NOTRAP`, batch-2/3 BEST set.

---

## 9. Run budget (lean protocol)

| Step | SH runs | Notes |
|---|---|---|
| P1.1 pressure test (+P1.2 profile, P1.3, P2.0 piggyback) | 2 (+2 if unclear) | decides Phase 1b |
| P2.1 + P2.2 infra | 1 | validation run, plus AW 3×45 s |
| M1 pool patch (if memory matters) | 3 | one per pool size |
| M2 tiled eviction (if still swapping) | 1–2 | |
| P3.1 encoding thread | 2 | 120 s smoke + 280 s |
| P3.2 + P3.3 + P3.4 | 2–3 | combined where counters separate them |
| Final promotion of the winning stack | 3 | |
| **Total** | **~14–16** (~1.3 h of runs) | vs ~30+ with 3 runs per idea |

---

## 10. Expected outcome (estimates, not promises)
- Memory: 20–24 GB → about 13–15 GB with M1+M2 (+M3); 10 GB isn't reachable at native 4K.
- FPS in game: about 1.5 → about 4–6 with P3.1 + P3.2 (+ memory, if P1.1 shows it matters). This matches the
  3.9–5.8 fps one run reached while walking. Beyond that needs P3.5.
- Input lag shrinks with FPS (≈ 2–3 frames of pipeline latency).

## 11. Open decisions for the user
1. Revert c9401c87 (GUEST_IMPORT) or keep it opt-in forever.
2. Whether a lower-detail texture pool (M1) is acceptable as an opt-in for performance.
3. Approval before M3 or P3.5 (both multi-day, design doc first).
4. Memmon thresholds (footprint 24 GB / swap 12 GB / free 6 %).
