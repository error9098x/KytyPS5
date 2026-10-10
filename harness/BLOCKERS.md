# Active blockers

## Apple vertex subgroup fallback

MoltenVK 1.4.2 rejects `SubgroupLocalInvocationId` in Animal Well's first
vertex shader. The PPSA22520-only Apple path substitutes `gl_VertexIndex` so
the process can continue. This is an evidence-driven compatibility fallback,
not yet a visual correctness proof. A screenshot/reference comparison is
required before retaining it as an accepted improvement.

## Unresolved load-time imports and black render output

108 unresolved imports are patched at load (Agc_v1 77, AgcDriver_v1 22, libkernel 8,
LibcInternalExt 1), but no unresolved-import calls were observed. The current
draw trace shows real draws and successful pipelines, so the causal role of these imports
is unproven. Resolve only imports that appear in a call trace before implementing them.
See `harness/runs/flow-check/guest-printf.log` for the relocation list.

## MoltenVK push descriptors (fixed for Animal Well)

The black-frame blocker was reproduced as a descriptor-binding issue. MoltenVK's
push-descriptor path left Animal Well's storage-buffer/image resources unusable in
the guest shaders, while ordinary descriptor sets produced the expected title art.
Kyty now selects ordinary descriptor sets only for `PPSA22520` on Apple hosts.
Keep the workaround title-gated and validate other titles separately.

## NGS2 streamed-audio reset (fixed)

Gameplay input reaches streamed audio. The game sends an NGS2 block reset
(`flags=0x4`) after a voice stops accepting blocks; this is a valid queue reset,
not an invalid block submission. Kyty now accepts that reset transition. A 115-second
input run completed without a fatal error; repeat longer gameplay soaks next.

## Audio output (signal fixed; host routing selectable; speaker audibility pending)

The audio path was initially silent even though SDL queued blocks. Opt-in signal
telemetry showed zero NGS2 output, caused by an unresolved voice-info export and
an incomplete graph render order/channel setup. The export, graph, and output
writeback fixes now produce nonzero NGS2 and SDL samples. Input latency and frame
pacing have been measured with host, guest-poll, and present timestamps; the
harness keeps these probes opt-in.

The first frame-timing probe measured a 59.12 FPS median with a 33.37 ms p95 and
a 2.96 s startup outlier. Three focused probes now measure host-to-guest-poll and
host-to-present timing. The deterministic probe reproduces roughly one-second
host-path samples when the first synthetic input arrives before the game has
begun its first pad poll. Exact-state traces show the queued transitions are
drained in order. Sampling places Metal residency work around `vkQueueSubmit`
before/around the first poll; shader compilation begins afterward, so prewarming
is not yet justified as the fix. Two of three new post-initialization probes stay
below 31 ms host-to-present maximum; the first still overlaps startup. A separate
gameplay route reached the “Wake up” scene and measured Arrow-Left at 7.35 ms
median, 17.59 ms p95, and 24.30 ms maximum to guest observation, with a 16.64 ms
next-present maximum.

The startup-only 2.88–3.44 s gap is separately characterized by a macOS process
sample: MoltenVK/Metal command encoding (`beginRendering`, image clears, and draws)
and timeline-semaphore waits dominate the host GPU thread while the guest executes.
Opt-in timing measured a largest guest-GPU processing slice of 724 ms and an early
queue submission of about 456 ms; later slices stay below 60 ms, so the complete
gap also includes guest initialization or a wait outside one GPU submission.
`AgcSuspendPoint` itself waits only 25.5 ms initially and 8–18 ms afterward. A
guest barrier bypass is therefore not justified without a rendering-correctness
test. MoltenVK `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=1` was also tested in
three runs and did not reduce the gap, so it remains disabled. Disabling Metal
heaps (`MVK_CONFIG_USE_MTLHEAP=0`) caused multi-second latency regression and was
rejected.
Controller, axis, mouse, and keyboard events are now timestamped. AudioOut2 also
releases its state mutex before the potentially blocking SDL queue call.

macOS currently exposes both `HyperX Cloud Stinger 2 Wireless` and `MacBook Air
Speakers`. Kyty logs the resolved SDL device and supports `--audio-device
"MacBook Air Speakers"` (or `KYTY_AUDIO_DEVICE` in the harness). Both devices
report `paused=0` and draining converted bytes in probes. The host was found
muted at zero output volume and was restored to 50% unmuted; the remaining check
is the user's physical listening confirmation.

## Render-texture chain (title path closed; gameplay visual gate pending)

The earlier internal-target investigation established that the final black output
was a descriptor-binding failure, not a missing import or shader-position issue.
Ordinary descriptor sets now produce the title artwork. Keep the workaround
PPSA22520/Apple-gated and use fixed-point gameplay screenshots for the remaining
visual correctness gate. Do not revisit S_LOAD/component-offset lowering based
on the historical black-frame evidence.

## Silent Hill: The Short Message — mesh shaders (HARD HOST LIMIT, 2026-10-10)

Silent Hill (PPSA10112) now reaches `EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled)` at
`src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp:625`. It is an Unreal Engine title whose
PS5 NGG geometry shaders Kyty models as Vulkan mesh shaders; there is no non-mesh fallback in Kyty
(`shaders.cpp:243` and `pipelineCache.cpp:625` both require host mesh support; `renderDraw.cpp:1086`
gates the mesh path on `ShaderType::Mesh`).

This machine cannot satisfy that requirement:

- The Vulkan driver here is **MoltenVK 1.4.2**, which does **not** expose `VK_EXT_mesh_shader`
  (absent from its documented extension list; upstream issue KhronosGroup/MoltenVK#2375 has been
  open since 2024-10). The device-extension dump in `harness/runs/shtsm-fix2/kyty.log` confirms no
  mesh extension is advertised.
- The one macOS Vulkan driver that does support mesh shaders, **KosmicKrisp** (Mesa, in the LunarG
  macOS Vulkan SDK, Vulkan 1.4 conformant on Metal 4), is **Apple-Silicon-only** ("only functional
  on Apple Silicon, not older x86_64 hardware").
- Kyty only runs as **x86-64** here: PS5 guest code is x86-64 and Kyty executes it natively —
  `runtimeLinker.cpp:237` casts a guest address to a function pointer, and the JIT stubs in `jit.h`
  emit x86-64 machine code with x86 register constraints (`register ... asm("r14")`). A native arm64
  Kyty does not run x86-64 guest code. (arm64 also fails to build: `hostException.cpp` reads x86
  `__rip`/`__rax`/... from `mcontext`, and `x64InstructionEmulator` is x86-64-only.)

So the requirements are mutually exclusive on this machine: x86-64 Vulkan driver → no mesh shaders;
mesh shaders → arm64 → cannot execute x86-64 guest code. Running PPSA10112 (or any PS5 3D title that
uses NGG with a real geometry stage) needs one of: a mesh-shader-capable x86-64 Vulkan driver, a
Kyty arm64 CPU backend (x86-64 interpreter/JIT for the guest), or a Kyty-side non-mesh lowering of
the NGG path. None is a small change.

Cleared on the way here (kept): aliased storage-buffer views (`5f8b23d4`) and the vertex subgroup
fallback for PPSA10112 (`652d4247`). Both are Apple/`PPSA22520`-gated and regress neither Animal Well
nor the non-Apple path; see `harness/LOG.md`.
