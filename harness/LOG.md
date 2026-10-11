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
