# Handoff: Silent Hill: The Short Message (PPSA10112) on KytyPS5 / macOS

Written 2026-10-10 for continuing in Codex. Branch: `codex/animal-well-compat`.

## Current state (updated 2026-10-10)

- Animal Well (PPSA22520) runs: stable gameplay, audio, ~60 FPS. Input lag fixed (`56852717`,
  see `harness/INPUT_LAG_FIX.md`). Do not regress it.
- Silent Hill: The Short Message (PPSA10112) still exits 65, but now **three blockers in**: the
  first two are fixed and it reliably reaches the third, a hard host limit.
  1. ~~MoltenVK rejects aliased buffer bindings~~ **FIXED** (`5f8b23d4`): on Apple, emit only the
     32-bit storage-buffer view and emulate sub-word access via shift/mask.
  2. ~~Vertex shader reads `SubgroupLocalInvocationId`~~ **FIXED** (`652d4247`): the Apple
     vertex-subgroup fallback is no longer PPSA22520-only; it also covers PPSA10112.
  3. **Mesh shaders — HARD HOST LIMIT.** Reaches
     `EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled)` (`pipelineCache.cpp:625`). MoltenVK
     1.4.2 has no `VK_EXT_mesh_shader`; KosmicKrisp has it but is arm64-only; Kyty executes PS5
     x86-64 guest code natively and cannot run as arm64. Full analysis in `harness/BLOCKERS.md`.
  Progress before stopping: VS 4 / PS 3 / CS 16 shaders compiled, past engine config into ATRAC9
  audio decode. Verification: 3 consecutive runs all stop at the same mesh-shader line; Animal Well
  regresses on none.

## Game data

- Source archive: `~/Downloads/[SuperPSX]-SILENT.HILL.The.Short.Message-PPSA10112-ASIA-Game-v1.001-PS5.rar`
  (password `SuperPSX`; tested OK, 135 entries; extracted with `unar`, not 7-Zip, which can't read this RAR5 variant).
- Extracted to: `/Users/aviral/Documents/ChatGPT/PS5/SilentHill.ShortMessage-extracted/PPSA10112-app`
  (18 GB, 124 files; `eboot.bin` 148 MB; Unreal Engine paks `pakchunk0-ps5.ucas` 13 GB + `.pak` 2.8 GB).
- `param.json`: title "SILENT HILL: The Short Message", PPSA10112, content version 01.000.001.
- No `.bak` files, so it looks unmodified. It is a third-party (SuperPSX) copy, not a dump from the user's own console.

## Build and run

x86-64 under Rosetta is the only working macOS target (native arm64 fails in
`src/common/hostException.cpp` and the x64 instruction emulator).

```bash
cd /Users/aviral/Documents/ChatGPT/PS5/KytyPS5
cmake --build _Build/macos-x86_64 --target kyty_emulator -j6

KYTY_GAME_DIR=/Users/aviral/Documents/ChatGPT/PS5/SilentHill.ShortMessage-extracted/PPSA10112-app \
KYTY_FRAME_STATS=1 KYTY_FRAME_TIMING=1 KYTY_PRINTF_DIRECTION=File KYTY_TIMEOUT_SECONDS=60 \
./harness/run.sh shtsm-run-N
```

`harness/run.sh` defaults to the Animal Well directory; `KYTY_GAME_DIR` overrides it. It sets the
x86-64 MoltenVK 1.4.2 dylib (`harness/tools/MoltenVK-1.4.2/libMoltenVK.dylib`) via `SDL_VULKAN_LIBRARY`.
Output goes to `harness/runs/<name>/` (`kyty.log`, `guest-printf.log`, `scan.txt`, `run.json`).
Add `KYTY_GRAPHICS_DEBUG_DUMP=true KYTY_SHADER_LOG_DIRECTION=File` to dump SPIR-V into
`harness/runs/<name>/shaders/`.

Runs so far: `shtsm-first-1`, `shtsm-argbuf-1`, `shtsm-dump-1` (SPIR-V), `shtsm-fix1`,
`shtsm-fix2`, `shtsm-verify-1..3`.

## Blocker 1 (FIXED, `5f8b23d4`): MoltenVK rejected aliased buffer bindings

Log (`harness/runs/shtsm-first-1/kyty.log`):

```
[mvk-error] SPIR-V to MSL conversion error: Attempting to alias same binding with a descriptor
array which is not implemented through argument buffers. This is unsupported.
[mvk-error] VK_ERROR_INITIALIZATION_FAILED: Compute shader function could not be compiled into pipeline.
--- Fatal Error ---
Not implemented (result != vk::Result::eSuccess) in src/graphics/host_gpu/renderer/pipeline/shaders.cpp:632
```

- Failing shader: `harness/runs/shtsm-dump-1/shaders/0007_new_shader_cs_80b8f1b01227d2c4.spv`
  (6 other compute shaders and 1 vertex shader compile fine).
- Cause: it declares `%buffers_u8` and `%buffers` as two variables at the **same
  DescriptorSet 0 / Binding 0**, both `Aliased`. Vulkan allows this; Metal's SPIRV-Cross path does not.
- **Fix applied** (option 1 below, Apple-gated via `UseSubwordStorageBufferViews()` in
  `spirvEmitterInternal.h`): declare only the 32-bit view; sub-word access goes through the
  existing shift/mask path in `Load/StoreSubwordInBounds`. `pipelineCache.cpp`'s
  `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1` experiment (`shtsm-argbuf-1`) made no difference.

Touched: `spirvEmitterInternal.h`, `spirvEmitterModule.cpp` (declaration + capabilities),
`spirvEmitterMemoryHelpers.cpp` (view selection).

## Blocker 2 (FIXED, `652d4247`): vertex shader subgroup builtins

`[mvk-error] SPIR-V to MSL conversion error: Subgroup builtins are not available in this type of
function.` — a vertex shader loads `SubgroupLocalInvocationId`
(`shtsm-sgdump/shaders/0005_new_shader_vs_*.spv`), used only as `(0xFFFFFFFF >> (lane & 31)) & 1`,
which is always 1. The Apple vertex-subgroup fallback (lane id from the vertex index) existed but was
PPSA22520-only; named it `PipelineCacheNeedsVertexSubgroupFallback()` and added PPSA10112.

## Blocker 3 (OPEN, HARD HOST LIMIT): mesh shaders

`EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled)` — see `harness/BLOCKERS.md`. This machine
cannot provide host mesh shaders to an x86-64 Kyty. No Kyty-side change is available (the NGG path
has no non-mesh lowering), so this needs an external capability, not an emulator edit.


## Expected next problems (not yet observed)

Unreal Engine title: many shaders, large streaming, many more libraries. Likely: more unsupported
shader features (the Animal Well-style missing Vulkan features still apply: `minLod`,
`shaderBufferInt64Atomics`, `shaderCullDistance`), unimplemented imports, memory pressure with
18 GB of paks on a 16 GB machine, and a long startup. `mkdir(.../shpc/intermediate/datasmithcontenttemp)
failed: 2` appeared in the log; it is probably harmless (missing directory) but unverified.

## Animal Well changes already in the branch (context)

- Vulkan: unsupported Apple features made optional with explicit errors if used
  (`vulkanWindow.cpp`, `graphicContext.h`, `imageView.cpp`, `pipelineCache.cpp`).
- Vertex subgroup-id fallback via `gl_VertexIndex` (Apple + PPSA22520 gated).
- NGS2 audio: voice query/port info/mastering gain, graph ordering, output writeback.
- Immediate present mode for PPSA22520 on Apple (`KYTY_PPSA22520_MAILBOX` restores Mailbox).
- `--audio-device` option. The host Mac was muted (volume 0) at one point; check the system volume.
- Controller coalescing fix in `src/libs/controller.cpp`.
- Opt-in diagnostics (env vars): `KYTY_FRAME_STATS`, `KYTY_FRAME_TIMING`, `KYTY_INPUT_STATS`,
  `KYTY_AUDIO_STATS`, `KYTY_PIPELINE_TIMING`, `KYTY_MEMORY_TIMING`, `KYTY_GPU_SUBMIT_TIMING`,
  `KYTY_GPU_PROCESS_TIMING`, `KYTY_PRESENT_TIMING`, `KYTY_SUSPEND_TIMING`,
  `KYTY_COMMAND_STATS`, `KYTY_DRAW_INPUT_STATS`, `KYTY_DRAW_RESOURCE_STATS`,
  `KYTY_GPU_HARD_SYNC`, `KYTY_MAX_PENDING_FLIPS`, `KYTY_SWAPCHAIN_IMAGE_COUNT`, `KYTY_VBLANK_FREQUENCY`.
- Other docs: `harness/REPORT.md`, `LOG.md`, `BLOCKERS.md`, `ROADMAP.md`, `SPEC.md`, `INPUT_LAG_FIX.md`.

## Repo state at handoff

- HEAD `77a8a8fd` plus this file. `_DownloadData/` is an untracked runtime folder (not committed).
- The build tree reports `-dirty` and so the Vulkan pipeline cache is disabled in logs
  ("Vulkan pipeline cache: disabled (dirty build)") until the tree is clean; `_DownloadData/`
  being untracked may be what trips this.
- Tests: `ctest --test-dir _Build/macos-x86_64 -R 'ngs2_sampler|shader_cfg' --output-on-failure`.

## Suggested first steps

1. Implement option 1 (Apple-gated, read-only) and rerun `shtsm-run`; confirm the shader compiles.
2. Make pipeline-creation failure non-fatal for diagnosis, then list every failing shader in one run.
3. Re-test Animal Well (3 runs, all-nonblack frames, audio nonzero) to confirm no regression.
4. Repeat: run, find the first blocker, fix, commit, log in `harness/LOG.md`.
