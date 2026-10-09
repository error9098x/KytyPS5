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
- Input-driven probes `audio-probe-order` and `audio-probe-reverb` captured
  nonzero NGS2 and SDL samples after startup (SDL RMS 0.106–0.283, peaks up to
  0.56). Title-only probes remain silent. The opt-in harness now reports these
  streams separately; speaker audibility is still unconfirmed.
- macOS default output is HyperX Cloud Stinger 2 Wireless. Kyty now supports
  selecting `MacBook Air Speakers` explicitly and logs the resolved device,
  paused state, queued bytes, and converted-byte availability; both tested
  devices were active and draining.
- AudioOut2 now releases its state mutex before the potentially blocking SDL
  pacing call; a matched latency run is still needed to quantify the effect.
- Frame pacing probe: median 16.91 ms / 59.12 FPS, p95 33.37 ms; startup shader
  compilation produced a 2.96 s outlier.
- Three Immediate-present probes measured 60.31–60.71 FPS median and 19.83–20.34
  ms p95. Startup spikes remained 2.73–3.21 s. The clean Release build now
  loads and saves the PPSA22520 Vulkan pipeline cache.

## Kept fixes

- Apple-only ordinary descriptor sets for `PPSA22520`.
- NGS2 voice channel query, reset-block handling, stereo reverb setup, graph render
  ordering, custom-mastering output, and mastering gain/writeback.
- Opt-in frame timing, audio signal, and input-to-present telemetry.

## Remaining work

- Capture matched input-to-present samples with a focused keyboard or SDL
  controller route; automated runs with no delivered input remain unmeasured.
- Run three Immediate-present pacing probes, then compare them with the default
  Mailbox baseline.
- Mark a controlled gameplay soak with `KYTY_ROUTE=gameplay` so the per-run
  stage tracker does not infer gameplay from incidental startup input.
- Confirm audible output on the user's selected macOS playback device.
- Compare fixed-point screenshots against a reference and remove major visual defects.
- Reduce startup shader stutter and measure sustained gameplay performance.
- Resolve the remaining 108 load-time imports only when a call trace proves they matter.
