# Animal Well on KytyPS5: progress, where to continue, and what it takes to finish

Status date: 2026-10-10. Read `harness/ANALYSIS.md` first for how everything works and the evidence.
Acceptance stages (from the mission): 1 first frame, 2 title/menu, 3 gameplay 10 min,
4 visually correct, 5 performance (>=30 FPS, no shader stutter spikes).

---------------------------------------------------------------------------------------------------

## 1. Where we are

| Stage | State | Evidence |
|---|---|---|
| Build + launch on macOS (x86-64/Rosetta + MoltenVK) | **Done** | `run.sh` runs reproducibly |
| Boot without crash, assets load, game logic runs | **Done** | 60-90 s runs, no crash; atlas readback perfect; sprites animate |
| 1. First frame presented | **Done** | Three clean 20 s runs: 27 sampled frames each, 17 with content |
| 2. Title / menu visible | **Reached** | Captured 3840x2160 frame visibly shows Animal Well CRT title artwork |
| 3. Gameplay stability | **Done** | Three marked 10-minute Cross-input soaks ended cleanly |
| 4. Visual correctness | **Pending external reference** | Three fixed-point captures are consistent; PS5/reference comparison remains |
| 5. Performance | **Baseline only** | Mailbox median 59.12 FPS; three Immediate runs 60.31–60.71 FPS; startup spikes remain |

The original black-frame failure is fixed for PPSA22520 on Apple hosts by selecting ordinary
descriptor sets instead of MoltenVK push descriptors. The remaining work is quality and
measurement: verify physical speaker audibility, measure visible-state input response, and
compare fixed-point screenshots with an independent reference.

### Done, committed
* Apple/MoltenVK device-feature gating (3 missing features made optional, with explicit errors).
* NGS2 mastering gain + `sceNgs2RackGetInfo` + `sceNgs2VoiceGetPortInfo` (tests pass).
* Game-gated vertex subgroup fallback (harmless for this game's VS; keeps MoltenVK from rejecting it).
* Measurement harness (`run.sh`, scanners, stage tracker) + ~15 env-gated diagnostics
  (frame readback, internal render-target readback with PNG, sampled-texture readback, vertex
  probes, GPU-vs-guest vertex compare, viewport probe, solid-PS, clip-guard and depth-clamp switches).
* Docs: `SPEC.md`, `LOG.md`, `BLOCKERS.md`, `ANALYSIS.md`, this file.

### Not done
* Fixed-point visual comparison against an independent reference and sustained performance work.
* Audible confirmation on the user's selected macOS playback device.
* Visible-state response measurement for the user's reported 40 ms–1 s delay.
* The 108 unresolved imports (never called in the measured route) remain intentionally deferred.
* Keep the local save backup at `harness/backups/animal-well-20261010/` intact;
  its `MANIFEST.sha256` covers the two save files. No separate repository or
  standard macOS application-support config was found.

---------------------------------------------------------------------------------------------------

## 2. Current evidence and remaining failure band

The original all-black presentation has a confirmed cause and fix: MoltenVK push
descriptors left PPSA22520 resources unusable on Apple, while ordinary descriptor
sets produce the Animal Well CRT title artwork. The workaround is title-gated and
does not change other games.

The remaining work is outside the basic render path:

1. Input-driven routes generate nonzero NGS2 and SDL PCM. SDL opens successfully,
   reports `paused=0`, and its converted-byte queue drains. The host default is
   HyperX Cloud Stinger 2 Wireless; Kyty now accepts an exact `--audio-device`
   override for users listening on another output.
2. Mailbox timing measured 59.12 FPS median. Three Immediate runs measured
   60.31–60.71 FPS median and 19.83–20.34 ms p95, but startup shader spikes remain
   2.73–3.21 seconds. The clean Release pipeline cache is active.
3. One controlled Cross-input gameplay soak reached 10 minutes cleanly. Two more
   marked gameplay soaks are required for the stage gate.
4. Matched input-to-present samples are still missing because the locked macOS
   session did not deliver automated keyboard/controller events.
5. Fixed-point screenshot/reference comparison is still needed for visual stage 4.

---------------------------------------------------------------------------------------------------

## 3. Next steps, in order (each has a pass/fail signal)

**Step 1 — Verify the selected output route.** Run three short gameplay routes with
`KYTY_AUDIO_STATS=1 KYTY_AUDIO_DEVICE="MacBook Air Speakers"` (or the user’s
chosen exact SDL name). Pass: nonzero SDL RMS, `paused=0`, and a draining
converted-byte queue on all three runs.

**Step 2 — Repeat the gameplay gate.** Run two additional 10-minute routes with
`KYTY_ROUTE=gameplay`, preserving the existing save backup. Pass: timeout at
600 seconds, exit 0, no fatal/unimplemented/GPU errors, and gameplay audio/input
evidence in each run.

**Step 3 — Measure latency.** With an unlocked focused window and a real SDL
controller, run three Immediate probes with `KYTY_INPUT_STATS=1` and
`KYTY_FRAME_TIMING=1`. Pass: at least 30 matched controller events per run and
stable median/p95 next-present delays.

**Step 4 — Visual gate.** Capture the same fixed gameplay points on three runs and
compare against a reference capture with `harness/image_diff.py`, recording image
diffs and any missing effects.

**Step 5 — Startup stutter.** Keep the active Vulkan pipeline cache and profile the
remaining shader compilation spike; retain a change only if the three-run p95/max
startup metrics improve without reducing sustained FPS.

---------------------------------------------------------------------------------------------------

## 4. What happens after the first pixels appear (expect more work)

Each item will likely reveal its own issue; handle them as a loop with the harness gate
("target/frame `content=yes`, then PNG compared to a reference").

1. **Final composite correctness:** the 4K pass is an sRGB CRT/upscale shader that samples 320x180
   targets; verify sRGB/UNORM handling, the sprite colorkey discard (cyan), blend modes, and the
   Metal behavior of `OpGroupNonUniformBallot` in pixel shaders (used for the discard/WQM logic).
2. **Stage 2 (title/menu):** check that the title screen appears and animates. If the game waits
   for input, verify the pad library (`libPad`/user service) delivers events and map keyboard/controller.
   Check the audio path (NGS2/AudioOut) does not stall the game loop.
3. **Stage 3 (10 min stability):** run 10-minute soak runs x3; watch memory growth, the 1024-call
   unresolved-stub log cap, GPU resource leaks, swapchain recovery, any now-reachable stubs
   (resolve NIDs of the 108 imports and implement those the game actually hits).
4. **Stage 4 (visual correctness):** save reference PNGs at fixed points, add an image-diff check to the
   harness, compare internal targets (lighting, normals, bloom) for glitches.
5. **Stage 5 (performance):** 4K output (3840x2160) is heavy on a base M-series GPU while the game
   renders 320x180 internally: add a resolution/present scale option, disable diagnostics
   (`KYTY_FRAME_STATS=0`; the readbacks stall the GPU), keep the Vulkan pipeline cache enabled and
   investigate persistent shader compilation (62 shaders still compile at startup), profile the CPU
   side (Rosetta) and the per-draw descriptor/upload costs. Target: >=30 FPS
   average with no stutter above an agreed budget (set it with the user; the mission left [N] unset).

---------------------------------------------------------------------------------------------------

## 5. What it would take to fully fix the game (honest estimate)

* **Biggest unknown:** one root cause for "nothing rasterizes". Steps 1-4 are designed to find it in
  a few focused sessions; the evidence says it is a single systemic issue (it affects every draw),
  not many shader bugs. Once fixed, large parts of the scene should appear at once.
* **Then:** a tail of correctness bugs (MoltenVK vs RDNA2 semantic differences, depth range, sRGB,
  subgroup ops), input mapping, audio, and performance. Typically this tail is longer than the first
  fix; plan for iterative harness-driven loops per stage.
* **Risk areas:** MoltenVK limitations that cannot be worked around inside Kyty (portability subset
  gaps) might force shader-side emulation; Rosetta CPU overhead may cap performance; macOS support in
  Kyty is experimental upstream, so expect to fix unrelated Apple-only bugs and keep every change
  gated so other platforms/titles are unaffected.
* **Process to keep:** one fix per commit, before/after runs x3 using `scan.txt` (`content=` and
  presented frames), reviewer pass on each diff, log every stub, update `LOG.md`, back up `_SaveData/`.

---------------------------------------------------------------------------------------------------

## 6. Quick reference for the next session

* Build: see `ANALYSIS.md` section 2. Run: `./harness/run.sh name` (`KYTY_TIMEOUT_SECONDS`, `KYTY_FRAME_DUMP=1`).
* Fast target check (the key measurement):
  `KYTY_INTERNAL_FRAME_STATS=1 KYTY_INTERNAL_FRAME_SAMPLED=1 KYTY_INTERNAL_FRAME_SKIP=300 KYTY_INTERNAL_FRAME_MAX=60 KYTY_INTERNAL_FRAME_DUMP_DIR=<dir> ./harness/run.sh x`
  then summarize `InternalFrameStats` lines (role, extent, `rgb_nonblack`) and view the PNGs.
* Per-draw geometry: `KYTY_INTERNAL_FRAME_VERTICES=1 KYTY_INTERNAL_FRAME_PIXEL_HASH=0xbe7777460c3444ff`.
* Key shader hashes: VS `0xcdcf88828e5c2df8` (all draws), sprite PS `0xbe7777460c3444ff`, final 4K PS
  `0xd89c2250b89200d0`. Current SPIR-V is in `harness/runs/solid-spv/shaders/`.
* Do not: `git add harness` or `git add .`; run `KYTY_GRAPHICS_DEBUG_DUMP` for more than ~10 s; trust
  a SPIR-V dump from an older build; judge progress by "no crash" alone.
* Provenance note for the user: the archive is tagged `[SuperPSX]` and contains modified files
  (`.bak_*`); confirm it came from your own console dump and keep the files private.
