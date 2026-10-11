# Run log

## Baseline 1 — 2026-10-09

- Build: x86-64 Release, KytyPS5 `4c8fc53`, no launcher
- Command: `kyty_emulator --game <Animal Well directory>`
- Result: exit code 65 after 3.5 s, before game loading or window creation
- First blocker: `Failed to load Vulkan Portability library`
- Stage: 0 (no first frame)
- Evidence: `harness/baseline-run-1.log`

The failure is host setup: the x86-64 Rosetta build needs an x86-64 MoltenVK/Vulkan
library. No game-specific diagnosis is valid until this is resolved.

## Apple Vulkan capability fallback — 2026-10-09

- Three runs reached Vulkan device creation and then failed identically on
  `unsupported mastering control: 0x30000004`.
- Public ABI/disassembly identified the command as NGS2 mastering gain
  (`fbw_level`, `lfe_level`). Added gain handling and registered
  `sceNgs2RackGetInfo`.
- Three follow-up runs reached the first vertex pipeline but failed identically
  in MoltenVK: subgroup builtins are unavailable in vertex functions.
- Registered and implemented `sceNgs2VoiceGetPortInfo`; unresolved import count
  fell from 9 to 0.

## PPSA22520 Apple vertex fallback — 2026-10-09

- Added a game-specific Apple fallback that substitutes `gl_VertexIndex` for
  `gl_SubgroupInvocationID` in vertex/local shader stages.
- Three 30-second runs stayed alive after VS/PS pipeline creation with no logged
  unresolved imports, GPU errors, or fatal error. They are provisionally stage 1;
  screenshots are still required to confirm the first frame and visual fidelity.
- `ngs2_sampler` and `shader_cfg` CTest tests pass.

## Frame readback diagnostics — 2026-10-09

- Added `KYTY_FRAME_STATS=1` / `KYTY_FRAME_DUMP_DIR=<dir>` (swapchain.cpp, diagnostics only).
  Samples presents 1-10 and every 60th, reads the presented frame back, writes PNGs.
- Run `frames-1` (45 s): >=540 presents. Frame 1 is a 1280x720 blank; every later
  frame is 3840x2160 and **100% black (max channel 0)** in all 20 sampled frames.
- Conclusion: the game IS running and presenting 4K frames, but the rendered
  content is black. Stage 1 (first frame) is reached; stage 2 (title) is not.
- Next: A/B test the PPSA22520 vertex-subgroup fallback, then inspect draws/clip guard.
- Note: `LOGF` FrameStats lines did not land in kyty.log (log routing); PNGs are the source of truth.

## Harness fix: guest log was not scanned — 2026-10-09

- `LOGF` output goes to `guest-printf.log` (`--printf-direction File`), not `kyty.log`.
  `scan_log.py`/`stage.py` only read `kyty.log`, so FrameStats and **unresolved imports were invisible**.
- **Correction:** earlier entries claiming "0 unresolved imports" only covered kyty.log.
  Scanning guest-printf.log (run `flow-check`) shows **109 unresolved PLT imports patched to stubs**:
  Agc_v1 77, AgcDriver_v1 22, libkernel_v1 8, Ngs2_v1 1, LibcInternalExt_v1 1.
- 38 sampled frames, all black. Leading hypothesis for the black output is now the
  missing AGC (PS5 GPU command) API, not only the vertex fallback. A/B the fallback second.
- Harness now: stats always on; PNG dumps opt-in (`KYTY_FRAME_DUMP=1`, ~25 MB/4K frame);
  default MoltenVK path auto-set; stage line reports `content=yes|NO (all black)`.

## Draw-path diagnostics — 2026-10-09

- `command-stats-1` records many `draw_auto` commands and compute dispatches; the
  game is submitting real graphics work.
- `draw-debug-1` shows successful `BeginRendering`, `DrawComplete`, and pipeline
  creation for the 4K video buffer at `0x0200450000`; sampled frames remain black.
- No `Unresolved import stub called` entries were observed. The 109 unresolved
  entries are load-time relocations only in these runs.
- The lane-zero A/B (`lane-zero-1`) also produced 19/19 all-black samples, so
  changing the vertex fallback lane value did not improve pixels.

## Pixel/resource A/B — 2026-10-09

- `no-pixel-kill-1` produced 33/33 all-black samples; disabling the PS pixel-kill
  flag is not the cause.
- `resource-debug-postbuild` shows the final 4K pass samples internal 320x180
  images (for example `0x0212a70000`) and completes successfully. The black
  result is upstream in the internal render-texture chain; a separate resource
  dump records the sampled image descriptors.

## Internal target readback — 2026-10-09

- `internal-readback-1` read back producing color targets after draws without
  writing guest memory. Most 8-bit intermediate targets were all-black, but
  several later targets reached 100% non-black pixels.
- The final 3840x2160 video target remained `rgb_nonblack=0.0000%` and
  `alpha_avg=0.00`, despite successful draw completion. This narrows the issue
  to the final compositing chain or its source texture state.
- Added `KYTY_INTERNAL_FRAME_ADDRESS` filtering for targeted follow-up runs.
- Added NGS2 mastering/rack/port regression coverage; `ngs2_sampler` passes.

## MoltenVK descriptor-set fix — 2026-10-10

- MoltenVK push descriptors were the remaining draw-path fault. With the original
  push-descriptor path, presented frames stayed black and GPU readback showed the
  vertex shader's storage-buffer constants as zero even though guest memory held
  `320.0, 180.0` and `1024, 2048`.
- Forcing ordinary descriptor sets for title `PPSA22520` makes the original shaders
  read their resources correctly. A 30-second run produced 29 sampled frames, 19
  with content; the captured 3840x2160 frame shows Animal Well's CRT title artwork.
- The workaround is title-gated in `pipeline/shaders.cpp`; no global push-descriptor
  behavior changes. The remaining 10 black samples occur during startup/transition.

## Gameplay audio progression — 2026-10-10

- Sending the mapped Cross key (`J`) advances beyond the title artwork and exercises
  streamed NGS2 sampler audio.
- The first soak exposed a normal `flags=0x4` block reset while the voice had stopped
  accepting new blocks. Kyty rejected that reset before clearing the queue.
- Allowing reset blocks in that state removes the fatal error. A 115-second run with
  Cross input timed out cleanly (`exit=0`), with no unimplemented or GPU errors.

## Audio signal and graph fix — 2026-10-10

- `KYTY_AUDIO_STATS=1` measured the complete path: SDL opened 48 kHz streams and
  queued blocks, but both NGS2 output and SDL RMS were exactly zero.
- The missing NGS2 `sceNgs2VoiceQueryInfo` export was implemented for info `0x4001`
  (8-byte channel info), resolving the repeated NGS2 stub calls.
- The NGS2 render graph now initializes the game's stereo reverb node, renders
  non-master voices before mastering sinks, and emits custom-mastering voices.
- Input-driven probes `audio-probe-order` and `audio-probe-reverb` recorded
  nonzero NGS2 and SDL levels after startup (SDL RMS `0.106`–`0.283`, peaks up
  to `0.56`). Title-only probes remain silent. Audio telemetry remains opt-in,
  and the harness now reports both signal streams as `[audio]`.
- Restored the full NGS2 PCM output-mix/writeback path that had been lost during
  an earlier diagnostic edit; this is required for the nonzero SDL samples.

## Input latency telemetry — 2026-10-10

- `KYTY_INPUT_STATS=1` timestamps host key events with the same monotonic clock used
  by `FrameStats`; `harness/latency.py` reports next-present median, p95, and max.
- The harness now appends a `[latency]` line per run. A focused window and at least
  one input event are required for matched samples; missing matches are reported
  explicitly rather than treated as zero latency.
- `KYTY_FRAME_TIMING=1` adds per-present timestamps without frame readbacks;
  `harness/fps.py` reports pacing. A 10-second probe measured median 16.91 ms
  (59.12 FPS), p95 33.37 ms, with a 2.96 s startup initialization outlier.
- `harness/fps.py` now also prints a fixed 120-interval startup window and a
  separate warm pacing summary. Existing cache-loaded runs report warm medians
  around 60–62 FPS, warm p95 frame times around 20–22 ms, and warm maxima below
  25 ms.
- Controller buttons, controller axes, and mouse buttons now use the same
  monotonic `InputStats` clock. A follow-up automated keyboard probe produced
  no input events, so it correctly reported no matched samples rather than
  inventing a latency value; repeat with a focused SDL controller route.

## Audio queue contention — 2026-10-10

- AudioOut2 previously held its global state mutex while `AudioOutOutputs`
  paced SDL audio. PCM, gains, and object-mix data are now snapshotted under
  the mutex and the backend call runs after unlock. This is a static contention
  fix; a matched input latency run is still required to quantify its effect.

## Playback-device diagnosis — 2026-10-10

- macOS reports **HyperX Cloud Stinger 2 Wireless** as the default output;
  MacBook Air Speakers are available but not default. Kyty therefore sent its
  valid PCM stream to the HyperX device when no device was specified.
- Added `--audio-device <exact SDL name>` and `KYTY_AUDIO_DEVICE` harness
  support. A probe selecting `MacBook Air Speakers` resolved SDL device 29 with
  `paused=0`; the default probe resolved the HyperX device with `paused=0`.
  Queue and converted-byte availability drain in both cases. This explains a
  likely host-routing cause of “no audio” without changing the NGS2 mix.

## Harness stage and pacing — 2026-10-10

- `stage.py` now uses `FrameTiming` when readbacks are disabled and distinguishes
  first presentation, non-black content, and a long run with nonzero gameplay
  audio/input evidence. Zero-valued startup telemetry no longer counts as
  gameplay.
- `KYTY_PRESENT_MODE=Immediate` passes `--present-mode Immediate` for an
  uncapped pacing probe. Animal Well on Apple now uses Immediate when Mailbox
  is configured; other titles retain their configured mode.
- Controlled soak runs can set `KYTY_ROUTE=gameplay`; `run.json` preserves that
  marker so Stage 3 is never awarded from a long title-only run by accident.
- Three Immediate-present probes completed without crashes: median FPS
  `60.71`, `60.42`, and `60.31`; p95 frame times were `20.34`, `20.32`, and
  `19.83` ms. Each retained a single startup initialization gap between `2.73` and
  `3.21` seconds.
- A clean Release build now loads `_PipelineCache/PPSA22520.bin` (1,144,892
  bytes) on all three follow-up probes. The cache is active, but it does not
  remove the initial shader-compilation spike.
- The harness now defaults shader logging to `Silent`; set
  `KYTY_SHADER_LOG_DIRECTION=File` for full IR dumps. A clean rebuild first
  invalidated the driver cache; the two subsequent cache-loaded Silent probes
  measured 60.17–60.44 FPS median with 2.93–3.60 s max gaps. Suppressing
  diagnostic dumps does not remove the first-use MoltenVK/scene stall.
- `KYTY_PRINTF_DIRECTION=Silent` is now supported for performance probes and
  still produces a normal scanner summary with empty guest-log diagnostics.

## Title-gated low-latency presentation — 2026-10-10

- Before the change, the default Mailbox path measured 59.12 FPS median with
  33.37 ms p95; explicit Immediate probes measured 60.31–60.71 FPS median with
  19.83–20.34 ms p95.
- Kyty now selects Immediate automatically for `PPSA22520` on Apple when the
  user leaves the mode at Mailbox. Other titles and platforms keep their
  configured mode. Set `KYTY_PPSA22520_MAILBOX` (presence is enough; its value
  is not parsed) to retain Mailbox for a comparison.
- Three default-mode runs after the change resolved the title gate and measured
  host-to-present median/p95/max of `9.28/27.50/36.13 ms`,
  `10.58/23.44/34.71 ms`, and `9.37/25.70/36.00 ms`. Each timed out cleanly,
  with no fatal, GPU, or unimplemented errors and nonzero gameplay audio.

## Startup pipeline timing — 2026-10-10

- Added opt-in `KYTY_PIPELINE_TIMING=1` logs for shader-program materialization
  and Vulkan pipeline creation. The first compute program materialized in about
  39 ms and its Vulkan pipeline creation took about 2.4 ms, while the associated
  frame gap was about 3 seconds.
- This rules out the Kyty IR compiler and the direct `vkCreateComputePipelines`
  call as the whole stall. The remaining cost is likely MoltenVK/Metal first-use
  compilation or queue synchronization; no behavior change was kept for this
  diagnostic.

## Startup memory timing — 2026-10-10

- `KYTY_MEMORY_TIMING=1` measured 1,172 direct-memory operations during an
  8-second probe: 586 allocations and 586 named mappings, totaling 333 ms;
  the slowest mapping was 15.96 ms. Direct-memory setup is therefore not the
  primary source of the roughly 3-second first-use gap.

## Startup host-GPU sample — 2026-10-10

- A macOS `sample` captured during the exact 2–4 second startup gap. The active
  Kyty graphics thread was in `vkQueueSubmit` → MoltenVK command encoding,
  including `MVKCommandEncoder::beginRendering`, `MVKCmdClearImage`, and
  `MVKCmdDraw`, while a companion thread waited in `vkWaitSemaphores` on a
  MoltenVK timeline semaphore. The guest execution thread remained in the
  normal `RuntimeLinker::Execute` path.
- `KYTY_SUSPEND_TIMING=1` measured the first `AgcSuspendPoint` wait at `25.544 ms`
  and subsequent waits at roughly `8–18 ms`; suspend-point semaphore waiting is
  not the source of the full `2.88–2.99 s` startup gap.
- No behavior change was retained from this sample. The remaining startup
  blocker is host-side first-use Metal/MoltenVK command encoding and GPU
  synchronization; changing guest barriers without a correctness proof would
  risk rendering and gameplay stability.

## Native Apple Silicon build probe — 2026-10-10

- Configured a separate `arm64` Release/no-Qt build successfully, but the
  emulator target stops before linking in `src/common/hostException.cpp`: the
  existing Apple signal bridge reads x86-only `__rip`, `__err`, and `__rXX`
  members from an ARM context.
- The same portability boundary appears in `loader/x64InstructionEmulator.cpp`,
  which directly decodes and mutates x86-64 registers in the saved context. A
  correct native build therefore needs an ARM context adapter plus a deliberate
  host-side x64 instruction-emulation strategy; a partial exception-field patch
  would not produce a runnable emulator. The validated x86-64/Rosetta build is
  retained for Animal Well.
- Tested MoltenVK's documented `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=1`
  as an environment-only A/B change. Three runs measured max gaps of `2.929`,
  `2.967`, and `2.871 s` with median FPS `60.17`, `60.41`, and `60.44`, which
  is indistinguishable from the default path. The setting was rejected and is
  not enabled by Kyty.
- Tested MoltenVK's documented `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` as a
  second environment-only A/B. Three title probes remained visually valid and
  showed 2.62–2.87 s max gaps, but the matched gameplay probes produced a
  394 ms host-to-present and 550 ms guest-poll outlier in one run. It was
  rejected and the default argument-buffer path remains enabled for gameplay.

## PM4 submission split experiment — 2026-10-10

- Tested an opt-in PM4 flush interval of 2,048 packets to split the first large
  guest command batch before MoltenVK submission. Three title probes measured
  max frame gaps of `3.228`, `2.735`, and `2.918 s`, with no consistent gain over
  the default path. The diagnostic code was reverted; no production behavior
  changed.

## Shared Vulkan pipeline-cache experiment — 2026-10-10

- Upstream [MoltenVK issue #2863](https://github.com/KhronosGroup/MoltenVK/issues/2863)
  reports that a shared `VkPipelineCache` can
  serialize SPIR-V-to-MSL conversion and Metal compilation. Kyty therefore
  tested an opt-in path that passes no driver cache to pipeline creation.
- Three title probes measured max gaps of `2.791`, `3.037`, and `2.918 s`, with
  no consistent startup improvement. Three warm gameplay probes measured
  host-to-present maxima of `46.34`, `35.97`, and `30.53 ms`, including a worse
  tail than the default path. The switch was rejected and reverted; Kyty keeps
  its active cache for the validated behavior.

## Isolated MoltenVK #2863 build — 2026-10-10

- Built an isolated universal MoltenVK dylib from upstream with the #2863
  per-shader pipeline-cache locking patch:
  [KhronosGroup/MoltenVK#2863](https://github.com/KhronosGroup/MoltenVK/issues/2863).
- Three Kyty startup probes using only `SDL_VULKAN_LIBRARY` redirection measured
  max gaps of `5.117`, `2.924`, and `2.958 s`. The first run regressed badly and
  the other two were indistinguishable from the bundled 1.4.2 path. The tested
  artifact was `_Build/MoltenVK-upstream/Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib`,
  built from upstream commit `67b2682699d7903606b97a0392061d85d27d49e6`,
  SHA-256 `ab9893f6aa1e6de5f02aea09c572aec26f2c10878ed3f4efa70ed0a044581929`.
  The custom dylib is not selected by the harness; the bundled runtime remains
  validated.

## MoltenVK asynchronous submission experiment — 2026-10-10

- Tested the documented `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` setting as an
  environment-only change. Three title probes showed no startup improvement,
  and three warm gameplay probes measured host-to-present maxima of `34.71`,
  `34.41`, and `38.78 ms`, with no gain over the default synchronous path.
  The setting was rejected; the default remains enabled.

## Gameplay stability gate — 2026-10-10

- `gameplay-gate-3-20261010`, `gameplay-gate-4-20261010`, and
  `gameplay-gate-5-20261010` each ran for 600 seconds with route marker
  `gameplay`, exit code 0, and no fatal, unimplemented, or GPU-validation
  errors.
- MacBook Air Speakers were selected explicitly. Nonzero SDL/NGS2 samples were
  observed in all three runs: SDL `1949/3368`, `2071/3357`, and `2131/3366`;
  NGS2 `648/1123`, `689/1119`, and `709/1122`.
- `harness/acceptance.py --stage 3` reports `PASS` for all three gameplay runs;
  the same check reports `PASS` for the three visual probe runs at stage 2.

## Visual consistency probes — 2026-10-10

- `visual-gate-1/2/3-20261010` captured fixed frame `000600` after the same
  short Cross-input route. All three runs produced non-black gameplay content
  (12, 12, and 13 content samples respectively) and the frame shows the player
  character with the “Wake up” prompt.
- At a 320x180 comparison scale, pairwise frame-000600 mean absolute channel
  differences were `0.851`, `0.775`, and `0.742`; more-than-four-level channel
  changes covered `3.76%`, `3.83%`, and `3.16%`. This establishes repeatable
  rendering, but an independent PS5/reference capture is still needed to claim
  visual correctness.
- Added `harness/image_diff.py`, a dependency-free RGB/RGBA PNG comparator. The
  same frame pairs report stride-8 MAE `0.902` and `0.829`, with changed-pixel
  fractions `4.87%` and `5.07%`.

## Full input-path latency — 2026-10-10

- Added `InputPoll` timestamps at the guest `PadRead`/`PadReadState` boundary.
- `InputPoll` now records the API (`PadRead` or `PadReadState`), returned count,
  button mask, and guest sample timestamp. `InputState` records the host-side
  button update, allowing the harness to measure when the guest actually sees a
  transition rather than only when it calls the pad API.
- Three warm probes measured host-to-next-present median/p95/max of
  `12.84/49.43/97.98`, `9.47/28.20/32.19`, and `8.33/27.23/30.91` ms after
  exact-state telemetry was added. The first still overlapped residual startup;
  the other two stayed below 31 ms. Guest-poll p95 values were `41.42`,
  `19.12`, and `19.29` ms. These runs support a warm steady-state baseline,
  but do not prove that every startup transition is free of hitches.
- The short startup probe reproduced roughly 0.8–1.1 s host-to-observed-button
  and host-to-present delays. The guest then drained the queued button
  transitions in order. The delay begins before the first pad poll; Metal
  residency work surrounds that initialization window, while shader compilation
  begins afterward. This points to an initialization/render-thread stall rather
  than the host input mutex dropping events.

## Deterministic input probe — 2026-10-10

- Added opt-in `KYTY_INPUT_PROBE=1`. It injects a keyboard press/release through
  the same `HostInputKey` path as a real SDL event, so latency probes do not
  depend on a focused window, a connected controller, or an unlocked desktop.
  `KYTY_INPUT_PROBE_KEY`, `KYTY_INPUT_PROBE_START_MS`,
  `KYTY_INPUT_PROBE_INTERVAL_MS`, and `KYTY_INPUT_PROBE_HOLD_MS` tune it; the
  default key is `J` and the first event is delayed by 2 seconds.
- One 30-second immediate-present run with the default start contained
  host-to-present samples of `1187.4`, `668.2`, and `232.2` ms. Matching
  guest-poll samples were `1006.0`, `671.1`, and `486.8` ms. Exact-state traces
  measured about `1.07` s to the guest observing the button and to the next
  present. The delay starts before the first pad poll; Metal residency work is
  active around `vkQueueSubmit`, while shader compilation begins afterward.
  The cause is therefore an initialization/render-thread stall, not proven
  shader compilation on the input path.

## MoltenVK residency A/B — 2026-10-10

- `MVK_CONFIG_USE_MTLHEAP=0` was tested against the default heap path. One
  clean 12-second probe regressed host-to-present median/p95/max to
  `318.6/2376.9/2394.4` ms, host-to-guest-poll to
  `306.1/2142.8/2160.3` ms, host-to-observed-button max to `4140.8` ms, and
  startup max to `3810` ms. The setting is rejected and was not retained.

## Blocked-submission retry experiment — 2026-10-10

- A diagnostic override was briefly added for the retry timeout used when all
  guest-GPU submission queues are blocked. Source review showed
  `Common::CondVar::WaitFor` takes microseconds, so the existing `100` value is
  0.1 ms rather than 100 ms. The hypothesis could not explain the observed
  stall; the change was reverted and no runtime behavior was retained.

## Gameplay input route — 2026-10-10

- The synthetic probe now supports a bounded Cross bootstrap followed by a
  separate movement key after a configurable warm-up. A fixed-point screenshot
  from the route reached the gameplay `Wake up` scene.
- With frame readbacks disabled, Arrow-Left movement transitions were observed
  at `7.35/17.59/24.30` ms median/p95/max. Their next-present maximum was
  `16.64` ms. The same run's roughly one-second samples were confined to the
  initial Cross bootstrap and first guest initialization.
- Readback-enabled visual runs are kept separate because frame dumps stall the
  GPU and inflate latency tails.

## Virtual-vblank frequency A/B — 2026-10-10

- Exposed `KYTY_VBLANK_FREQUENCY` through the harness and compared three 120 Hz
  runs with three 60 Hz runs using the gameplay bootstrap route.
- Arrow-Left guest-observation p95/max values at 120 Hz were
  `16.55/17.00`, `40.03/49.96`, and `15.03/42.46` ms. At 60 Hz they were
  `22.64/26.84`, `21.27/23.82`, and `17.41/19.22` ms. Startup stalls remained
  multi-second in both groups, so 120 Hz was not retained as a default.
- The 60 Hz virtual vblank preserves PS5 timing semantics; 120 Hz remains an
  opt-in diagnostic comparison through the harness.

## Metal Performance HUD — 2026-10-10

- Ran the unmodified harness with `MTL_HUD_ENABLED=1`,
  `MTL_HUD_ELEMENTS=fps`, and `MTL_HUD_ALIGNMENT=topright`.
- The Kyty log contains `[libMTLHud]` registrations for FPS, frame interval,
  present delay, and logical FPS metrics. The run exited cleanly and measured
  421 presented frames at a 60.03 FPS median.
- A desktop screenshot could not be captured in the headless Codex session, so
  the HUD's on-screen text is not visually verified here. No emulator code or
  synchronization setting was changed.

## Guest GPU submission timing — 2026-10-10

- Opt-in `KYTY_GPU_SUBMIT_TIMING=1` and `KYTY_GPU_PROCESS_TIMING=1` separated
  guest-GPU processing from presentation. The largest startup processing slice
  was `724,262` microseconds and the corresponding early queue submission was
  about `456` milliseconds. Later processing slices stayed below 60 ms.
- The frame timeline still contained a roughly 3.4 s gap, so the full startup
  stall is not one queue submission; guest initialization or a wait outside the
  GPU process remains. The timing hooks are diagnostic only and are disabled by
  default.

## Swapchain image-count A/B — 2026-10-10

- Added opt-in `KYTY_SWAPCHAIN_IMAGE_COUNT`, clamped to the Vulkan surface's
  legal minimum and maximum. The default remains three images; zero or one is
  not a valid request on this surface.
- Three default runs measured FPS medians `60.22`, `62.05`, and `60.76`, with
  maximum frame gaps `2857`, `2863`, and `2817` ms. Three legal two-image runs
  measured medians `60.23`, `61.38`, and `61.01`, with maximum gaps `2820`,
  `2833`, and `2848` ms. The two-image path does not improve startup or
  sustained pacing consistently and is not retained as a default.
- Three 35-second runs with the first probe event delayed to 10 seconds measured
  warm host-to-present median/p95/max of `8.94/22.68/28.62 ms`,
  `9.12/26.16/32.93 ms`, and `10.46/27.34/35.00 ms`. Guest-poll p95 was
  `19.19`, `18.64`, and `19.39 ms`; one run had a `225.54 ms` poll outlier.
  No warm one-second delay reproduced.

## Host audio check — 2026-10-10

- The host was set to `output volume: 0` and `output muted: true`, while the
  emulator telemetry showed nonzero NGS2 and SDL PCM on `MacBook Air Speakers`.
  This explains inaudible playback without indicating a new emulator-side drop.
- Restored the host output to 50% and unmuted it. The emulator still supports
  selecting the exact playback device with `--audio-device` or
  `KYTY_AUDIO_DEVICE` if the user wants the HyperX headset instead.

## Save backup — 2026-10-10

- Copied `_SaveData/` to the ignored local backup
  `harness/backups/animal-well-20261010/_SaveData/` before further gameplay
  testing. `MANIFEST.sha256` records both save-file hashes. No separate Kyty
  config file was present in the repository or the standard macOS application
  support location.

## Shader descriptor trace — 2026-10-09

- The first VS descriptor trace reports valid readable scalar buffers:
  - screen constants: `320.0`, `180.0`
  - texture constants: `1024`, `2048`, plus valid metadata
- Both descriptors are `type=0`, nonzero, 16-byte ranges. The constant-buffer
  materialization/offset hypothesis is rejected for this draw.

## Hypothesis check (no code changes) — 2026-10-09

- `flow-check` (30 s): **0** "Unresolved import stub called" lines. The 109 stubbed imports were
  patched at load but never called in that window, so the missing-AGC theory is NOT supported
  (a 1024-call log cap exists; the window was short). Demoted in BLOCKERS.md.
- The game does issue GPU work: many VS/PS pipelines created, 64 compute dispatches (CS 24576x1x1).
- Vertex shader `0002_new_shader_vs_cdcf88828e5c2df8` uses the lane ID only as
  `(0xFFFFFFFF >> (lane & 31)) & 1`, which is always 1, so the `gl_VertexIndex` fallback is
  behaviourally harmless for this shader. Theory demoted.
- ~~New leading suspect~~ (REJECTED by the descriptor trace above: scalar buffers are valid and readable): the shader reads position data from storage buffers with bounds checks
  that return 0 when out of range. All-zero positions trigger the "zero-position clip guard"
  (spirvEmitterFlow.cpp), which clips the primitive. Not yet tested.
- Next experiments (each with 3 runs, compare `[frames]`): (a) env-gated disable of the clip guard,
  (b) log first draws' vertex-buffer sizes/addresses and the shader's computed positions,
  (c) check pixel shader outputs and whether the render target feeds the presented buffer.

## Controller input-lag fix — 2026-10-10

- User reported ~1 s lag with a physical DS4 even after the "verified" low-latency config.
- Re-run with the controller connected: Arrow-Left host→guest median 430 ms, max 944 ms.
  Presentation hacks (hard sync, 1 pending flip, 2 swapchain images) did not help (median 813 ms).
- Root cause: `GameController::AddState()` queued every motion/stick packet; `scePadRead(num=1)`
  returned the oldest sample. Fixed by coalescing non-discrete updates (`56852717`).
- After fix: median 14.17 ms, max 18.68 ms; next present max 33.42 ms. See `INPUT_LAG_FIX.md`.

## Silent Hill: The Short Message bring-up — 2026-10-10

Two Apple/MoltenVK blockers cleared; the game now reaches mesh shaders.

- **Blocker A — aliased storage-buffer views (FIXED, `5f8b23d4`).** The first compute shader
  emitted both `%buffers_u8` and `%buffers` at DescriptorSet 0 / Binding 0 with `Aliased`.
  Metal's SPIR-V-Cross path rejects that. On Apple, Kyty now declares only the 32-bit view and
  emulates sub-word access through the existing shift/mask path. Before: exit 65 at ~3.5 s with
  `SPIR-V to MSL conversion error: Attempting to alias same binding`. After: the shader compiles.
- **Blocker B — vertex subgroup builtins (FIXED, `652d4247`).** A vertex shader read
  `SubgroupLocalInvocationId`, which Metal vertex functions cannot expose. The lane-id fallback
  (from the vertex index) was PPSA22520-only; the title check is now named and includes PPSA10112.
  That shader uses the id only in an always-1 lane mask.
- **Progress:** shaders compiled before stopping went 0 → VS 4 / PS 3 / CS 16, past engine config
  and into ATRAC9 audio decoder init.
- **Regression:** Animal Well, 3 × 45 s runs, stage 2, 41–43 content frames each, no change.
- **Blocker C — mesh shaders (OPEN, host limit).** `EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled)`
  at `pipelineCache.cpp:625`. This host's MoltenVK 1.4.2 does not expose `VK_EXT_mesh_shader`
  (absent from its documented extension list; upstream issue #2375 has been open since 2024-10).
  Kyty renders PS5 NGG geometry as Vulkan mesh shaders, so any PS5 3D title needs host mesh shaders.

## Silent Hill bring-up: mesh emulation, Rosetta finding — 2026-10-10

- Mesh shaders emulated through compute; first emulated draw encoded. Vertex ballot/shuffle handled
  with a one-lane subgroup mode. Push descriptors disabled on all Apple titles.
- GPU error bisect (`KYTY_MESH_EMU_SKIP`): none with both halves skipped, error with either half.
  Scratch size 16/64/256 MB and the newer MoltenVK build do not change it.
- Rosetta abort root-caused and reproduced in 40 lines (`harness/repro/rosetta_avx_straddle.c`):
  a 256-bit AVX store whose second half faults after the first half completed. Page tracking
  activity rises from 126 to 2,433 protections once BDA mode starts. Mitigations tried did not
  remove it; see `ROSETTA_AVX_PAGE_PROTECTION.md`.
- Animal Well: 3 of 3 runs clean after the shader and descriptor changes, 2 of 2 after the final
  fault-handler change (title content visible, about 60 FPS).
- Silent Hill: still no rendered frames. Not solved.

## 2026-10-11: post-Continue "guest null access" was a MoltenVK null descriptor set (fixed, `8ea17d3d`)

- The `runtimeLinker.cpp:756` report in `shtsm-descriptor-crosshold-1` was not guest code. The
  faulting PC (`0xff7bada77d`) lies in the x86-64 slice of `libMoltenVK.dylib` (load base
  `0xff7b9f3000`, slice offset `0xe777d`); `atos` resolves it to `bindMetalResources(...) + 909`
  (inlined `executeBindOps`). The instruction is `mov rdx, [rdx+0x10]` with `rdx = 0`:
  `common._descriptorSets[op.set]` was null when the graphics encoder bound resources.
- Cause: `ExecuteEmulatedMeshDraw` called `CommitBindings` for the graphics bind point only when
  `graphics_stages` was non-empty. Depth-only emulated mesh draws (no pixel shader) skipped it, but
  the descriptor-backed generated vertex shader still reads the scratch binding, so set 0 was never
  bound. The physical-BDA route needs no descriptor there, which is why only the descriptor route hit it.
- Fix: always commit graphics bindings on the descriptor route (`KYTY_MESH_SCRATCH_DESCRIPTOR=1` only;
  other routes and titles are unchanged).
- Verification: `shtsm-nullset-fix-1` (descriptor route, 256 MB, input probe start 25 s, interval 4 s,
  hold 250 ms) ran 86 s, past the previous 43 s crash point, with no MoltenVK fault.
- New first blocker: guest abort from Unreal `LowLevelFatalError`: "Ran out of memory allocating
  262144 bytes with alignment 65536", `AvailablePhysical: 65536`, `UsedPhysical: 13107068928`.
  At boot Unreal reserves the whole 12.5 GiB (`0x320000000`) direct-memory pool by halving requests
  (300 MB, then 6.25 GiB, 3.125 GiB, ... until failure), so this is Unreal's own sub-allocator running
  dry, not Kyty's pool or host RAM. Next step: find why demand exceeds the real-console budget
  (suspect deferred frees that wait on GPU fences/labels that complete late or never, plus the
  steady 64 KB batch-map page moves seen before the abort).
- Debug tip: to symbolize a host-side PC, subtract the module base and use
  `atos -arch x86_64 -o harness/tools/MoltenVK-1.4.2/libMoltenVK.dylib -l <base> <pc>`; the fat-file
  x86-64 slice starts at file offset 0x4000.

## 2026-10-11 (afternoon): Silent Hill OOM root-caused to unpaced AvPlayer video (fixed, `78019f9f`)

- Diagnostics committed: `06298f5e` (`KYTY_MEM_TRACE=1` batch-map accounting), `4095c468`
  (`KYTY_EOP_TRACE=1` EOP interrupt counters), `42f83087` (`KYTY_AVPLAYER_TRACE=1` video delivery),
  `21c3ffb4` (symbolized host frame-pointer backtrace in the unhandled-exception report, macOS).
- EOP trace (`shtsm-eoptrace-2`): requested = executed = delivered at all times,
  `no_registration=0`. EOP delivery was never broken. After ~35 s Kyty simply stops seeing any
  `release_mem` packets because the guest stops submitting GPU work.
- `sample` of the live process (`shtsm-sample-3`, ~55 s): RHIThread and AgcSubmissionThread idle in
  condition waits; **RenderThread 1 spins in guest 0x900e63d20**, called in a loop from
  0x900e63b50..0x900e63c01. That function is Unreal's media video-sample converter: per sample
  it creates two GPU buffers of `pitch*height` and `pitch/2*height` (NV12 Y/UV) through RHI
  vtable+0x590, i.e. the 64 KB batch maps seen in the memory trace. The caller drains a sample
  queue (`0x90574df20` = try-dequeue) and never empties it.
- AvPlayer trace (`shtsm-avtrace-1`): the title movie `shpc/content/movies/ui_titlemovie01.mp4`
  (378x150 H.264, 29.97 fps, 9.1 s, no audio) runs with `sceAvPlayerSetAvSyncMode(1 = None)` and
  looping on. Kyty delivered frames unpaced in sync-None mode: from ~24 s player clock the guest
  pulled ~1,000 frames/s and frame timestamps ran to 397 s at a 42 s clock. Unreal's render thread
  never finished a frame, so per-frame allocations were never retired: ~230 MB/s until the 12.5 GiB
  pool is exhausted (the "Ran out of memory" LowLevelFatalError).
- Fix `78019f9f`: sync mode None only drops the audio lock; video is still paced against the player
  clock (`time_stamp + loop offset <= now`). Opt-out: `KYTY_AVPLAYER_UNPACED_SYNC_NONE=1`.
  (shadPS4 also delivers unpaced in sync None; this title shows that is wrong.)
- Verification: `shtsm-avpace-b-1` ran the full 110 s with no OOM; live memory rose in load steps
  and plateaued at ~5.9 GB (was 12.5 GB and abort at ~70 s). AvPlayer delivery stays ~30 frames/s
  with frame time tracking the clock. `shtsm-avpace-b-2/-3` reached 60 s at ~4.2 GB before the
  separate crash below.

## 2026-10-11: new first blocker: intermittent host use-after-free in compute dispatch (MoltenVK/AGX)

- About 60% of Silent Hill runs (descriptor route, 256 MB) die at 40-60 s with
  `Unhandled host exception ... address=0x1b0` (once a garbage pointer). New backtrace:
  `-[AGXG17GFamilyComputeContext setTexture:atIndex:]` <- MoltenVK `bindMetalResources` <-
  `MVKMetalComputeCommandEncoderState::prepareComputeDispatch` <- `MVKCmdDispatch::encode` <-
  `vkQueueSubmit` <- `CommandScheduler::Submit/Flush` <- `GuestGpu::Process`. One run instead
  failed GPU-side with `kIOGPUCommandBufferCallbackErrorInvalidResource` (device lost). A Metal
  texture bound to a compute dispatch was freed after recording but before or during execution.
- `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2` (MoltenVK encodes at record time): 2 of 2 runs clean
  to 90 s (vs ~6 of 9 crashing without it). This points to the texture dying between vkCmd recording
  and vkQueueSubmit.
- Ruled out: texture-cache image deletion (deferred to tick completion), descriptor-pool recycling
  (tick-gated, same master semaphore), slot-vector moves (`Image` is non-movable), texel buffer
  views (none used), tiler/fault-manager compute (buffers only), and stale views written through
  `MakeImageInfo` (`KYTY_VIEW_CHECK=1` registry found none in 2 crashing runs).
- Open: find the freed object (candidates: views reached outside `MakeImageInfo`, e.g. mesh/compute
  paths that build `DescriptorImageInfo` directly; presentation `FrameImage::Configure` destroying
  an image immediately). Workaround candidate: set `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2` on
  the Silent Hill route.

## 2026-10-11: title-menu black background

- The menu background is the AvPlayer movie `ui_titlemovie01.mp4` (live-action footage of the
  protagonist) plus a 3D scene. `shtsm-menu-1` (no input probe, internal frame stats):
  - Movie path works: Unreal's NV12->RGB conversion writes 244x150 B8G8R8A8Srgb media textures with
    100% non-black pixels; the dumps show the correct movie frames.
  - The 3D scene renders: 3840x2160 RGBA16F scene colour (`0x80a1640000`, `0x809d640000`) is 64%
    non-zero, avg RGB ~(0.19, 0.13, 0.07), peak 2406.
  - Everything after that is black: an RGBA16F 3840x2160 target is (0,0,0,1), and every 8-bit
    3840x2160 B8G8R8A8 target is 0% non-black. The UI is drawn afterwards and composites correctly.
  - So the scene is lost in the post-process/tonemap chain. Suspects: the tonemap LUT
    (A2B10G10R10, possibly a 3D texture) and eye adaptation (R32G32B32A32F); zero exposure or a black
    LUT both produce exactly this. The stats tool skipped these formats; extended to decode them.

## 2026-10-11 (afternoon): title/warning black background fixed: MeshScratch binding collided with pixel Buffers (`86ec37b3`)

- Diagnostics committed: `4d939946` (`KYTY_VIEW_CHECK`), `9262422d` + `1d2e62da` (internal frame
  stats decode post-process formats, compute-written images, whole 3D volumes with per-slice
  coverage), `86c1a4ce` (`KYTY_RESOURCE_DUMP_HASH=<hash>` dumps a shader's bound buffers at draw
  time), `78f749ea` (`KYTY_DEBUG_EXPORT_VGPR=<hash>:<pc>:<vgpr>` replaces one pixel shader's
  compressed MRT0 export with a captured VGPR as (|v|, |v|/1024, 1, 1), for bisecting).
- `shtsm-menu-2` / `shtsm-lut-1`: the 3D colour-grading LUT (32x32x32 A2B10G10R10, guest
  `0x80472a0000`) was **all zero in every slice and every frame**. The tonemap pass
  (ps `0x8b3b4df51624ff13`) samples it, so the whole scene went black. Eye adaptation was fine
  (1x1 RGBA32F = 1, 0.36, 1, 2).
- `KYTY_DEBUG_SOLID_PS` (`shtsm-lut-solid-3`): the LUT filled magenta in 32/32 slices, so
  rasterization and the layer export work. The LUT pixel shader (ps `0x4a992c93b3630c1f`, Unreal
  CombineLUTs) itself computed 0.
- `shtsm-resdump-1`: guest memory for its uniform buffer was valid (LUTSize = 32.0 at +244, film
  slope 0.88, ...); the embedded ACES tables read through `s_getpc_b64` + `TBUFFER_LOAD` were
  valid too. The IR before and after passes was correct.
- VGPR bisect (`shtsm-bis-2..4`): rcp(LUTSize) = inf, LUTSize - 1 = -1, and UB[256]*UB[416] = 0,
  so **every uniform-buffer read in that draw returned 0** (bounds-checked load out of range).
  Forcing small read-only buffers to stream from guest memory changed nothing (diagnostic
  reverted).
- SPIR-V dump (`shtsm-spv-1`): the pixel shader's storage-buffer array is `Binding 56`.
  `NativeBinding(Pixel, Buffers)` = 0 + 1*56 = 56 and `NativeBinding(Mesh/Vertex, MeshScratch)` =
  56 + 0*56 = 56. On the descriptor-scratch route (`KYTY_MESH_SCRATCH_DESCRIPTOR=1`), every
  emulated mesh draw wrote the mesh scratch buffer over the pixel shader's buffers (and the set
  layout had a duplicate binding).
- Fix `86ec37b3`: MeshScratch gets its own binding, 4*56 = 224, after the last stage group. Only
  the opt-in descriptor-scratch route emits it.
- Verification `shtsm-fix-1`: LUT 32/32 slices non-zero, avg RGB (0.72, 0.70, 0.66). The warning
  screen now shows the 3D room (a lit doorway) behind the text instead of black.

## 2026-10-11 15:20 IST: correction: title-menu background is still black after `86ec37b3`

- `86ec37b3` did fix the all-zero tonemap LUT (CombineLUTs now non-zero), but the claim in
  `281db93e` that the warning screen "now shows the 3D room" was wrong: that doorway is a 2D
  asset and is identical before and after the fix.
- `shtsm-title-1` (`/tmp/titlerun.sh`, PREFILL=2, Cross at 25/29/33 s) reaches the title menu;
  its background is still black. The menu presents at ~1 fps.
- `shtsm-title-stats-2`: the eye-adaptation 1x1 RGBA32F (`0x8069ac0000`) is
  (0.0003, 0.0003, 652, 1), which scales the scene to black. Its inputs are garbage: the
  compute-written 4K B10G11R11 at `0x80a54f0000` decodes B~9300, max 64512, and the 240x135
  RGBA16F holds 65504 and negatives. Suspect: 10/11/11 float format mapping or packing for
  compute storage-image writes.
- New diagnostic: `KYTY_INTERNAL_FRAME_MIN_SECONDS=N` limits internal frame stats to work
  recorded N s after start (sample the menu, not boot).

## 2026-10-11 15:45 IST: forced exposure test; title-menu background still black; 10/11/11 lead downgraded

- New opt-in diagnostic `46914434`: `KYTY_DEBUG_FORCE_EXPOSURE=1` clears every compute-written
  1x1 RGBA32F storage image to (1,1,1,1) after the dispatch (optional
  `KYTY_DEBUG_FORCE_EXPOSURE_ADDRESS=<addr>` filter). Off by default. It fired on
  `0x8063460000`, `0x8069ac0000`, `0x8069bd0000`.
- `shtsm-forceexp-2` (`/tmp/titlerun.sh`, 105 s): the title menu is reached
  (`frames/frame_000780.png`) and the background is **still black**. So forcing the
  compute-written exposure textures does not bring the scene back.
- `shtsm-forceexp-stats-3` (stats from 100 s, menu) shows why the test is not conclusive:
  the exposure that matches the menu is ALSO written by a **pixel shader** into a render target:
  1x1 RGBA32F `0x8047290000` = (0.0003, 0.0003, 654.75, 1), ps `0x6db13477175bcbde`, fed by a
  16x2 RGBA32F rt `0x80aa6b0000` (z max 652, ps `0xe3125e2c8a09fe63`). The force hook only covers
  compute writes, so this one was not forced.
- The exposure value looks self-consistent rather than garbage: the menu scene colour (4K
  B10G11R11 rt `0x80a76a0000`, ps `0xe0d7ba4208d69fc6`) is 36% covered (dynamic resolution 60%,
  2304x1296 inside 3840x2160), avg ~143 over the whole surface, so ~400 inside the viewport;
  0.0003 x ~450 = ~0.13 (middle grey). z=652 is the average luminance, x = ~0.18/652.
- The B10G11R11 4K compute output `0x80a54f0000`/`0x80a5690000` (cs `0x3bf666b33f2d9fea`,
  R~0, G 0..1, B avg ~9300 max 64512) matches Unreal's motion-blur velocity-flatten layout
  (velocity length, angle, depth), not colour, so it is probably not garbage. Downgraded the
  "10/11/11 storage-write packing" lead. One real gap remains: storage-image views use an
  identity swizzle, so a compute write to a guest `k10_11_11Float` image (host B10G11R11 +
  BGRA view mapping + Reverse10_11_11 transfer) lands R/B-swapped. That swaps channels; it
  cannot make a frame black.
- Still suspicious: cs `0xe0c73b753a708b2b` / `0x7341209544b19208` write 240x135 RGBA16F with
  negative averages and 65504 (inf/NaN) in ~90% of texels.
- Where the black appears: the tonemap pass (ps `0x8b3b4df51624ff13`) writes the 4K B8G8R8A8
  `0x80ab7b0000` with only 0.0157% non-black pixels while its known inputs (scene colour, LUT
  32/32 slices non-zero, exposure ~0.0003) look valid.
- Next: extend the force hook to render-target writes (or a VGPR bisect of the tonemap ps with
  `KYTY_DEBUG_EXPORT_VGPR`) to find which tonemap input zeroes the output: scene-colour UV/viewport
  scale (60% dynamic resolution), bloom, or the exposure read.

## 2026-10-11 ~17:15 IST — title-menu black background traced from tonemap/exposure back to the DOF chain


Commits (all opt-in diagnostics, off by default):
- `07f27f9d` KYTY_DEBUG_EXPORT_VGPR now keeps the shader's own B/A export. Before, dropping B/A let dead-code elimination remove image samples, which caused an EXIT in ResourceMaterialization.cpp:486 on the tonemap PS.
- `70800d2d` KYTY_RESOURCE_DUMP_OFFSET=<bytes> and KYTY_RESOURCE_DUMP_SKIP=<n> for KYTY_RESOURCE_DUMP_HASH.
- `dc9820c6` KYTY_DEBUG_FORCE_EXPOSURE also forces pixel-shader-written 1x1 RGBA32F targets (the eye-adaptation rt) after the draw.
- `1e3ac3cf` KYTY_DEBUG_FORCE_EXPOSURE=<value> (for example 0.003). Frame stats gain lum_p10_90/lum_median metering for B10G11R11 and RGBA16F, and lit_avg_bytes for 8-bit targets.
- `638298ad` KYTY_IMAGE_TRACE_HASH=<hash> (KYTY_IMAGE_TRACE_EVERY=n) logs each image's source, first-use pc, guest T#, and resolved cache image for one shader.
- UNCOMMITTED on the Mac: renderCompute.cpp KYTY_DISPATCH_TRACE=1 (+ _MIN_FRAME / _MIN_SECONDS). It logs cs hash and group counts, and reads indirect args back from the GPU. Built OK, and run shtsm-disp-2 used it. Commit it.

Findings:
- Exposure is NOT the bug.
  - Forced exposure 1.0 makes the menu background white (shtsm-forceexp-4 frame_000720.png).
  - Forced 0.003 shows a foggy, very blurred blue-grey scene (shtsm-forceexp-5 frame_000720.png).
  - The real exposure (0.0003–0.0006) matches the metered pre-post scene: scene colour rt 0x80a7..0000 lum_p10_90 ≈ 190–220, eye-adaptation avg lum 313–386.
  - EA params: 0.1/0.9 percentiles, min/max lum 0.000176/188743, histogram scale 1/30, bias 1/3.
  - View UB: PreExposure 1, OneOverPreExposure (tonemap s31 @2168) 1.
- The tonemap PS 0x8b3b4df51624ff13 is fine. Disassembly is in work/tmps.txt.
  - KYTY_IMAGE_TRACE_HASH (shtsm-imgtrace-1) shows that on menu frames its scene input t0 (user SGPR s0) = 0x80d5..0000. That is a 4K B10G11R11 written by cs 0x2bfb24830d8fcfaa (dispatch 1x1x1296), not the pre-post scene 0x80a7..0000.
  - Bloom (s8) and lens (s16) are 1x1 dummies on the menu.
- cs 0x2bfb24830d8fcfaa (DOF recombine, by its role) outputs a scene ~4.4x darker and almost uniform:
  - output: lum_p10_90 48.8, median 48.8, max (194, 57, 88)
  - input scene: lum_p10_90 213, max 15000
  - x ≈ 48.8 × 0.0005 ≈ 0.02 → LUT toe → near black. That is the black background.
- The DOF gather outputs (cs 0x778f33d239f78462 direct 240x135; 0x254033019fc671c8 and 0x2213d0ab109940c8 indirect 469x1x1) are only 0.7–0.8% non-zero over 1920x1080. 2304x1296/2 = 1152x648 should be fully covered.
- Two other indirect dispatches get 0 groups: cs 0xc3c3f948c9c86e1a and 0x447abe57e0af3240 (shtsm-disp-2).
- Leading hypothesis: DOF tile classification and indirect-arg building under-count tiles. The cs is wave64 on host subgroup 32 (lane_count=2 emulation; MoltenVK reports subgroup 32, size_control=false). Ballot/mbcnt/atomic/DPP/permlanex16 wave-reduction emulation is the suspect.
  - Example: cs 0xe0c73b753a708b2b (disassembly in work/mb.txt) is an 8x8 CoC min/max reduction using DPP row_shr + permlanex16 + readlane 31/63.
- Rejected:
  - A k10_11_11 storage-swizzle (R/B swap) fix was tried in ResourceMaterialization. The velocity-flatten output is unchanged, so that image is k11_11_10. Reverted, not committed.
  - VGPR bisect numbers: v13@0x1c8 ≈ 42 (pre-exposure red), v8@0x198 ≈ 0.675 (s44 constant, lens term 0), v5@0x180 ≈ 0 (bloom dummy).
- The title menu needs ≥ 82 s runs: the menu appears at dump frame 660–720.

## 2026-10-11 ~18:45 IST — title-menu black: FFT bloom is fine; its pre-filter clamps the scene because PreExposure=1 (LDS wave-sync hypothesis rejected)

Relabel (from the ~16:45 investigation): cs 0x2bfb24830d8fcfaa / 0x674e5a7163897bc9 are Unreal convolution (FFT) bloom, not the DOF chain.
- Forward: 0x2bfb reads the 4K B10G11R11 scene (0x80a75a0000 in shtsm-fft-3) and writes a 4098x1296 RGBA32F row spectrum.
- 0x674e (1x1x4098) does the column FFT, the kernel multiply (kernel spectrum 4098x4096 RGBA32F, rebuilt by 0x2bfb 4098x4096→4098x4096) and the inverse column pass.
- Inverse: 0x2bfb writes the 4K B10G11R11 that the tonemap uses as its scene input (Bloom/lens inputs are 1x1 dummies, as Unreal does when FFT bloom is enabled).

Rejected: the LdsWaveSync hypothesis (s_waitcnt lgkmcnt(0) → subgroup barrier).
- Both FFT shaders DO use s_barrier: 40 in 0x2bfb, 48 in 0x674e. The disassembler prints it in lowercase, so the earlier case-sensitive grep missed it.
- With the fix, the 0x2bfb output was unchanged: lum median 48.93 (shtsm-ldsstats-on) vs 48.82 before (shtsm-swzfix-1). The menu was still black (shtsm-ldssync-3 frame_000780.png).
- The fix did not slow anything down: shtsm-ldssync-2 matched shtsm-title-1 frame timing. The 29-minute stall in shtsm-ldssync-1 was the Mac sleeping while the MCP was down.
- Reverted, not committed. The patch is kept at /tmp/ldswavesync_rejected.patch on the Mac.

Numerical check (shtsm-fft-2, raw readbacks via new KYTY_INTERNAL_FRAME_RAW_DIR, analysis in project work/fft/):
- Column/kernel pass 0x674e is ≈ identity on the spectrum (avg/max within ~1%).
- numpy irfft of the forward spectrum row reproduces the final output row exactly (mean 43.3, std 1.8, zero outside x≥2304). The inverse FFT is correct.
- The forward FFT is also correct. Alpha DC = 2305 ≈ 2304 pixels, and the spectrum follows the input. BUT the input the FFT sees is ~constant: every row mean ≈ 43 R, while scene row means run 164→485.
- Cause: the forward pre-filter in 0x2bfb (pc 0x5b0): lum = 0.2126R+0.7152G+0.0722B; if lum > Min: color *= min(Max, Mult*(lum−Min)+Min)/lum. UB dwords 0..2.
- Menu-frame UB (shtsm-fftub-2, KYTY_RESOURCE_DUMP_HASH=0x2bfb24830d8fcfaa): Min=0.6 (0x3f199998), Max=50 (0x42480000), Mult=150 (0x43160000). Boot frames: (5.32, 11068, 50.5) and (2.76, 5088, 104.5).
- Scene lum ≈ 500, so every pixel is clamped to lum 50. Measured output lum ≈ 50.9 → the result is flat and 10x dark. The tonemap then multiplies by exposure 0.0003, giving black.
- This also explains the earlier "forced exposure 0.003 → foggy, very blurred blue-grey scene".
- Interpretation: the thresholds expect a pre-exposed scene colour (×~0.001–0.003), but Kyty's view UB has PreExposure = 1 (seen earlier, tonemap s31 @2168). Unreal sets View.PreExposure on the CPU from a GPU readback of the previous frame's eye adaptation. Suspect: that readback never reaches guest memory in Kyty (GPU-resident image/buffer not written back, or its fence/label path), so PreExposure stays 1.

New diagnostics: KYTY_INTERNAL_FRAME_STATS meters the top rows of images too large for the 64 MiB download (rows_clamped). KYTY_INTERNAL_FRAME_RAW_DIR=<dir> writes raw readbacks.

Causal proof (opt-in debug poke, commit 2de0bc80):
- Run shtsm-prefilter-off-3 used KYTY_DEBUG_BUFFER_POKE=0x2bfb24830d8fcfaa:0:0:3f199998:7f7fffff, which sets pre-filter Min to FLT_MAX so the pre-filter never fires.
- Result: FFT bloom output lum median went from 49 to 463, and the TITLE MENU BACKGROUND IS LIT (rooftop scene with the girl holding her phone): harness/runs/shtsm-prefilter-off-3/frames/frame_000780.png. Baseline, still black: harness/runs/shtsm-ldssync-3/frames/frame_000780.png.
- This is a proof only, NOT a fix. The real fix is to make the scene colour arrive in the units the pre-filter expects, most likely by making Unreal's View.PreExposure correct (the CPU eye-adaptation readback).
- Commits: 51945a2b (rows_clamped + KYTY_INTERNAL_FRAME_RAW_DIR), 2de0bc80 (KYTY_DEBUG_BUFFER_POKE). Both are opt-in; default paths unchanged.

## 2026-10-11 ~19:20 IST — FIXED: black title-menu background (aeddbf04, linear-image readback for PPSA10112)

- Root cause confirmed. Silent Hill reads its GPU-written (linear) eye-adaptation result back on the CPU to set Unreal View.PreExposure. Kyty's linear-image readback (`--readback-linear-images`) was off by default, so the CPU never saw the value, PreExposure stayed 1, and the FFT-bloom pre-filter (0.6/50/150) clamped the scene to lum 50, which rendered black.
- Evidence:
  - With `--readback-linear-images true` and no other forcing: shtsm-rbli-1 frame_000780.png and shtsm-rbli-2 frame_000720.png show the lit rooftop menu scene (girl with phone).
  - With the new per-title default (aeddbf04, no flags): shtsm-rbdef-1 frame_000780.png is lit. guest-printf logs "linear-image readback enabled by default for PPSA10112".
  - Baseline (black): shtsm-ldssync-3 frame_000780.png.
- No measurable slowdown: frame-dump timing matched the black baseline (frames 360→780 in ~78 s vs ~87 s).
- aeddbf04: title-gated (PPSA10112 only). KYTY_READBACK_LINEAR_IMAGES=0|1 overrides it; `--readback-linear-images true` still forces it.
- Animal Well 3/3 after aeddbf04 (aw-regress-{1,2,3}-rblinear: stage 2, 32-33/42-43 content frames, the same 10 black boot frames as the 33/43 baseline). Readback is not enabled for PPSA22520.
- Supporting commits: def6fafe (harness KYTY_EXTRA_ARGS), 2de0bc80 (KYTY_DEBUG_BUFFER_POKE), 51945a2b (frame-stats rows_clamped / raw dumps).
- Still stage 2. No gameplay soak or reference-image check has passed. The menu still presents at ~1 fps.

## 2026-10-11 ~19:40 IST — KYTY_VIDEOOUT_RES=720|1080 (opt-in), title-menu FPS, "New Game crash" triage

- New opt-in `KYTY_VIDEOOUT_RES=720|1080` (unset / 2160 / anything else = unchanged default).
  - `sceVideoOutGetOutputStatus` reports resolution code 1 (non-4K) instead of 2.
  - Silent Hill ignores that: its RHI only reads `dynamicRange` from the status (eboot 0x90114c82d) and sizes the display buffers from the `PS5.DefaultBackBufferWidth/Height` cvars, registered with hard-coded defaults 0xf00/0x870 (eboot rva 0x174d7d7 / 0x174d84a). Tried codes 0 and 1 first: the game still registered 3840x2160 (shtsm-vres1080-1, shtsm-vrescode0-1).
  - So for PPSA10112 only, `Loader::GamePatch::ApplyVideoOutResolution` rewrites those two immediates after relocation, after verifying the 19 surrounding bytes (lea of the cvar name + `mov edx, imm32`). Other titles: log only.
  - The presenter already blits any registered buffer size to the swapchain (vkCmdBlitImage, linear).
- Evidence: shtsm-vres720-ng-1 registers 1280x720 (log "VideoOut resolution patch: PPSA10112 default back buffer set to 1280x720"; RegisterBuffers2 width 1280 height 720); shtsm-fps-1080-1 registers 1920x1080. Title menu background is lit at 720: harness/runs/shtsm-vres720-ng-1/frames/frame_001260.png (unforced, no exposure forcing).
- Title-menu FPS (KYTY_FRAME_TIMING presents/s on the menu, FRAME_STATS off; noisy, Alan was using the machine during several runs):
  - default 4K: 0.45-1.5 fps (shtsm-fps-def-1, shtsm-newgame-1, shtsm-vres1080-1 had stats on)
  - 1080: 0.9-1.4 fps (shtsm-fps-1080-1)
  - 720: ~1.9 fps (shtsm-vres720-ng-1); boot/warning screens 46 fps vs 25-28 at 4K.
  - Conclusion: resolution is NOT the main menu bottleneck; 720 helps only marginally. Next: profile the per-frame CPU/GPU cost on the menu (present timing, readback/texture-cache stalls).
- "Crash on NEW GAME" (shtsm-play-1): not a guest crash. The log ends with `Event: quit` = SDL_EVENT_QUIT (window close / Cmd+Q / SIGTERM; a harness timeout produces the same line). No kyty_emulator .ips in DiagnosticReports. exit 0. kyty.log ending mid-line is unflushed stdout (same in normal runs). In that run only two Cross (J) presses were made (19.8 s, 22.3 s); the menu needs three, so NEW GAME was probably never selected.
  - Repro shtsm-vres720-ng-1: Cross x3, then Cross every 4 s from 78 s on the menu (NEW GAME). The game kept running for 70 s after NEW GAME until the harness timeout; screen after NEW GAME is black (frames 1320-1560) and presents ~2-8 fps. Not a crash; what the black screen is (loading, cutscene, or missing rendering) is still open. aeddbf04 / MVK prefill not implicated (no crash to attribute).
- Animal Well 3/3 with KYTY_VIDEOOUT_RES unset after 60f1fb0c (aw-regress-{1,2,3}-vres: stage 2, 31/41, 33/43, 32/42 content; same 10 black boot frames as baseline; no resolution-patch log lines).
- Still stage 2. No gameplay or visual acceptance passed.

## 2026-10-11 ~21:15 IST — Perf batch 1 committed and validated at native 4K (no resolution changes)

- Commits (each separate): ed0f14c2 (KYTY_MESH_SCRATCH_DESCRIPTOR parsed once, =1|true), 5966faae (KYTY_READBACK_LINEAR_IMAGES strict 0/1/true/false), 829ce2c8 (unified-memory GPU cache budget cap = RAM - max(RAM/2, 8 GiB) -> 8192 MiB on the 16 GB Air; KYTY_GPU_BUDGET_MB override; KYTY_GPU_BUDGET_LEGACY=1), 3c4bc90c (opt-in KYTY_GC_TUNING=1), 59baf401 (clear "GPU fatal" device-lost/OOM report), 75c3b77a (MemoryUsage log every 30 s, KYTY_FRAME_SAMPLE_SECONDS), ac508762 (loud shm_open EPERM fallback log), 113277c5 (AvPlayer sync-None pacing only for streams without audio), 55ecc06f (scePadReadState edge latch, KYTY_PAD_READSTATE_LATCH=0 disables). Earlier in this batch: 5bc5b003, 9a651731 (KYTY_UNMAP_FASTPATH), 28e8f767 (KYTY_FAST_VRANGES + KYTY_REMAP_STATS).
- 4K New Game, 480 s, default flags (shtsm-b1ng-off-1; route = tr2.sh + KYTY_INPUT_PROBE_KEY=J START 60000 INTERVAL 3000 HOLD 100): stage 2, reached gameplay/cutscenes (frames 1800/1980 lit, no streaks), no OOM, no "GPU fatal", no LowLevelFatalError. GPU usage 8.1-8.6 GiB against the 8192 MiB budget; process footprint 21 GB (11 GB SYSTEM_MANAGED + 9.5 GB graphics) on the 16 GB Air, swap 2-6.5 GB.
- FPS at 4K (KYTY_FRAME_TIMING, 40 s windows): title menu 1.25-1.3 fps (median ~770 ms; shtsm-b1-off-1); in-game 0.7-1.9 fps (median 590-1570 ms) flags off (shtsm-b1ng-off-1) vs 0.6-1.4 fps flags on (KYTY_FAST_VRANGES=1 KYTY_UNMAP_FASTPATH=1, shtsm-b1ng-on-1). Within noise: the remap path is no longer the bottleneck (in-game BatchMap time < 1 % of wall time; the unmap fast path rarely fires because the GPU thread is never idle: unmap_slow_busy dominates).
- Profiles (`sample`, 10 s): menu GPU thread 62 % blocked in synchronous downloads (MaterializeColorClear 26 %, fault reads from guest threads 24 %, DrawIndirect self-faults 12 %); in-game GPU thread ~100 % busy: 36 % CommandScheduler::Wait (MaterializeColorClear 17.5 %, guest fault reads 11 %, DrawIndirect 3 %, aggressive buffer GC drain 7 %), MoltenVK addCommand (PREFILL=2) 20 %, PrepareBda 9 %, pipeline lookup + SRT walk 13 %. RenderThread waits ~47 % on the GPU side and ~12-32 % in fault readbacks.
- Animal Well 3/3 with default flags after 55ecc06f (aw-regress-{1,2,3}-b1: stage 2, 32/42, 33/43, 33/43 content).
- Still stage 2.


## 2026-10-11 ~22:20 IST — Perf batch 2 (readback/drain removal), all opt-in, native 4K

Profiling (`sample`, 10 s at 360 s, 4K) showed the guest GPU thread is the critical path in game: ~36 % of it blocked in full GPU drains (CommandScheduler::Wait), the rest CPU recording. Per-origin counters (KYTY_READBACK_STATS, shtsm-rb-stats-1) in game, per 2 s: clear-metadata 10-12 drains costing 0.6-1.1 s, CPU read faults ~130 (sequential 4 KiB pages of one GPU-written buffer, walked backwards), CPU write faults on GPU-dirty pages 90-275.

New commits (all behind env flags, defaults unchanged):
- 6ef9e0eb KYTY_READBACK_STATS=1 (ReadbackStats line every 2 s: count/bytes/caller wall/GPU wait per origin, hot fault pages, cpufill) and KYTY_FAULT_COALESCE=1 (a read or write fault on a GPU-dirty page downloads the whole contiguous GPU-dirty run inside the buffer, capped at 1 MiB, in one drain; only the written range becomes CPU-modified).
- 5634afc3 KYTY_CLEAR_META_NOSYNC=1: a recognised uniform-fill compute over plain memory that no cached image aliases and no recorded GPU write dirtied is applied on the CPU (same rule DMA_DATA fills follow), so MaterializeColorClear no longer reads metadata back.
- 97ee4c5e KYTY_BDA_SYNC_SKIP=1: PrepareBda (per draw) skips its full scan of every cached buffer when no page became CPU-modified, no buffer was registered and no range was mapped since the last complete scan (global CPU-dirty generation bumped in RegionManager::ChangeState / region creation / buffer register / MapMemory).
- bbb414e7 KYTY_GC_DRAIN_INTERVAL=<n>: above the critical memory threshold the buffer GC's GPU-dirty retirement (download + full wait) runs only every n-th collection with up to 8x the batch; clean buffers still retire every collection.

Effect (stats on in both arms):
- clear drains 10-12 -> 0 per 2 s (cpufill 20-30 fills / 2 s instead); read faults ~130 -> ~15 per 2 s; write-fault drains ~100-275 -> 0-8 per 2 s (shtsm-rb-fix-2 vs shtsm-rb-stats-1).
- GPU thread profile in game: readback waits 25-30 % -> ~16 %, PrepareBda 10 % -> 3 % (when the generation is stable), buffer-GC drain 17 % -> 3 % (shtsm-rb-all-1 vs shtsm-rb-gc-1). After this the guest RenderThread is busy ~69 % (guest JIT code) and becomes the co-critical path.
- Visual: title menu still lit, in-game frames sane (shtsm-rb-fix-2 frame_001059 menu, 001650/001800 game; shtsm-rb-gc-1 001560/001620).
- No OOM, no device loss in any 480 s run; GPU usage 8.0-8.7 GiB vs 8192 MiB budget; host swap 3-8 GB (machine-wide, 16 GB Air).
- Animal Well (PPSA22520), 45 s each, stage 2 with content in every run:
  - aw-regress-{1,2,3}-allflags: KYTY_CLEAR_META_NOSYNC + KYTY_FAULT_COALESCE + KYTY_BDA_SYNC_SKIP + KYTY_FAST_VRANGES + KYTY_UNMAP_FASTPATH (no GC interval in this set) -> 32/42, 33/43, 33/43 content/sampled. 3/3.
  - aw-regress-{1,2,3}-allflags2 and -allflags3: the same plus KYTY_GC_DRAIN_INTERVAL=16 -> 24/34, 27/37, 29/39 and 28/38, 30/40, 30/40. 3/3 + 3/3.
  - aw-regress-{1,2,3}-fastvr: KYTY_FAST_VRANGES=1 alone -> 30/40, 28/38, 25/35. 3/3.
  - The lower sample counts in the later batches track machine state (each batch ran straight after a 480 s Silent Hill run, swap 3-8 GB): the FAST_VRANGES-only batch dropped the same way, so it is not attributable to the GC interval.

## 2026-10-11 ~22:40 IST — batch 2 A/B at 4K (shtsm-ab-all-2 vs shtsm-ab-base-1), CPU fill pre-invalidate

Uncommitted fix built and measured: TryConsumeComputeImageClear now calls BufferCache::InvalidateMemory over the whole range before the CPU fill (KYTY_CLEAR_META_NOSYNC path only), so the fill no longer takes one write fault per 4 KiB page.

Same route as shtsm-ab-base-1 (F10 probe, J bootstrap x12, reaches the in-game phone-message scene), FPS per 40 s window (python3 /tmp/fpsw.py <run> 40):

| window | base-1 (flags off) | all-1 (batch 2 + GC16) | all-2 (+ fill pre-invalidate) |
|---|---|---|---|
| 0-40 s (intro) | 24.7 | 14.2 | 14.1 |
| 40-80 s | 13.9 | 6.9 | 6.3 |
| 80-120 s | 4.9 | 7.3 | 6.6 |
| 120-160 s | 1.5 | 2.1 | 2.5 |
| 320-480 s (in game) | 1.0-1.2 | 1.35-1.43 | 1.35-1.57 |

- The pre-invalidate does not fix the intro regression. ReadbackStats in the intro explains it: with the flags off, 64-168 clear-metadata drains per 2 s each wait only for a little GPU work (~1.5 s total wait, fault waits ~60 ms); with the flags on, clear drains are 0 but the 15 fault + 10 inval drains per 2 s each wait 30-100 ms for the whole unsubmitted backlog (0.4-1.2 s total). The readback flags move the GPU wait to the surviving drains; the general fix is submitting earlier (batch 3: KYTY_FLUSH_ON_LABEL) and waiting on the caller (KYTY_FAULT_WAIT_ON_CALLER).
- In game the batch-2 flags are +25-40 % (1.0-1.15 -> 1.35-1.57 fps). Frame 1560 of shtsm-ab-all-2 (phone message) is visually sane. No OOM, no device loss.
- KYTY_FAST_VRANGES becomes the default (Animal Well 3/3 with it alone and 6/6 with all flags); KYTY_FAST_VRANGES=0 restores the old path.

## 2026-10-11 ~23:45 IST — Perf batch 3 A/B at native 4K (label flush vs draw-budget flush vs full stack)

All arms: same route as shtsm-ab-base-1 (F10 probe, J bootstrap x12, 480 s, PREFILL=2) with the batch-2 flags (CLEAR_META_NOSYNC, FAULT_COALESCE, BDA_SYNC_SKIP, GC_DRAIN_INTERVAL=16, UNMAP_FASTPATH) plus FAULT_WAIT_ON_CALLER, BDA_DIRTY_REGIONS, THREAD_QOS. FPS per 40 s window (python3 /tmp/fpsw.py <run> 40):

| window | base-1 (off) | b3-label-1 (+FLUSH_ON_LABEL) | b3-budget-1 (+FLUSH_DRAWS) | b3-full-1 (+label+draws+BDA_SYNC_EPOCH+WRITE_FAULT_SPAN=16+SRT_MEMO) |
|---|---|---|---|---|
| 0-40 s (intro) | 24.7 | 14.7 | 13.9 | 14.6 |
| 40-80 s | 13.9 | 7.0 | 7.0 | 7.0 |
| 80-120 s | 4.9 | 6.1 | 7.5 | 6.5 |
| 120-160 s | 1.5 | 3.1 | 2.1 | 2.1 |
| 160-240 s (phone-message scene) | 0.6-1.3 | 2.4-2.6 | 0.9-1.4 | 1.0-1.05 |
| 240-440 s | 0.7-1.2 (phone scene) | 3.9-5.8 (walking, see below) | 1.2-1.6 (phone scene) | 1.4-1.85 (phone scene) |

- Not like for like after ~240 s: only the label arm's input route closed the phone message and reached free walking (frame 2220: room with the painted curtain; frames of budget-1/full-1/base-1 at the same time still show the phone). Its 3.9-5.8 fps is a different scene. The like-for-like evidence is 160-240 s (same phone scene in every arm): label 2.4-2.6 vs budget 0.9-1.4 vs full 1.0. FLUSH_DRAWS (alone or stacked with the label flush) gives back the label-flush gain there.
- b3-label-1 ended at 462 s with exit 0 (not a timeout) and no exception/OOM line; the last 40 s window is partial. To recheck.
- No OOM / "Ran out of memory" / host exception in any batch-3 run (label, budget, full, all).
- Intro: the regression is still KYTY_CLEAR_META_NOSYNC: shtsm-intro-b3-2 (batch-3 flags only, no batch-2) 26.2 fps 0-20 s; shtsm-intro-flush-1 (FLUSH_DRAWS only) 25.5.
- KYTY_SRT_MEMO=1 (4 slots) in b3-full-1: hit=5376 miss_inputs=49677 miss_words=1 per 2 s (~10 % hits; misses are input-key misses, not changed words).
- Animal Well x3 with the full batch-3 stack (aw-regress-*-b3full): stage 2, 33/43 content in 3/3.
- Default-candidate set going into batch 4: batch-2 flags + FAULT_WAIT_ON_CALLER + BDA_DIRTY_REGIONS + THREAD_QOS + FLUSH_ON_LABEL=1 (no FLUSH_DRAWS). All flags stay opt-in. Claims stay at stage 2.
- 181eb078 (KYTY_RT_FEEDBACK_UNION, committed by the chain3 job) reviewed: is_bound/shader_write are set by the binding pre-pass before AcquireRenderTargets (the layout choice already depends on is_bound), and the requested access is a superset, so it is correct; arm shtsm-b3-rtfu-1 measures it.

## 2026-10-12 ~00:25 IST — Perf batch 4 results (runner kyty-runner-r6, ex perf4): 8886d9b5..3223f2ef

Commits (one per swarm entry, all opt-in or measure-only): 8886d9b5 AUDIO2_POLL_PACE, b4f4dce1 GPU_BLOCKED_BACKOFF, d384c3df SYNC_AUDIT, dc6db616 BUFFER_CACHE_STATS/BUFFER_GROWTH, 7fde5c50 READBACK_PRODUCER_TICK(_STATS), 86b1e488 RETIRE_ON_LAST_USE, 5a327c02 MESH_PSB_TRIM, c6e94326 SEM_SPIN, 814d5e7f FAULT_WAIT_ON_CALLER safeguards, cde9eb25 SRT_MEMO=2 (+VERIFY), 67744f19 SUBMIT_THREAD, 15ff4df2 WRITE_FAULT_STATS, 3223f2ef EVICT_STATS/SMART/ALIAS.

All b4 arms ran with BEST = batch-2 flags (incl. CLEAR_META_NOSYNC) + FAULT_WAIT_ON_CALLER + BDA_DIRTY_REGIONS + THREAD_QOS + FLUSH_ON_LABEL, tr2.sh route (probe start 25 s / 4 s / 250 ms, F10 + J bootstrap x12) — NOT the shtsm-b3-label-1 route (b2run.sh: start 60 s / 3 s / 100 ms + REMAP/READBACK stats).

PREFILL arms (120 s, BEST + RETIRE_ON_LAST_USE), fps per 40 s window (python3 /tmp/fpsw.py <run> 40):

| arm | 0-40 | 40-80 | 80-120 | result |
|---|---|---|---|---|
| pfA PREFILL=2 | 11.9 | 6.2 | 2.6 | stage 2, clean |
| pfB PREFILL=1 | 12.0 | 6.4 | 2.2 | stage 2, clean (no gain vs pfA) |
| pfC PREFILL=1 + SUBMIT_THREAD | - | - | - | crash at 33 s: host exception, null deref (address 0x20) in the CommandScheduler::SubmitAsync thread |
| pfD PREFILL=0 + SYNC_QUEUE_SUBMITS=0 | - | - | - | device lost at 31 s: kIOGPUCommandBufferCallbackErrorInvalidResource (3.8 GiB used) — RETIRE_ON_LAST_USE does not cover every PREFILL=0 lifetime |

- Verdict: keep MVK PREFILL=2 default. PREFILL=1+RETIRE is safe but no fps. SUBMIT_THREAD races (MoltenVK useResources / pool) — stays opt-in, do not promote.
- SRT_MEMO=2 VERIFY: SH 120 s and AW 45 s, 0 mismatches (verify_exit=0). Hit rate only ~5 % (calls=753664 hits=38740 key_miss=66287 bypassed=648637): adaptive bypass shuts it off; no fps value in SH. Keep opt-in.
- shtsm-b4-all-1 (BEST + RETIRE + AUDIO2_POLL_PACE + GPU_BLOCKED_BACKOFF + MESH_PSB_TRIM + SEM_SPIN + SRT_MEMO=2 + all stats + WIDE_STORE_HOT record mode, moved=0) STALLED: 216 presents in 0-40 s then none to 480 s (stage 1, black frames, no exception). sample at 360 s (/tmp/samp-b4all.sym.txt): RenderThread in pthread_cond_wait, GuestGpu::ThreadRun idle in CondVar::WaitFor, main thread in KernelWaitEventFlag, AK::EventManager 88 % in AudioOut2ContextGetQueueLevel -> SleepMicro (pace sleep), PresentThread sleeping. Reproduced by shtsm-b4-widehot-1 (same set + 9 HOT-relocated sites): 177 presents then stall. Bisect -> batch 5 / r6 entry.
- KYTY_WIDE_STORE_HOT: swarm 33 found its relocation can silently corrupt state on an unseen jump into S+4 (valid opcode at the displacement high byte). Never in a default set; widehot-1 results are suspect (and it stalled anyway).
- EVICT_SMART check skipped: evcheck saw 0 EvictStats lines (the all-1 run never left the intro), inconclusive, not a negative.
- Animal Well (45 s, PREFILL=2): b4def 34/44, 33/43, 33/43; b4all (BEST+new+SRT_MEMO=2) 31/41, 29/39, 29/39; b4wide 31/41, 33/43, 34/44; b4evict (EVICT_SMART+ALIAS+STATS) 34/44, 33/43, 34/44 (reclaim never triggered). 12/12 stage 2 with content. aw-srtv-b4 34/44, verify 0.

## 2026-10-12 ~00:50 IST — Perf batch 5: swarm 06 / 31 / 32 (70583ab8, 2d05b32c, ad369982)

- 70583ab8 KYTY_GPU_INDIRECT (06), 2d05b32c KYTY_MESH_BATCH + KYTY_DEPTH_READ_UNION (31), ad369982 KYTY_SPEC_READ_HOIST + KYTY_STATE_CACHE (32). One build, one commit each, all opt-in.
- shtsm-b5-gpuind-1 (BEST + GPU_INDIRECT + READBACK_STATS + SYNC_AUDIT, 480 s, tr2 route): stage 2, timed out, no exception. GpuIndirect per 2 s: gpu=54-128, cpu_multi=0, cpu_dest=0, resolve=0/0 drains/0 us, seq_miss=0, overflow=0, oob=0, fallback only not_mesh (3-4 in the intro, ~250-290 in game). The GPU path is taken and never needs a CPU resolve. Frame 1320 (phone message) visually sane. Not reverted.
- shtsm-b5-mb-1 (BEST + MESH_BATCH + DEPTH_READ_UNION + SPEC_READ_HOIST): stage 2, timed out, no exception. MeshBatch mean_run median 1.30 over 220 lines, 1.24 in the second half (in game), last 10 lines 1.0-1.61; rejects 0; all flushes at end_rendering. Below the 1.3 bar in game -> MESH_BATCH stays off. StateCache (hoist only): 4.42 M draws, spec_reads 143.8 M (32.5/draw), spec_bound_pairs 2.19 G (~496 per draw, ~15 bound images walked per read), spec_range_pairs 2.26 G.

FPS per 40 s window (same tr2 route; no same-route best-set 480 s baseline exists — b3-label-1 used the b2run route, see r6 confirmation entry):

| window | b5-gpuind-1 | b5-mb-1 |
|---|---|---|
| 0-40 | 12.3 | 11.6 |
| 40-80 | 6.1 | 6.4 |
| 80-120 | 2.9 | 2.7 |
| 120-160 | 1.5 | 1.4 |
| 160-200 | 1.0 | 1.1 |
| 200-240 | 1.35 | 1.85 |
| 240-320 | 1.8-1.95 | 1.8 |
| 320-480 | 1.2-1.6 | 1.3-1.6 |

- Both arms are the phone-message scene from ~160 s to 480 s; neither reached free walking on this route.
- Animal Well (45 s): b5off 30/40, 33/43, 33/43; b5gpuind 33/43, 34/44, 33/43; b5mb 32/42, 31/41, 32/42. 9/9 stage 2 with content.

## Label-run exit (shtsm-b3-label-1, 462 s, exit 0, timed_out false)

- Ended by a clean SDL_EVENT_QUIT ("Event: quit", window.cpp GameEventQuit) at 23:20:15; no exception/OOM line. Kyty only gets SDL_EVENT_QUIT from SIGINT/SIGTERM or an app quit; the harness's own timeout SIGTERM gives the same exit 0 but sets timed_out. Nothing in b10.sh/b2run.sh pkills mid-run; the time matches the perf4 runner takeover (23:19). Conclusion: external termination, not an emulator failure. Recheck: the r6 480 s confirmation runs count "Event: quit" and must report timed_out=true.

## 2026-10-12 ~04:00 IST — r6 batches 6/7 invalid (disk full), r7 reruns (runner r7, MCP my-computer3)

**Root cause of the 01:10 outage and of the b6/b7 "stalls": the Data volume was full.**
- At 02:40 there was 3.7 GB free and harness/runs held 163 GB. shtsm-b6-best-1 and shtsm-b7-best-1 died with `spdlog fatal error: ... guest-printf.log: No space left on device`. The other b6/b7 arms ran with swap unable to grow.
- So every b6/b7 number is void, including the "SEM_SPIN collapses to 0.05 fps" lead and the b6-backoff 445 s run. The b7 NOTRAP smoke (exit -9 at 97 s) is void too. b7 stopped at 01:07 at the start of best-1. No b7 480 s run, NOTRAP AW, or sample was ever produced.
- Cleanup, approved by Alan:
  1. gzip of 18 non-shtsm logs over 100 MB took free space from 3.7 to 45 GB.
  2. Alan then cleared space himself and ordered all runs deleted except shtsm-ab-base-1, shtsm-ab-all-2 and shtsm-b3-label-1. Their frames/ were removed, logs kept, and the b3-label route kept in shtsm-b3-label-1/route/ (b2run.sh, tr2.sh, b10.sh, invocations.txt). Free space went from 100.5 to 163.1 GiB.
- New runner rules:
  - df ≥ 20 GB is checked before every run.
  - Frames are trimmed to every 240th present plus the last one, as JPEG.
  - /tmp/r7memmon.sh logs footprint, swap and free% every 5 s to /tmp/r7mem.log. It kills kyty when footprint > 24 GB, swap > 12 GB or free < 6 %. The first thresholds were 13 GB and then 21 GB; both killed normal runs (best-1, backoff-1, best480-3).

**SH in-game memory:** footprint 22–23 GB (sample: Physical footprint 22.8 G, peak 23.1 G in win480-3), swap peak about 9.5 GB, free% 11–15. The tunnel survived this once disk space was available.

**b6 bisect rerun (120 s, F10 + J×12 tr2 route, best = FAULT_COALESCE BDA_SYNC_SKIP GC_DRAIN_INTERVAL=16 UNMAP_FASTPATH FAULT_WAIT_ON_CALLER BDA_DIRTY_REGIONS THREAD_QOS FLUSH_ON_LABEL), fps per 20 s:**

| arm | 0-20 | 20-40 | 40-60 | 60-80 | 80-100 | 100-120 | end |
|---|---|---|---|---|---|---|---|
| best-2 | 27.8 | 12.0 | 9.0 | 3.85 | 2.9 | 2.0 | timed out, stage 2, intro text |
| +AUDIO2_POLL_PACE (pace-2) | 12.0 | 4.1 | 2.95 | 2.15 | 1.3 | 0.8 | timed out, still on the title card at 462 presents |
| +GPU_BLOCKED_BACKOFF (backoff-2) | 27.8 | 12.5 | 9.95 | 2.65 | 1.75 | 1.2 | timed out, intro text |
| +SEM_SPIN (semspin-2) | 22.75 | 9.3 | 7.1 | 6.9 | 2.2 | 1.9 | timed out; SemSpin spin_hits=0 (69/69 waits blocked) |

- No arm stalls; the shtsm-b4-all-1 "stall" bisect is closed as a disk-full artefact.
- **AUDIO2_POLL_PACE is a regression on this route** (about half the presents, slower intro) → revert from any candidate set.
- **SEM_SPIN**: zero spin hits and slower in the first 60 s → no value, keep off.
- **GPU_BLOCKED_BACKOFF**: neutral to slightly worse after 60 s (one run) → keep off.

**NOTRAP (9211bfe2, entry 33) smoke shtsm-r7-notrap-smoke-3 (120 s):**
- Self-test OK (sentinel 0x0e, hits=1 at the zone).
- `candidates=178761, patched=178761 (padding=3861, ud2=162)`, `notrap=9703, notrap_kept=162`. This confirms the handoff prediction: patched = 178772 − 11, the 11 being wide-store pairs consumed in one trampoline. N + K + pairs = 9876.
- wide-store-trap hits over 120 s: 16 384, vs 2 097 152 for best-2 (128× fewer SIGILL traps).
- fps 26.25 / 11.2 / 5.9 / 2.9 / 2.75 / 0.9, the same shape as best-2. Clean, timed out, stage 2.
- Animal Well with NOTRAP (45 s): 33/43, 34/44, 34/44 content/sampled, stage 2, 3/3 (eboot notrap=1143 kept=33, libc notrap=35).

**480 s confirmations (b3-label probe: J at 60 s / 3 s / 100 ms + REMAP/READBACK stats, plus F10 + J×12 bootstrap; best flags in every arm), fps per 40 s:**

| window | best480-3 (killed 293 s) | win480-3 (+GPU_INDIRECT +SPEC_READ_HOIST +DEPTH_READ_UNION) | notrap480-3 (+WIDE_STORE_NOTRAP) |
|---|---|---|---|
| 0-40 | 24.1 | 21.75 | 18.9 |
| 40-80 | 13.7 | 12.4 | 11.05 |
| 80-120 | 6.55 | 7.5 | 3.9 |
| 120-160 | 1.88 | 2.0 | 1.8 |
| 160-200 | 0.97 | 1.07 | 0.70 |
| 200-240 | 0.95 | 0.95 | 0.88 |
| 240-280 | 1.32 | 1.23 | 1.20 |
| 280-320 | (killed) | 1.65 | 1.60 |
| 320-360 | - | 1.77 | 1.62 |
| 360-400 | - | 1.40 | 1.48 |
| 400-440 | - | 1.50 | 1.60 |
| 440-480 | - | 1.40 | 1.55 |

- best480-3 was killed by the then-21 GB memmon limit (fp 22.5 GB, swap 8.5 GB); it is being rerun as shtsm-r7-best480-4.
- Scenes: win480-3 is on the phone message (frame 2160) and best480-3 in the in-engine room at 290 s. **Neither reached free walking.** win and notrap timed out cleanly, exc 0, quit 0.
- win480-3: GpuIndirect gpu=54/2 s, resolve 0, fallback not_mesh=252. Peak footprint 23.1 G, swap peak 9.5 GB.
- notrap480-3: trap hits 16 384 total in 480 s. RenderThread `_sigtramp` share in the 360 s sample is 5.7 % vs 14.7 % in win480-3 (target < 2 % not met; the remainder is presumably write-fault SIGSEGV). Peak footprint 24.0 G, swap 10.1 GB.
- In game (320-480 s), notrap ≈ win (1.48-1.62 vs 1.40-1.77). There is no same-run best baseline yet.

## 2026-10-12 ~04:40 IST — r7: same-route best480 baseline, entry 34 KYTY_GUEST_IMPORT (c9401c87) PROOF §8

**shtsm-r7-best480-4** (best flags, b3-label probe + F10/J×12, binary 9211bfe2; clean 480 s, no exception):
- fps per 40 s: 20.2 / 11.3 / 6.7 / 1.75 / 1.2 / 0.8 / 0.85 / 1.5 / 1.6 / 1.5 / 1.48 / 1.32.
- Peak footprint 22.6–23 G, swap 9.4 GB. Wide-store traps reach 4.19 M. RenderThread `_sigtramp` share 16.2 % in the 360 s sample.

**In-game 320–480 s, like for like (same route, same session), mean of the four 40 s windows:**

| arm | 320-360 | 360-400 | 400-440 | 440-480 | mean | vs best |
|---|---|---|---|---|---|---|
| best480-4 | 1.60 | 1.50 | 1.48 | 1.32 | 1.48 | – |
| win480-3 (+GPU_INDIRECT +SPEC_READ_HOIST +DEPTH_READ_UNION) | 1.77 | 1.40 | 1.50 | 1.40 | 1.52 | +3 % |
| notrap480-3 (+WIDE_STORE_NOTRAP) | 1.62 | 1.48 | 1.60 | 1.55 | 1.56 | +6 % |

- These are single runs, and the differences are within run-to-run noise. All three arms end on the phone message; none reached free walking.
- NOTRAP cuts RenderThread `_sigtramp` from 16.2 % to 5.7 %, the one mechanistic win.

**Entry 34 KYTY_GUEST_IMPORT:** `git apply --3way` hit 3 conflicted files, all mechanical, resolved by keeping both sides:
- bufferCache.h: `<memory>`+`<mutex>`, b4 members + import members.
- commandScheduler.cpp: includes.
- bufferCache.cpp: CompleteCallerReadbacks before the imported early-return, teardown of both, and the producer-tick block closed before the imported unmark block.

Build OK; committed c9401c87 (opt-in).

PROOF §8 runs (480 s, shtsm-ab-base-1 route F10 + J×12, PREFILL=2, SCRATCH_DESCRIPTOR, FRAME_TIMING; no best flags):

| window | ab-base-1 (hist.) | gi-1 | gi-storage-1 | ab-all-2 (hist.) | gi-b2-1 |
|---|---|---|---|---|---|
| 0-40 | 24.7 | 14.1 | 14.4 | 14.1 | 10.9 |
| 40-80 | 13.9 | 4.0 | 4.0 | 6.3 | 4.7 |
| 80-120 | 4.9 | 3.1 | 2.7 | 6.6 | 3.7 |
| 320-360 | 1.18 | 1.02 | 1.00 | 1.57 | 1.20 |
| 360-400 | 1.00 | 1.18 | 1.15 | 1.35 | 1.25 |
| 400-440 | 1.15 | 1.32 | 1.23 | 1.45 | 1.32 |
| 440-480 | 1.10 | 1.18 | 1.20 | 1.45 | 1.20 |
| mean 320-480 | 1.11 | 1.18 | 1.15 | 1.46 | 1.24 |

- Boot line: `VK_EXT_external_memory_host enabled, minImportedHostPointerAlignment=4096` (Rosetta 4 K pages). fb_vk=0 and fb_align=0 throughout; no "must be host-visible"; no exception; all runs timed out cleanly. The gi-1 last frame (1192) is the phone message and looks sane.
- **live_mib peaks at 201–233 MiB** (need ≥ 1000). No RAM win: footprint peak 22.0–23.0 G and swap 9.1–9.3 GB, the same as non-import runs.
- **war_wait sustained past 120 s:** gi-1 16–290 per 2 s, storage 8–201, b2 51–2114. `fb_demoted` climbs to 300–470. gi-1 RenderThread `_sigtramp` share is 44.7 % (write-after-read faults).
- Intro fps is roughly halved (40–120 s) in all three. In game, gi-b2 is −15 % vs ab-all-2; gi-1 is +6 % vs ab-base-1, but the baselines are from an older session.
- Animal Well with GUEST_IMPORT: 32/42, 33/43, 33/43 content/sampled, stage 2, 3/3. live_mib 12, created 0, no exception.
- **Verdict: kill criteria met** (war_wait > 20 per 2 s sustained past 120 s, also with CLASSES=storage; live_mib ≪ 1000; no memory gain). Entry 34 is a null result for SH → revert c9401c87, or leave it opt-in and never default; that is Alan's call.
