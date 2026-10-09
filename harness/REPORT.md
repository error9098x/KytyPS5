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
  streams separately; speaker audibility is still unconfirmed.
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
- Stage 3 is stable across three runs. Fixed-point gameplay screenshot comparison
  remains the Stage 4 gate.
- Three fixed-point visual probes consistently rendered the gameplay “Wake up”
  scene. Downscaled pairwise mean absolute differences were 0.742–0.851, but
  no independent PS5/reference image is available, so Stage 4 remains pending.
- The visual comparison is now reproducible with
  `python3 harness/image_diff.py <reference.png> <candidate.png> --stride 8`.

## Kept fixes

- Apple-only ordinary descriptor sets for `PPSA22520`.
- NGS2 voice channel query, reset-block handling, stereo reverb setup, graph render
  ordering, custom-mastering output, and mastering gain/writeback.
- Opt-in frame timing, audio signal, and input-to-present telemetry.

## Remaining work

- Measure visible-state response with a focused keyboard or SDL controller route;
  host-to-guest-poll and host-to-present timing are now measured, but they do not
  explain the user’s reported one-second cases.
- Confirm audible output on the user's selected macOS playback device.
- Compare fixed-point screenshots against a reference and remove major visual defects.
- Reduce the startup guest/host initialization hitch and measure sustained gameplay performance.
- Resolve the remaining 108 load-time imports only when a call trace proves they matter.
