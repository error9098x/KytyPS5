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
- Three 30-second focused probes delivered 24 controller events each. Their
  input-to-next-present results were median/p95/max: `5.80/45.76/45.77` ms,
  `11.51/118.49/118.50` ms, and `9.83/42.20/42.21` ms.
- Input-to-guest-poll results were `11.01/33.62/33.62` ms,
  `13.86/98.61/98.62` ms, and `10.61/25.79/25.80` ms. No one-second delay
  appeared in these controlled probes; visible-state/gameplay response remains
  the unresolved part of the user’s report.

## Deterministic input probe — 2026-10-10

- Added opt-in `KYTY_INPUT_PROBE=1`. It injects a keyboard press/release through
  the same `HostInputKey` path as a real SDL event, so latency probes do not
  depend on a focused window, a connected controller, or an unlocked desktop.
  `KYTY_INPUT_PROBE_KEY`, `KYTY_INPUT_PROBE_START_MS`,
  `KYTY_INPUT_PROBE_INTERVAL_MS`, and `KYTY_INPUT_PROBE_HOLD_MS` tune it; the
  default key is `J` and the first event is delayed by 2 seconds.
- One 30-second immediate-present run with the default start contained
  title-to-gameplay host-to-present samples of `1187.4`, `668.2`, and `232.2` ms.
  Matching guest-poll samples were `1006.0`, `671.1`, and `486.8` ms. The first
  transition loads and compiles a large shader batch, which is the concrete
  source of the roughly one-second host-path samples.
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
