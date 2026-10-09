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
