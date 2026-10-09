# Animal Well on KytyPS5 (macOS, Apple Silicon): what was done and what we learned

Status date: 2026-10-09. Branch `codex/animal-well-compat`. Title PPSA22520 v01.000.011.
Companion file: `harness/ROADMAP.md` (progress, next steps, what is needed to finish).

Evidence labels used below: **[verified]** = measured/observed in a run or read in code;
**[inferred]** = reasoned but not directly tested; **[rejected]** = tested and ruled out.

---------------------------------------------------------------------------------------------------

## 1. One-paragraph summary

Animal Well now boots under Kyty on this Mac, loads all its assets, runs its CPU logic (animation
is visibly advancing), creates dozens of pipelines, issues thousands of draws per second, and
presents 3840x2160 frames at a steady rate. **Every presented frame is 100% black.** Intermediate
render targets prove the GPU work is not being delivered: draws with valid vertices, a valid
texture atlas and even a forced solid-magenta pixel shader leave their render targets at exactly
their clear value. The failure is therefore at (or after) vertex/raster/attachment-store, not in
asset loading, not in imports, and not in the pixel shader's math. The root cause was MoltenVK's
push-descriptor path: Animal Well's descriptor arrays produced unusable storage-buffer/image
bindings. Selecting ordinary descriptor sets for `PPSA22520` on Apple hosts restores the original
guest shader resources. Three clean 20-second runs now show the CRT title artwork; section 7 still
lists follow-up work for menu input, longer stability, and performance.

---------------------------------------------------------------------------------------------------

## 2. Environment and how to build/run

| Item | Value |
|---|---|
| Host | Apple Silicon Mac17,3, macOS 26.5.1, 16 GiB |
| Kyty base commit | `4c8fc53a` (upstream), work on `codex/animal-well-compat` |
| Build dir | `_Build/macos-x86_64` (Release, Ninja, `CMAKE_OSX_ARCHITECTURES=x86_64`, `KYTY_BUILD_LAUNCHER=OFF`) |
| Game dir | `../Animal.Well-extracted-unar/[SuperPSX]-Animal.Well-PPSA22520-JPN-Game (v01.000.011)-PS5` |
| Vulkan | MoltenVK 1.4.2 x86-64 dylib at `/tmp/mvk-x86/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib` (**`/tmp` is volatile**) |

**Why x86-64:** the native ARM64 build fails in `src/common/hostException.cpp` (it uses x86 thread
state fields `__rip/__err`). Kyty executes PS5 guest code natively as x86-64, so the emulator itself
must be x86-64 and runs under Rosetta. **[verified]**

**Why a downloaded MoltenVK:** Homebrew's MoltenVK/loader are ARM64-only, so the Rosetta process
cannot load them (`Failed to load Vulkan Portability library`, exit 65). The official release
`MoltenVK-all.tar` contains a universal dylib. Set `SDL_VULKAN_LIBRARY` (the harness does this).
`curl` downloads of the 1.4.2 tarball were unreliable; `aria2c -x16` worked. **[verified]**

Build (background it; foreground tool calls time out ~2 min):
```
cmake -S . -B _Build/macos-x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DKYTY_BUILD_LAUNCHER=OFF
cmake --build _Build/macos-x86_64 --target kyty_emulator --parallel 8
```
Run: `./harness/run.sh <name>` (30 s default; `KYTY_TIMEOUT_SECONDS=N`). Output in `harness/runs/<name>/`.

Hazards: never `git add harness` / `git add .` (run folders are huge; `harness/runs/` is now
git-ignored). `KYTY_GRAPHICS_DEBUG_DUMP=true` writes ~70 MB/s of log (a 60 s run = 4 GB), so keep
those runs to ~10 s and delete the log. The Mac may be locked, which breaks screenshots; use the
frame readbacks below instead.

---------------------------------------------------------------------------------------------------

## 3. How Kyty works (as far as we verified from code)

Pipeline, top to bottom:

1. **Loader / runtime linker** (`src/loader/runtimeLinker.cpp`): loads the guest ELF/SELF
   (`eboot.bin`, `sce_module`, `fakelib`), relocates it, resolves imports by NID hash against the
   emulated libraries (`LIB_FUNC("<NID>", fn)` in `src/libs/*.cpp`). **Unresolved imports are patched
   to a thunk** (`UnresolvedImportStub`) that logs the first 1024 calls and returns 0, instead of
   failing at load. Load-time lines read `Relocate: unresolved ... import patched to stub`.
2. **Guest CPU:** x86-64 code runs natively (hence Rosetta on Apple Silicon). Host exceptions are
   caught in `src/common/hostException.cpp`.
3. **Guest GPU command stream:** the game builds PM4 packets (AGC API). Kyty's command processor
   (`src/graphics/guest_gpu/command_processor/pm4*.cpp`, `graphicsRun.cpp`) decodes them into
   draw/dispatch/flip operations with captured register state (viewport, render targets, blend...).
4. **Renderer** (`src/graphics/host_gpu/renderer/`): `renderDraw.cpp` (`ExecutePreparedDraw`),
   `renderCompute.cpp`, `context.cpp` (`CommandBuffer::BeginRendering`, uses
   `vkCmdBeginRendering` / dynamic rendering), `commandScheduler.cpp`, `sync.cpp` (EOP events,
   flips). Caches: `cache/bufferCache.cpp` (guest range -> host VkBuffer with a memory tracker:
   only ranges marked CPU-modified are re-uploaded), `cache/textureCache.cpp` (guest images ->
   VkImage, tiling/untiling, render-target/texture/video-out roles), `cache/streamBuffer.cpp`
   (VMA host buffers), `pipeline/pipelineCache.cpp`, `pipeline/shaders.cpp` (VkPipeline creation:
   vertex input, rasterizer, blend), `pipeline/descriptors.cpp` (descriptor materialization).
5. **Shader recompiler** (`src/graphics/shader/recompiler/`): decodes RDNA2 machine code ->
   IR (CFG, SSA) -> SPIR-V (`backend/spirv/*`). Guest shader hash identifies each shader
   (`ShaderRecompiler VS/PS ... hash=0x...` in the log). SPIR-V goes to MoltenVK (SPIRV-Cross)
   -> Metal.
6. **Presentation:** `VideoOutSubmitFlip` -> `FlipQueue` (`presentation/videoOut.cpp`) ->
   `Presenter::Present` -> `swapchain.cpp` (blit/scale the video-out image to the swapchain and
   `vkQueuePresentKHR`). `window.loop.presented_frames` counts presents.
7. **Audio:** `src/libs/ngs2.cpp` (NGS2 racks/voices/mastering), `libAudio.cpp`.

Logging: `LOGF` goes to the file chosen by `--printf-direction`; the harness sends it to
`guest-printf.log` (NOT `kyty.log`, which only has console prints). This tripped us up (section 8).

---------------------------------------------------------------------------------------------------

## 4. Code changes made (all on the branch)

| # | Area | Files | What / why |
|---|---|---|---|
| 1 | Apple Vulkan features | `vulkanWindow.cpp`, `graphicContext.h`, `imageView.cpp`, `pipelineCache.cpp` | Apple GPUs lack `imageViewMinLod`, `shaderBufferInt64Atomics`, `shaderCullDistance`; Kyty required them (device selection failed). Made optional, with an explicit error if a shader really uses one (reviewer found a silent-ignore bug in v1). |
| 2 | NGS2 audio | `ngs2.cpp/.h`, `libAudio.cpp`, `tests/Ngs2SamplerTests.cpp` | Mastering control `0x30000004` (gain: fbw_level/lfe_level) was fatal; implemented. Added `sceNgs2RackGetInfo` and `sceNgs2VoiceGetPortInfo` (NID `WCayTgob7-o`). Layouts inferred from public emulator ABIs + disassembly. CTest `ngs2_sampler` passes. |
| 3 | Vertex subgroup fallback | `SpirvEmitter*`, `spirvEmitterHelpers/Module.cpp`, `ShaderRecompiler.*`, `pipelineCache.cpp` | MoltenVK rejects `SubgroupLocalInvocationId` in vertex stages. Game-gated (PPSA22520 + Apple) fallback substitutes `gl_VertexIndex`; scalar builtins are now loaded as `int` + bitcast (an earlier version emitted an invalid `OpAccessChain` on a scalar). **[verified]** harmless for this game's VS (see 7). |
| 4 | Frame diagnostics | `swapchain.cpp` | `KYTY_FRAME_STATS`, `KYTY_FRAME_DUMP_DIR`: read back the presented source frame, log stats, write PNG. |
| 5 | Internal diagnostics (Codex + me) | `textureCache.cpp/.h`, `renderDraw.cpp`, `descriptors.cpp`, `spirvEmitterFlow/Module.cpp` | See section 5. |
| 6 | Harness | `harness/run.sh`, `scan_log.py`, `stage.py`, docs | See section 5. |

None of the diagnostics change behavior unless their env var is set.

---------------------------------------------------------------------------------------------------

## 5. The measurement harness and every diagnostic switch

`./harness/run.sh <name>`: builds nothing; launches with timeout + crash capture; writes
`kyty.log`, `guest-printf.log`, `run.json`, `scan.txt`, `stage.txt`, `shaders/`.
`scan.txt` ends with `[frames] N sampled, N all-black, N with content` and
`[stage] N presented_samples=N content=yes|NO (all black)`. Stage 1 = frames presented; stage
2+ cannot be automated yet. `scan_log.py` reads both logs (ranked: unresolved imports,
unimplemented, unsupported instructions, GPU errors, crashes). `KYTY_FRAME_DUMP=1` opts in to PNG dumps
(~25 MB per 4K frame).

| Env var | Effect |
|---|---|
| `KYTY_FRAME_STATS=1` (harness default) | Log `FrameStats:` for presents 1-10 and every 60th: size, avg/max, nonblack %, ALL_BLACK / HAS_CONTENT. |
| `KYTY_FRAME_DUMP_DIR=<dir>` | Also write those frames as PNG (dir must exist). |
| `KYTY_INTERNAL_FRAME_STATS=1` | After each draw, read back each color render target (RGBA8/BGRA8, RG16F, RGBA16F) and log `InternalFrameStats:` with role, address, format, extent, nonblack %, alpha avg, VS/PS hash. Stalls the GPU. |
| `KYTY_INTERNAL_FRAME_SAMPLED=1` | Also read back every texture the pixel shader samples (`role=sampled`). |
| `KYTY_INTERNAL_FRAME_SKIP=N` / `_MAX=N` | Skip the first N eligible samples (reach steady state) / raise the default 100-sample cap. |
| `KYTY_INTERNAL_FRAME_ADDRESS=0x..` / `_PIXEL_HASH=0x..` / `_UNIQUE=1` | Filter by guest address / PS hash / one sample per unique address. |
| `KYTY_INTERNAL_FRAME_DUMP_DIR=<dir>` | Write these readbacks as RGBA PNG (<= 2.2 Mpix each). |
| `KYTY_INTERNAL_FRAME_VERTICES=1` | `DrawVertexProbe`: prim type, count, first_vertex, vertex buffer, and the first 6 fetched vertices (x,y,u,v,color). |
| `KYTY_VERTEX_GPU_COMPARE=1` (+`_MIN_FRAME=N`) | Copy the vertex data the GPU buffer holds back to the host and compare with guest memory (`VertexGpuCompare ... MATCH/MISMATCH`). |
| `KYTY_DRAW_RESOURCE_STATS=1` | `DrawResourceState` (sampled image descriptors), `ShaderBufferState` (descriptor base/size/readable/first words). |
| `KYTY_DRAW_INPUT_STATS=1` + `KYTY_DRAW_INPUT_MIN_FRAME=N` (+ `KYTY_GRAPHICS_DEBUG_DUMP=true`) | `DrawInputState`: first 64 draws' vertex records from frame N. |
| `KYTY_FORCE_POSITIVE_VIEWPORT=1` (pre-existing) | Flip negative viewport heights to positive. |
| `KYTY_DISABLE_CLIP_GUARD=1` | Omit the zero-position clip-distance plane in vertex shaders. |
| `KYTY_INTERNAL_FRAME_VERTICES=1` also logs `ViewportProbe` (x,y,w,h, depth range, zscale/zoffset, clip flags; first 60 draws) |
| `KYTY_CLAMP_VIEWPORT_DEPTH=1` | Clamp viewport minDepth/maxDepth to [0,1]. |
| `KYTY_DEBUG_SOLID_PS=1` | Force every float MRT0 pixel-shader export to opaque magenta (1,0,1,1). |

---------------------------------------------------------------------------------------------------

## 6. What the game actually does (facts gathered)

* **Imports:** 109 unresolved at load (Agc_v1 77, AgcDriver_v1 22, libkernel_v1 8, Ngs2_v1 1,
  LibcInternalExt_v1 1). **None were called at runtime** in any run examined. **[verified]**
  (Not a cause of black output; they remain worth resolving later.)
* **Content loads correctly.** The 1024x2048 sprite atlas read back from the GPU is a clean,
  complete Animal Well sheet (BIGMODE and SHARED MEMORY logos, controller art, ANIMAL WELL text, fauna). It uses
  **cyan (0,255,255) as a colorkey**; texel (0,0) is opaque white (the "untextured" pixel). **[verified]**
* **Resolution chain:** the game renders at **320x180**, some passes at 1280x720, and a final pass
  to **3840x2160** (R8G8B8A8Srgb, PS `0xd89c2250b89200d0`, a CRT/upscale style shader that samples a
  320x180 target). Scene targets also include R16G16 and RGBA16F buffers and a flat
  (128,128,255) "neutral normal" lighting target. **[verified]**
* **Shaders:** every draw sampled uses ONE vertex shader (`vs=0xcdcf88828e5c2df8`): reads
  position/uv (float4) and color (RGBA8) from vertex attributes, scales by screen/texture
  constants from scalar buffers (320, 180, 1024, 2048), outputs `pos=(2x/320-1, -2y/180+1, 0, 1)`.
  The sprite pixel shader `0xbe7777460c3444ff` samples the atlas, discards cyan, multiplies by vertex
  color; blend SRC_ALPHA / INV_SRC_ALPHA. **[verified]**
* **Draw mix** (frame ~300): full-screen quads, sprite quads (count 24/12/6), and many 144-vertex
  colored meshes (48 triangles, u=v=0 -> white texel, opaque dark vertex colors). Animation is
  advancing (sprite y changes 56/57 between frames), so game logic runs. **[verified]**
* **Viewport:** guest viewport is `(0,180) 320x-180` (negative height). **[verified]**
* **Timeline:** frame 1 is a 1280x720 blank; sprite colors in frame 1 have alpha 0 (a fade-in
  start); by frame 300 they are opaque white. **[verified]**
* **Presents:** at least ~540 frames in 45 s, all 3840x2160 and fully black (max channel 0);
  alpha average 0 on the final target. **[verified]**

---------------------------------------------------------------------------------------------------

## 7. The black-frame investigation: hypotheses and results

Core observation: **after a draw, the target equals its clear value exactly** (0,0,0,0 or
0,0,0,255 or the flat 128,128,255,255), for every PS hash observed, including the final 4K pass.

| Hypothesis | Test | Result |
|---|---|---|
| Missing AGC/other imports starve the GPU path | Count `Unresolved import stub called` | **[rejected]** 0 calls in all runs |
| Apple GPU feature gaps stop device creation | Fixed (item 1) | **[fixed]** device now creates |
| NGS2 mastering crash | Fixed (item 2) | **[fixed]** boot proceeds |
| Vertex lane-ID fallback breaks the VS | Changed lane source; read SPIR-V | **[rejected]** lane only feeds `(0xFFFFFFFF>>(lane&31))&1` which is always 1; Codex also changed the lane source with no effect |
| Pixel-kill disabled | Codex run `no-pixel-kill-1` | **[rejected]** still black |
| VS scalar constants zero / wrong offsets | `ShaderBufferState` trace | **[rejected]** 320,180,1024,2048 valid and readable |
| Zero-position clip guard clips everything | `KYTY_DISABLE_CLIP_GUARD=1` | **[rejected]** `viewport`/targets identical |
| Negative viewport / winding / culling | `KYTY_FORCE_POSITIVE_VIEWPORT=1` | **[rejected]** identical (flipping would invert culling; both black) |
| Vertex data zero/degenerate | `DrawVertexProbe` | **[rejected]** valid rects/triangles, valid UVs, in-range positions |
| GPU vertex buffer stale (tracker misses CPU writes under Rosetta) | `KYTY_VERTEX_GPU_COMPARE` | **[rejected]** 400/400 MATCH, at frame 0+ and at frame 400+ |
| Texture atlas not uploaded | Atlas readback PNG | **[rejected]** perfect |
| Pixel shader computes zero | `KYTY_DEBUG_SOLID_PS=1` (verified in SPIR-V line `OpStore %out_mrt_0 (1,0,1,1)`) | **[rejected]** targets and all 45 presented frames still black. This bisect implies no fragment from *any* draw reaches *any* attachment. |
| VS SPIR-V invalid | `spirv-val` on current dump | **[rejected]** valid. (An earlier "invalid AccessChain" concern was from a stale dump.) |
| Vertex input attributes mis-typed (what the VS receives) | Existing `VertexInputState` log (resource-debug-postbuild): attr0 fmt=109 (`R32G32B32A32_SFLOAT`) off 0, attr1 fmt=37 (`R8G8B8A8_UNORM`) off 16, stride 32, binding 0 | **[rejected]** correct |
| Viewport depth range invalid | `ViewportProbe` + `KYTY_CLAMP_VIEWPORT_DEPTH=1` | **[anomaly found, not the cause]** every draw has depth `[-1,1]` (`zscale=1, zoffset=0, dx_clip_space=0`), which is illegal Vulkan without `VK_EXT_depth_range_unrestricted` (not used anywhere). Clamping to [0,1] left output identical (42/42 black). Still worth fixing properly. |
| Attachment feedback loop on MoltenVK | extension list in `kyty.log` | **[no evidence]** `VK_EXT_attachment_feedback_loop_*` not exposed, so the flag is false |
| Rasterizer discard / write mask / cull config | Code read of `pipeline/shaders.cpp` | **[no evidence]** no discard; cull from guest state; color write mask from guest `cb_shader_mask=0xf` |

### What remains (not yet tested; ranked) 

1. **(Eliminated.)** Vertex-input attribute descriptions are correct (see table). The binding
   *offset* passed to `vkCmdBindVertexBuffers` is not independently verified but the GPU-side buffer
   content at `offsets[0]+first*stride` matched guest memory, which exercises the same offset.
2. **MoltenVK/Metal translation or limits** for this pipeline (e.g. dynamic rendering with
   `attachment_feedback_loop_enabled`, `VK_EXT_depth_clip_control` absent, depth clip default-on,
   `ClipDistance` handling, `firstVertex`/base vertex, 4-wide output, SPIRV-Cross output of
   `OpGroupNonUniformBallot` in the PS). Needs a Metal GPU frame capture or a minimal repro.
3. **Image/attachment mismatch:** draws write one `VkImage` while readback/sampling uses another view
   of the same guest address (aliasing in the texture cache), or the view/layer used for rendering is
   wrong.
4. **Viewport depth range:** `minDepth/maxDepth` come from guest `zoffset/zscale`; with default depth
   clipping enabled on MoltenVK, an out-of-range pair would clip every fragment. Not logged yet.
5. Missing **AGC/driver behavior** that is only exercised later (none called so far).

---------------------------------------------------------------------------------------------------

## 8. Mistakes and corrections (so nobody repeats them)

* I first reported "0 unresolved imports"; the scanner only read `kyty.log`, while `LOGF` output is
  in `guest-printf.log`. After fixing the scanner, 109 appeared; then runtime counting showed they
  are never called.
* I first blamed the stubbed AGC imports, then the vertex lane fallback, then buffer reads; each was
  tested and rejected (table above). A stale SPIR-V dump (`/tmp/vs2.spvasm`) briefly suggested an
  invalid AccessChain; the current dump is valid. **Always re-dump shaders from the current build.**
* A 4 GB log from `KYTY_GRAPHICS_DEBUG_DUMP` hung my scan; use streaming `grep -a` and delete it.
* A `git add harness` of the run folders hung git.
* "Stage 1" (frames presented) was reported as progress before checking content; the correct gate
  is `content=yes`.

---------------------------------------------------------------------------------------------------

## 9. Evidence run index (`harness/runs/`, git-ignored)

`frames-1` (presented frames all black) - `flow-check` (109 stubs, 38/38 black) -
`internal-readback-1`, `steady-1`, `steady-sampled-1` (target tables) - `viz-1/2`, `atlas-1`
(PNG dumps; atlas image) - `viewport-1`, `noguard-1`, `solid-1`, `solid-spv` (rejected
hypotheses; `solid-spv/shaders/` has current SPIR-V) - `verts-1` (draw/vertex pairing) -
`gpucmp-1/2` (GPU vs guest vertex data) - `viewport-depth-1`, `depthclamp-1` (viewport depth).
