# Animal Well on KytyPS5 — current report

Status: gameplay is stable for the measured 10-minute soak; visual correctness,
audio confirmation on the user's host output, and input-latency characterization
remain in progress.

## Verified

- Build: x86-64 Kyty under Rosetta 2, Apple Silicon macOS, MoltenVK 1.4.2.
- Three clean 20-second descriptor-set runs: 17/27 sampled frames contained title
  artwork in each run.
- One 10-minute gameplay soak after scripted Cross input: timed out cleanly at
  600 seconds with no fatal, GPU, or unimplemented errors.
- NGS2 sampler tests and shader configuration tests pass.
- Audio telemetry has captured nonzero NGS2 and SDL samples in a successful route
  probe (SDL RMS 0.106–0.283, peaks up to 0.56). The current opt-in probe reports
  exact signal and queue levels.
- Frame pacing probe: median 16.91 ms / 59.12 FPS, p95 33.37 ms; startup shader
  compilation produced a 2.96 s outlier.

## Kept fixes

- Apple-only ordinary descriptor sets for `PPSA22520`.
- NGS2 voice channel query, reset-block handling, stereo reverb setup, graph render
  ordering, custom-mastering output, and mastering gain/writeback.
- Opt-in frame timing, audio signal, and input-to-present telemetry.

## Remaining work

- Capture matched input-to-present samples with the emulator window focused.
- Confirm audible output on the user's selected macOS playback device.
- Compare fixed-point screenshots against a reference and remove major visual defects.
- Reduce startup shader stutter and measure sustained gameplay performance.
- Resolve the remaining 108 load-time imports only when a call trace proves they matter.
