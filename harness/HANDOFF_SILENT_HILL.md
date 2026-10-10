# Handoff: Silent Hill: The Short Message (PPSA10112) on KytyPS5 / macOS

Written 2026-10-10 for continuing in Codex. Branch: `codex/animal-well-compat`.

## Current state

- Animal Well (PPSA22520) runs: stable gameplay, audio, ~60 FPS. Input lag fixed (`56852717`,
  see `harness/INPUT_LAG_FIX.md`). Do not regress it.
- Silent Hill: The Short Message **crashes ~3.5 s after launch** (exit code 65). Nothing is
  drawn; sampled frames are all black. Only the first blocker has been found. No code has been
  changed for this game yet.

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

Runs so far: `shtsm-first-1`, `shtsm-argbuf-1`, `shtsm-dump-1` (the last has the SPIR-V).

## Blocker 1 (found): MoltenVK rejects aliased buffer bindings

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
  DescriptorSet 0 / Binding 0**, both `Aliased`, as arrays of 2 storage buffers (one is a
  byte array, one a uint array). Vulkan allows this; Metal's SPIRV-Cross path does not.
- Emitted in `src/graphics/shader/recompiler/backend/spirv/spirvEmitterModule.cpp` ~lines 275-292:
  the `Buffers` kind loops over `{buffer_u8, buffer_u16, true, buffer_int64_atomics}`, defines one
  variable per alias via `Define(...)` with the same binding, and adds `DecorationAliased`.
- The `%buffers_u8` variable here is only used for `OpArrayLength` (13 references, no loads or stores
  in this shader), so a read-only workaround would unblock it.
- `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1` made no difference (tried in `shtsm-argbuf-1`).

Related code: `spirvEmitterMemory.cpp` lines ~336 and ~442 (`storage_buffers[bits == 8u ? 0 : 1]`),
`spirvEmitterMemoryHelpers.cpp` ~212-216, `spirvEmitterModule.cpp` ~144-150 and ~733-737.

### Fix options (none implemented)

1. Apple-only: when `buffer_u8`/`buffer_u16` are used, drop the extra aliased variables and emit
   byte/16-bit **loads** as 32-bit loads with shift and mask on the single `buffers` variable.
   Use `OpArrayLength` from that variable, adjusted to byte units.
2. Byte/16-bit **stores** need an atomic read-modify-write (`OpAtomicAnd`/`OpAtomicOr` on the
   containing uint); this is slower and needs care. This shader has no stores; others may.
3. Alternative: give each alias its own binding number and have the host bind the same buffer to
   each. This touches the descriptor layout and `IR::NativeBinding`; likely more invasive.
4. Keep any fix gated to Apple (`__APPLE__`) so Animal Well's validated path and other platforms
   don't change. Check the existing Apple gating in `pipelineCache.cpp` and `ShaderRecompiler.cpp`.
5. Separately consider making a failed pipeline non-fatal (log and skip the draw) so later
   problems surface in the same run. It is currently `Not implemented` at `shaders.cpp:632`.

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
