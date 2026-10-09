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
  (59.12 FPS), p95 33.37 ms, with a 2.96 s startup/shader outlier.
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
  uncapped pacing probe; the default remains Mailbox.
- Controlled soak runs can set `KYTY_ROUTE=gameplay`; `run.json` preserves that
  marker so Stage 3 is never awarded from a long title-only run by accident.
- Three Immediate-present probes completed without crashes: median FPS
  `60.71`, `60.42`, and `60.31`; p95 frame times were `20.34`, `20.32`, and
  `19.83` ms. Each retained a single startup shader spike between `2.73` and
  `3.21` seconds.
- A clean Release build now loads `_PipelineCache/PPSA22520.bin` (1,144,892
  bytes) on all three follow-up probes. The cache is active, but it does not
  remove the initial shader-compilation spike.
- The harness now defaults shader logging to `Silent`; set
  `KYTY_SHADER_LOG_DIRECTION=File` for full IR dumps. Three Silent Immediate
  probes measured 60.05–60.40 FPS median and 2.77–3.94 s max frame gaps, so
  suppressing diagnostic dumps improves normal harness overhead but does not
  remove the first-use MoltenVK/scene stall.

## Gameplay stability gate — 2026-10-10

- `gameplay-gate-3-20261010`, `gameplay-gate-4-20261010`, and
  `gameplay-gate-5-20261010` each ran for 600 seconds with route marker
  `gameplay`, exit code 0, and no fatal, unimplemented, or GPU-validation
  errors.
- MacBook Air Speakers were selected explicitly. Nonzero SDL/NGS2 samples were
  observed in all three runs: SDL `1949/3368`, `2071/3357`, and `2131/3366`;
  NGS2 `648/1123`, `689/1119`, and `709/1122`.

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
