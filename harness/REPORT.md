# Animal Well on KytyPS5 — current report

Status: gameplay stability is met on three 10-minute runs. Visual correctness,
physical speaker confirmation, and a visible-state explanation for the user's
long input-delay reports remain in progress.

## Verified

- Build: x86-64 Kyty under Rosetta 2, Apple Silicon macOS, MoltenVK 1.4.2.
- Three clean 20-second descriptor-set runs: 17/27 sampled frames contained title
  artwork in each run.
- Three marked 10-minute gameplay soaks after scripted Cross input: each timed
  out cleanly at 600 seconds with no fatal, GPU, or unimplemented errors.
- NGS2 sampler tests and shader configuration tests pass.
- Input-driven probes `audio-probe-order` and `audio-probe-reverb` captured
  nonzero NGS2 and SDL samples after startup (SDL RMS 0.106–0.283, peaks up to
  0.56). Title-only probes remain silent. The opt-in harness now reports these
  streams separately. The host was also muted at zero output volume; it has now
  been restored to 50% and unmuted.
- macOS default output is HyperX Cloud Stinger 2 Wireless. Kyty now supports
  selecting `MacBook Air Speakers` explicitly and logs the resolved device,
  paused state, queued bytes, and converted-byte availability; both tested
  devices were active and draining.
- The three gameplay gate runs explicitly selected MacBook Air Speakers and
  recorded nonzero SDL samples in `1949/3368`, `2071/3357`, and `2131/3366`
  telemetry samples. Physical audibility still cannot be verified by logs alone.
- AudioOut2 now releases its state mutex before the potentially blocking SDL
  pacing call. The clean gameplay movement route exercises the same runtime
  while keeping frame readbacks disabled; audio audibility remains a physical
  speaker check rather than a telemetry question.
- Three focused input probes now measure host-to-guest-poll,
  host-to-observed-button, and host-to-next-present timing. Two runs started
  after initialization and stayed below 31 ms host-to-present maximum; the
  first still overlapped startup and reached 98 ms. The harness reports the
  count of samples over 40, 100, and 500 ms.
- Added a bootstrap route that sends bounded Cross presses to enter the game,
  waits for initialization, and then measures Arrow-Left movement. In a clean
  run, movement was observed at `7.35/17.59/24.30` ms median/p95/max, and the
  next-present maximum was `16.64` ms. A separate fixed-point screenshot run
  reached the gameplay “Wake up” scene, confirming the route is no longer
  measuring the splash screen.
- The deterministic input probe reproduces roughly one-second host-to-poll and
  host-to-present samples when the probe fires before the game has begun its
  first pad poll. The exact state trace measured about 1.07 s to the guest
  observing the button and to the next present; the queued transitions are
  then drained in order. This identifies an initialization/render-thread stall
  rather than a lost host event. Sampling shows Metal residency work around
  `vkQueueSubmit`; shader compilation begins after the first pad poll, so a
  shader-prewarm fix is not yet justified. Visible-state response remains
  unmeasured.
- Frame pacing probe: median 16.91 ms / 59.12 FPS, p95 33.37 ms; startup guest
  initialization produced a 2.96 s outlier.
- Three Immediate-present probes measured 60.31–60.71 FPS median and 19.83–20.34
  ms p95. Startup initialization gaps remained 2.73–3.21 s. The clean Release build now
  loads and saves the PPSA22520 Vulkan pipeline cache.
- The harness now defaults shader logs to Silent. After one expected cache
  invalidation on the new revision, two cache-loaded Silent runs measured
  60.17–60.44 FPS median with 2.93–3.60 s max gaps, confirming the startup
  stall is not caused primarily by diagnostic file dumping.
- `harness/fps.py` now separates the first 120 startup intervals from sustained
  pacing. Existing runs show warm medians around 60–62 FPS, warm p95 frame times
  around 20–22 ms, and warm maxima below 25 ms; the multi-second gap is reported
  separately as startup.
- Opt-in pipeline timing measured roughly 39 ms for first compute program
  materialization and 2.4 ms for direct compute pipeline creation, versus a
  roughly 3 s present gap. The remaining startup cost is downstream of those
  Kyty-side stages and has not been changed speculatively.
- New opt-in guest-GPU timing measured a 724 ms maximum processing slice and a
  roughly 456 ms early Vulkan submission; later slices stayed below 60 ms. The
  full 3.4 s startup gap therefore spans more than one queue submission.
- Direct-memory timing measured 333 ms total across 1,172 operations, with a
  15.96 ms maximum mapping; memory setup is not the primary 3-second stall.
- Stage 3 is stable across three runs. Fixed-point gameplay screenshot comparison
  remains the Stage 4 gate.
- Three fixed-point visual probes consistently rendered the gameplay “Wake up”
  scene. Downscaled pairwise mean absolute differences were 0.742–0.851, but
  no independent PS5/reference image is available, so Stage 4 remains pending.
- The visual comparison is now reproducible with
  `python3 harness/image_diff.py <reference.png> <candidate.png> --stride 8`.
- The staged groups are machine-checked with `harness/acceptance.py`: gameplay
  Stage 3 and the three visual Stage 2 probes both pass their three-run checks.
- Metal's built-in HUD was enabled without code changes. Kyty logged the HUD
  metric registrations and a clean 60.03 FPS median run; the headless session
  could not capture the overlay visually.

## Kept fixes

- Apple-only ordinary descriptor sets for `PPSA22520`.
- NGS2 voice channel query, reset-block handling, stereo reverb setup, graph render
  ordering, custom-mastering output, and mastering gain/writeback.
- Opt-in frame timing, audio signal, and input-to-present telemetry.
- Apple/PPSA22520 title-gated Immediate presentation when Mailbox is otherwise
  selected; `KYTY_PPSA22520_MAILBOX` retains Mailbox for comparison.
- The virtual-vblank default remains 60 Hz. A three-run 120 Hz comparison did
  not consistently improve movement latency and retained multi-second startup
  stalls, so it remains an opt-in harness setting only.
- A legal two-image swapchain comparison also matched the three-image default
  within run-to-run noise and retained the startup gap; the default buffering
  remains unchanged.
- Startup profiling attributes the repeated 2.88–2.99 s gap to first-use
  MoltenVK/Metal command encoding and timeline synchronization; no unsafe
  synchronization workaround was retained.
- MoltenVK command-buffer prefill was tested in three environment-only runs and
  rejected because it did not reduce the startup gap.
- Disabling MoltenVK Metal argument buffers was also rejected: it preserved title
  pixels but worsened gameplay latency tails, including 394 ms host-to-present
  and 550 ms guest-poll samples.
- A native arm64 build probe was configured but deferred because the host fault
  bridge and x64 instruction-emulator context are both x86-specific; the
  validated target remains x86-64 under Rosetta.
- An opt-in PM4 submission split was tested and reverted because three runs did
  not improve the startup gap consistently.
- Omitting the shared Vulkan pipeline cache was tested against the upstream
  MoltenVK serialization hypothesis and reverted because it did not improve
  startup and worsened one warm gameplay latency tail to 46 ms.
- An isolated upstream MoltenVK #2863 patched dylib was also tested and rejected
  after a 5.12 s startup regression in one of three runs.
- Asynchronous MoltenVK queue submission was tested and rejected because it did
  not improve startup or warm gameplay latency.
- MoltenVK `MVK_CONFIG_USE_MTLHEAP=0` was tested as an environment-only A/B and
  rejected after one clean probe regressed host-to-present to 2.4 seconds,
  host-to-observed-button to 4.1 seconds, and startup max to 3.8 seconds.

## Remaining work

- Measure visible-state response and reduce the title-to-gameplay warm-up hitch;
  deterministic host-to-guest-poll and host-to-present timing is now available.
- Confirm the user can hear output after the host mute was cleared; telemetry
  already proves the emulator reaches the selected playback device.
- Compare fixed-point screenshots against a reference and remove major visual defects.
- Reduce the startup guest/host initialization hitch and measure sustained gameplay performance.
- Resolve the remaining 108 load-time imports only when a call trace proves they matter.

# Silent Hill: The Short Message (PPSA10112) — bring-up report — 2026-10-10

Stage reached: **1 not met** (does not render), but the failure point moved from a fixable emulator
bug to a host capability limit. Two blockers fixed, one remains and is not an emulator edit.

## Before / after

| | Before | After |
|---|---|---|
| Exit | 65 at ~3.5 s | 65 at ~3-5 s |
| First fatal | `shaders.cpp:632` — MSL "Attempting to alias same binding" | `pipelineCache.cpp:625` — `mesh_shader_enabled` |
| Shaders compiled | 0 (first compute shader failed) | VS 4 / PS 3 / CS 16 |
| Frames | 11 sampled, all black | 11 sampled, all black |

Verification: 3 consecutive runs each reach the same mesh-shader line. Animal Well regression:
3 × 45 s runs, stage 2, 41–43 content frames each, unchanged.

## Kept changes

1. `5f8b23d4` — Apple: emit a single 32-bit storage-buffer view, emulate sub-word access by
   shift/mask. Fixes Metal's alias rejection at DescriptorSet 0 / Binding 0.
2. `652d4247` — extend the Apple vertex-subgroup fallback to PPSA10112 (vertex shader read
   `SubgroupLocalInvocationId`, which Metal vertex functions cannot expose). Small, per-title.

Both are gated to Apple (and the subgroup one per-title), so they cannot affect other platforms,
and neither regresses Animal Well.

## Reverted / not attempted

Nothing was reverted. A pipeline-creation "log and skip" fallback was considered but not needed
once blocker 1 was fixed properly.

## Remaining issue (blocking, host capability)

Mesh shaders. MoltenVK 1.4.2 (the only x86-64 Vulkan driver here) has no `VK_EXT_mesh_shader`;
KosmicKrisp (which does) is arm64-only; Kyty executes PS5 x86-64 guest code natively and cannot run
as arm64. So PPSA10112 cannot progress past its NGG/mesh geometry on this machine. Full evidence and
the three possible unblock paths are in `harness/BLOCKERS.md`.
