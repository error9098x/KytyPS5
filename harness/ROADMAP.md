# Animal Well on KytyPS5: progress, where to continue, and what it takes to finish

Status date: 2026-10-10. Read `harness/ANALYSIS.md` first for how everything works and the evidence.
Acceptance stages (from the mission): 1 first frame, 2 title/menu, 3 gameplay 10 min,
4 visually correct, 5 performance (>=30 FPS, no shader stutter spikes).

---------------------------------------------------------------------------------------------------

## 1. Where we are

| Stage | State | Evidence |
|---|---|---|
| Build + launch on macOS (x86-64/Rosetta + MoltenVK) | **Done** | `run.sh` runs reproducibly |
| Boot without crash, assets load, game logic runs | **Done** | 60-90 s runs, no crash; atlas readback perfect; sprites animate |
| 1. First frame presented | **Done** | Three clean 20 s runs: 27 sampled frames each, 17 with content |
| 2. Title / menu visible | **Reached** | Captured 3840x2160 frame visibly shows Animal Well CRT title artwork |
| 3. Gameplay stability | **Measured once** | One 10-minute Cross-input soak ended cleanly; repeat twice |
| 4. Visual correctness | **Pending** | Need fixed-point reference captures and image diffs |
| 5. Performance | **Baseline only** | 59.12 FPS median in one timing run; startup and p95 spikes remain |

The original black-frame failure is fixed for PPSA22520 on Apple hosts by selecting ordinary
descriptor sets instead of MoltenVK push descriptors. The remaining work is quality and
measurement: repeat the gameplay soak, verify audio on the selected host device, obtain matched
input-to-present samples from a focused keyboard or controller route, and compare screenshots.

### Done, committed
* Apple/MoltenVK device-feature gating (3 missing features made optional, with explicit errors).
* NGS2 mastering gain + `sceNgs2RackGetInfo` + `sceNgs2VoiceGetPortInfo` (tests pass).
* Game-gated vertex subgroup fallback (harmless for this game's VS; keeps MoltenVK from rejecting it).
* Measurement harness (`run.sh`, scanners, stage tracker) + ~15 env-gated diagnostics
  (frame readback, internal render-target readback with PNG, sampled-texture readback, vertex
  probes, GPU-vs-guest vertex compare, viewport probe, solid-PS, clip-guard and depth-clamp switches).
* Docs: `SPEC.md`, `LOG.md`, `BLOCKERS.md`, `ANALYSIS.md`, this file.

### Not done
* Two more 10-minute gameplay soaks, fixed-point visual comparisons, and sustained performance work.
* Audible confirmation on the user's selected macOS playback device.
* A matched input-to-present route using the new keyboard, mouse, and SDL controller timestamps.
* The 108 unresolved imports (never called in the measured route) remain intentionally deferred.
* Keep the local save backup at `harness/backups/animal-well-20261010/` intact;
  its `MANIFEST.sha256` covers the two save files. No separate repository or
  standard macOS application-support config was found.

---------------------------------------------------------------------------------------------------

## 2. The narrowed failure band

Measured facts that any explanation must satisfy:

1. Draws are issued with valid geometry, valid vertex attributes (fmt/offset/stride correct),
   GPU-side vertex buffers identical to guest memory, a correct atlas texture, valid scalar constants.
2. The vertex shader SPIR-V is valid and its math is right (`pos=(2x/320-1, 1-2y/180, 0, 1)`).
3. After the draws, the color target equals its **clear value exactly** (0,0,0,0 / 0,0,0,255 /
   128,128,255,255), for every pixel-shader hash, including the final 4K pass.
4. Forcing every pixel shader to output opaque magenta does **not** change that, and the presented
   4K frames stay black. => No fragment from any draw reaches any attachment.
5. Not caused by: viewport sign, clip guard, depth clamp, pixel kill, imports, lane fallback.
6. MoltenVK enabled only: `KHR_maintenance1`, `KHR_portability_subset`, `KHR_push_descriptor`,
   `KHR_swapchain`, `EXT_depth_clip_control`, `EXT_memory_budget`, `EXT_robustness2`. Dynamic
   rendering is used via Vulkan 1.3 core. Pipeline cache is disabled ("dirty build").
7. Anomaly: viewport depth is `[-1, 1]` for every draw (illegal Vulkan without
   `VK_EXT_depth_range_unrestricted`). Clamping did not help, but it is a latent bug.

Therefore the root cause is one of: (a) a pipeline/render-state element that makes Metal discard
everything for these pipelines; (b) draws recorded to an attachment/image other than the one read
back; (c) a MoltenVK/Metal translation or portability-subset issue; (d) the vertex shader's
*runtime* outputs (position) not being what the SPIR-V implies. Section 3 is ordered to separate these.

---------------------------------------------------------------------------------------------------

## 3. Next steps, in order (each has a pass/fail signal)

**Step 1 - Turn on Vulkan validation and Metal diagnostics (cheap, may name the bug immediately).**
* Install the LunarG Vulkan SDK for macOS (universal binaries incl. `VK_LAYER_KHRONOS_validation`),
  put its layer on the loader path, run with `--vulkan-validation true` (the harness passes `false`
  today; add `KYTY_VULKAN_VALIDATION=1` to `run.sh`). The x86-64 emulator needs an x86-64 slice.
* Set `MVK_CONFIG_LOG_LEVEL=3` (or higher) and `MVK_CONFIG_TRACE_VULKAN_CALLS=1` for a short run to
  surface MoltenVK warnings (unsupported state, invalid viewport, ignored dynamic state).
* Pass: validation/MoltenVK names an invalid or unsupported state on the draw path (depth range,
  portability subset, dynamic rendering, push descriptors, pipeline state).

**Step 2 - Fullscreen-VS bisect (decides vertex-side vs raster/attachment-side).**
* Add `KYTY_DEBUG_FULLSCREEN_VS=1` in `spirvEmitterFlow.cpp` next to `KYTY_DEBUG_SOLID_PS`: for vertex
  shaders, overwrite `gl_Position` with a fullscreen triangle derived from `gl_VertexIndex`
  (`(x,y) = ((i<<1)&2, i&2)*2-1`, z=0, w=1). Combine with `KYTY_DEBUG_SOLID_PS=1`.
* Pass: magenta appears in render targets -> the VS's *runtime* position output/inputs are wrong
  (go to Step 3). Still nothing -> the problem is raster/attachment/pipeline (go to Step 4).

**Step 3 - If Step 2 shows pixels: find why the real positions are lost.**
* Output the VS's computed values to a debug target or compare `in_attr_0` as seen in the shader
  (e.g. write `in_attr_0.xy/320` as color to a debug draw). Suspects: SPIRV-Cross handling of
  `OpBitcast`/`OpSelect` on uint-packed floats, `ClipDistance` array of size 1 combined with
  `PerVertex`, the always-true lane mask (`0xFFFFFFFF >> (v&31)`) mis-lowered, `vertexAttributeAccessBeyondStride`
  portability limits.

**Step 4 - If Step 2 shows nothing: audit the attachment/pipeline path.**
* Check `RenderState` -> `vkCmdBeginRendering` for the target image/view (layer, mip, format,
  `is_clear` flapping), and whether the image drawn into is the same `ImageId`/`VkImage` that is
  later sampled and read back. Log `VkImage`/`VkImageView` handles for the draw attachment and for the
  readback.
* Try a minimal Kyty-internal draw (the `blitHelper.cpp` fullscreen-triangle pipeline) into the same
  target with the same `BeginRendering` path: if it draws, the difference is in the game pipelines'
  state (blend, color write mask via guest `cb_shader_mask`, depth/stencil, MSAA, rasterizer discard);
  if not, `BeginRendering`/image state is wrong.
* Print the full `VkGraphicsPipelineCreateInfo` for one sprite draw (rasterizer, blend, depth, MSAA,
  dynamic states) and check it against the Metal limits listed under `VK_KHR_portability_subset`.
* Capture one frame in Xcode (`MTL_CAPTURE_ENABLED=1`, GPU capture) and inspect the draw: vertex
  output positions, rasterized fragments, load/store actions, attachment contents.

**Step 5 - Fix the depth-range bug regardless.** Legalize the viewport depth for Metal
(`dx_clip_space=0`, guest `[-1,1]`): either use `VK_EXT_depth_clip_control` semantics correctly
(`negativeOneToOne` plus a legal `[0,1]` viewport range) or remap. Put it behind an Apple check; do
not change other platforms without verification.

---------------------------------------------------------------------------------------------------

## 4. What happens after the first pixels appear (expect more work)

Each item will likely reveal its own issue; handle them as a loop with the harness gate
("target/frame `content=yes`, then PNG compared to a reference").

1. **Final composite correctness:** the 4K pass is an sRGB CRT/upscale shader that samples 320x180
   targets; verify sRGB/UNORM handling, the sprite colorkey discard (cyan), blend modes, and the
   Metal behavior of `OpGroupNonUniformBallot` in pixel shaders (used for the discard/WQM logic).
2. **Stage 2 (title/menu):** check that the title screen appears and animates. If the game waits
   for input, verify the pad library (`libPad`/user service) delivers events and map keyboard/controller.
   Check the audio path (NGS2/AudioOut) does not stall the game loop.
3. **Stage 3 (10 min stability):** run 10-minute soak runs x3; watch memory growth, the 1024-call
   unresolved-stub log cap, GPU resource leaks, swapchain recovery, any now-reachable stubs
   (resolve NIDs of the 109 imports and implement those the game actually hits).
4. **Stage 4 (visual correctness):** save reference PNGs at fixed points, add an image-diff check to the
   harness, compare internal targets (lighting, normals, bloom) for glitches.
5. **Stage 5 (performance):** 4K output (3840x2160) is heavy on a base M-series GPU while the game
   renders 320x180 internally: add a resolution/present scale option, disable diagnostics
   (`KYTY_FRAME_STATS=0`; the readbacks stall the GPU), enable the Vulkan pipeline cache and a
   persistent shader cache (62 shaders compile at startup; "Vulkan pipeline cache: disabled (dirty
   build)"), profile the CPU side (Rosetta) and the per-draw descriptor/upload costs. Target: >=30 FPS
   average with no stutter above an agreed budget (set it with the user; the mission left [N] unset).

---------------------------------------------------------------------------------------------------

## 5. What it would take to fully fix the game (honest estimate)

* **Biggest unknown:** one root cause for "nothing rasterizes". Steps 1-4 are designed to find it in
  a few focused sessions; the evidence says it is a single systemic issue (it affects every draw),
  not many shader bugs. Once fixed, large parts of the scene should appear at once.
* **Then:** a tail of correctness bugs (MoltenVK vs RDNA2 semantic differences, depth range, sRGB,
  subgroup ops), input mapping, audio, and performance. Typically this tail is longer than the first
  fix; plan for iterative harness-driven loops per stage.
* **Risk areas:** MoltenVK limitations that cannot be worked around inside Kyty (portability subset
  gaps) might force shader-side emulation; Rosetta CPU overhead may cap performance; macOS support in
  Kyty is experimental upstream, so expect to fix unrelated Apple-only bugs and keep every change
  gated so other platforms/titles are unaffected.
* **Process to keep:** one fix per commit, before/after runs x3 using `scan.txt` (`content=` and
  presented frames), reviewer pass on each diff, log every stub, update `LOG.md`, back up `_SaveData/`.

---------------------------------------------------------------------------------------------------

## 6. Quick reference for the next session

* Build: see `ANALYSIS.md` section 2. Run: `./harness/run.sh name` (`KYTY_TIMEOUT_SECONDS`, `KYTY_FRAME_DUMP=1`).
* Fast target check (the key measurement):
  `KYTY_INTERNAL_FRAME_STATS=1 KYTY_INTERNAL_FRAME_SAMPLED=1 KYTY_INTERNAL_FRAME_SKIP=300 KYTY_INTERNAL_FRAME_MAX=60 KYTY_INTERNAL_FRAME_DUMP_DIR=<dir> ./harness/run.sh x`
  then summarize `InternalFrameStats` lines (role, extent, `rgb_nonblack`) and view the PNGs.
* Per-draw geometry: `KYTY_INTERNAL_FRAME_VERTICES=1 KYTY_INTERNAL_FRAME_PIXEL_HASH=0xbe7777460c3444ff`.
* Key shader hashes: VS `0xcdcf88828e5c2df8` (all draws), sprite PS `0xbe7777460c3444ff`, final 4K PS
  `0xd89c2250b89200d0`. Current SPIR-V is in `harness/runs/solid-spv/shaders/`.
* Do not: `git add harness` or `git add .`; run `KYTY_GRAPHICS_DEBUG_DUMP` for more than ~10 s; trust
  a SPIR-V dump from an older build; judge progress by "no crash" alone.
* Provenance note for the user: the archive is tagged `[SuperPSX]` and contains modified files
  (`.bak_*`); confirm it came from your own console dump and keep the files private.
