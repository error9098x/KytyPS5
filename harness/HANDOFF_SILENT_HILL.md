# Handoff: Silent Hill: The Short Message (PPSA10112) on KytyPS5 / macOS

Written 2026-10-10 for continuing in Codex. Branch: `codex/animal-well-compat`.

## Current state (updated 2026-10-10, end of session)

- Animal Well (PPSA22520) runs: stable gameplay, audio, ~60 FPS. Input lag fixed (`56852717`,
  see `harness/INPUT_LAG_FIX.md`). Re-verified after every change below (3 of 3 clean runs).
- Silent Hill: The Short Message (PPSA10112) **still does not render**; exits within 4 to 10 s.
  Fixed on the way: aliased buffer bindings (`5f8b23d4`), vertex subgroup builtins (`652d4247`),
  **mesh shaders** (now emulated through compute), **vertex ballot/shuffle** (one-lane subgroup
  mode), **MoltenVK push descriptors** (ordinary sets on every Apple title).
- What stops it now, in order (details in `harness/BLOCKERS.md`):
  1. Rosetta aborts on 256-bit AVX accesses to tracked pages; see `harness/ROSETTA_AVX_PAGE_PROTECTION.md`.
     Platform-level, reproduced in `harness/repro/rosetta_avx_straddle.c`.
  2. GPU `InvalidResource` / `Hang` errors after the first buffer-device-address draw.
  3. GPU-driven rendering needs GPU to CPU coherence, so tracking cannot just be disabled.

### Implemented this session (mesh emulation, "mesh via compute")

- `src/graphics/shader/meshEmulation.h`: scratch layout shared by both shaders.
- `src/graphics/shader/meshEmulationShader.{h,cpp}`: generated vertex shader that reads triangles
  and vertices from the scratch buffer by device address.
- `spirvEmitterMesh.cpp` (`EmitMeshComputeEntryPoint`): guest mesh program compiled as `GLCompute`.
- `pipeline/shaders.cpp`, `pipelineCache.*`: graphics pipeline with the generated vertex shader plus
  a compute pipeline on the same layout (`Pipeline::mesh_compute`).
- `renderDraw.cpp` (`ExecuteEmulatedMeshDraw`): per draw, compute dispatch, barrier, vertex draw.
- Enabled automatically when the device lacks `VK_EXT_mesh_shader`; `KYTY_DISABLE_MESH_EMULATION=1`
  turns it off, `KYTY_MESH_SCRATCH_MB` sizes the scratch (default 256).
- One-lane subgroup mode: `spirvEmitterHelpers.cpp` (`EmitSubgroupBallot/FindLsb/Shuffle`), applied
  only to Apple vertex programs that use ballot, shuffle or subgroup barriers.

### Debug switches added

`KYTY_MESH_TRACE`, `KYTY_MESH_VS_DUMP=<dir>`, `KYTY_MESH_EMU_SKIP=compute|draw|all`,
`KYTY_BUFFER_LOG`, `KYTY_PAGE_PROTECT_LOG`, `KYTY_FAULT_LOG`, `KYTY_NO_PAGE_PROTECT`,
`KYTY_ROSETTA_RUNAHEAD`, `KYTY_ENABLE_PUSH_DESCRIPTORS`.

### Not verified

- The emulated mesh output has never been seen on screen, so its correctness is unknown.
- `shader_recompiler_compute` and `shader_recompiler_alignbyte` cannot run on this Mac (they need
  fragment barycentrics and 64-bit LDS atomics); they were not run against these changes.

### Suggested next steps

1. Replace page-protection coherence (option 1 in `ROSETTA_AVX_PAGE_PROTECTION.md`), or stop
   running guest code under Rosetta. Nothing else removes the abort.
2. Bisect the GPU error further: it needs a per-encoder status from Metal (command buffer
   `errorOptions` with encoder execution status) to name the failing command.
3. Only then judge the emulated mesh draw visually.

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

## Update 2026-10-10 (later): mesh-shader blocker analysis and proposed fix

`KYTY_MESH_TRACE=1` (new, opt-in, `src/graphics/shader/shader.cpp` `PrepareProgram`) logs why a
draw takes the mesh path. Silent Hill's first mesh draw (run `shtsm-mesh-trace`):

```
MeshTrace: hash=4e991d9d461f621d merged=1 native_ngg=0 type=4 fast_launch=0 prim=6 gs_out_prim=2
gs_max_vert_out=3 max_vertices=192 max_prims=64 prims_per_group=62 verts_per_group=64 threads=192
lds_dw=2176 wave=64 gs_front=1
```

So it is a real ES+GS pair (`merged=1`, GS front+back), triangle-strip output, max 3 vertices
out per input primitive (one triangle each), 64 input verts / 62 prims per group, 192 threads,
8.7 KB LDS. Kyty runs this only as a Vulkan mesh shader; MoltenVK has no `VK_EXT_mesh_shader`.

### Proposed fix: "mesh via compute" (not implemented)

The emitter is well isolated (`spirvEmitterMesh.cpp`, ~230 lines): the guest function writes
per-lane outputs into Private/Workgroup arrays, and only `EmitMeshEntryPoint` calls
`OpSetMeshOutputsEXT` and copies to Output arrays. Plan:

1. Recompiler: add an Apple option to emit the Mesh program as `GLCompute` with the same
   LocalSize. In `DefineMeshOutputs`/`EmitMeshEntryPoint`, replace Output arrays with stores into
   an SSBO: vertex records at `(group * max_vertices + i)` (position, params, clip/cull, layer)
   and triangle records at `(group * max_primitives + i)` as `uvec4(v0, v1, v2, flags)`; write a
   "dead" marker for `i >= primitives` or the null/cull bit. `group = WorkgroupId.x +
   WorkgroupId.y * NumWorkgroups.x` (Y = instance, matching `drawMeshTasksEXT(groups, instances, 1)`).
   Drop the MeshShadingEXT capability/extension and mesh execution modes.
2. Recompiler: generate a passthrough vertex shader. `prim = gl_VertexIndex / 3`,
   `corner = gl_VertexIndex % 3`; read the triangle; if dead, output a degenerate position;
   otherwise read the vertex record and write `gl_Position`, the same `Location`s as the mesh
   outputs (so the pixel shader is unchanged), clip/cull distances, and layer.
3. Host: `NativeBinding` maps both Mesh and Compute to binding group 0, so the compute variant
   can reuse the mesh stage's descriptor slots. Add `eCompute` to the stage flags used for that
   layout, or build a compute layout from the same bindings.
4. Draw path (`renderDraw.cpp` `ExecutePreparedDraw`, the `mesh_active` branch): allocate or
   grow a scratch SSBO; `EndRendering()`; bind the compute pipeline, commit bindings with
   `eCompute`, push the same `draw_data`; `dispatch(mesh_groups, instance_count, 1)`; add a
   barrier (compute write -> vertex shader read); then draw
   `mesh_groups * instance_count * max_primitives * 3` vertices with the passthrough pipeline.
   Rendering is begun lazily before each draw, so ending it before the dispatch fits the
   existing structure.
5. Gate everything to Apple when `!mesh_shader_enabled`; keep the real mesh path elsewhere.

Rough size: 600–1000 lines across the recompiler, pipeline creation, and the draw path.
Risks: per-draw render-pass splits (performance), primitive restart (already unimplemented on
the mesh path), provoking-vertex/flat semantics, and whatever Unreal Engine blockers come next.
