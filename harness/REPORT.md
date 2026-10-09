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
  pacing call; a matched latency run is still needed to quantify the effect.
- Three focused input probes now measure both host-to-guest-poll and
  host-to-next-present timing. The worst observed p95 was 118.49 ms, with no
  one-second sample; visible-state response remains unmeasured.
- The deterministic input probe reproduces roughly one-second host-to-poll and
  host-to-present samples only during the title-to-gameplay transition, where
  first-use shader compilation blocks input polling and presentation. Three warm
  immediate-present runs measured host-to-present p95 of 22.68–27.34 ms and
  maxima of 28.62–35.00 ms. Visible-state response is still unmeasured, so this
  does not by itself prove perceptual input lag; the transition hitch remains
  open for a prewarm or asynchronous shader-compilation fix.
- Frame pacing probe: median 16.91 ms / 59.12 FPS, p95 33.37 ms; startup guest
  initialization produced a 2.96 s outlier.
- Three Immediate-present probes measured 60.31–60.71 FPS median and 19.83–20.34
  ms p95. Startup initialization gaps remained 2.73–3.21 s. The clean Release build now
  loads and saves the PPSA22520 Vulkan pipeline cache.
- The harness now defaults shader logs to Silent. After one expected cache
  invalidation on the new revision, two cache-loaded Silent runs measured
  60.17–60.44 FPS median with 2.93–3.60 s max gaps, confirming the startup
  stall is not caused primarily by diagnostic file dumping.
- Opt-in pipeline timing measured roughly 39 ms for first compute program
  materialization and 2.4 ms for direct compute pipeline creation, versus a
  roughly 3 s present gap. The remaining startup cost is downstream of those
  Kyty-side stages and has not been changed speculatively.
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

## Kept fixes

- Apple-only ordinary descriptor sets for `PPSA22520`.
- NGS2 voice channel query, reset-block handling, stereo reverb setup, graph render
  ordering, custom-mastering output, and mastering gain/writeback.
- Opt-in frame timing, audio signal, and input-to-present telemetry.
- Apple/PPSA22520 title-gated Immediate presentation when Mailbox is otherwise
  selected; `KYTY_PPSA22520_MAILBOX` retains Mailbox for comparison.
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

## Remaining work

- Measure visible-state response and reduce the title-to-gameplay warm-up hitch;
  deterministic host-to-guest-poll and host-to-present timing is now available.
- Confirm the user can hear output after the host mute was cleared; telemetry
  already proves the emulator reaches the selected playback device.
- Compare fixed-point screenshots against a reference and remove major visual defects.
- Reduce the startup guest/host initialization hitch and measure sustained gameplay performance.
- Resolve the remaining 108 load-time imports only when a call trace proves they matter.
