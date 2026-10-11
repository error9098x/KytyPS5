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

## Silent Hill: The Short Message (PPSA10112) — current blockers (2026-10-11)

The title reaches and renders the warning/Continue screen on the descriptor-backed route. Cleared
since the first report:

- Aliased storage-buffer views (`5f8b23d4`) and the vertex subgroup fallback (`652d4247`).
- **Mesh shaders are no longer a hard limit.** Without `VK_EXT_mesh_shader` (MoltenVK) the guest
  NGG/GS program is compiled as a compute shader that writes triangles and vertices to a scratch
  buffer, and a generated vertex shader draws them (`src/graphics/shader/meshEmulation*.{h,cpp}`,
  `spirvEmitterMesh.cpp`, `renderDraw.cpp`). The first emulated draw is encoded
  (`MeshEmulation: first draw groups=1 instances=64`). Visual correctness is unverified.
- Vertex programs that use ballot/shuffle (readfirstlane, exec masks) now run as one-lane
  subgroups on Apple (`single_lane_subgroup`); Metal rejects `threads_per_simdgroup` in vertex
  functions.
- Ordinary descriptor sets are now used for every title on Apple (MoltenVK push descriptors leave
  storage buffers unusable; first found with Animal Well). `KYTY_ENABLE_PUSH_DESCRIPTORS=1`
  restores push descriptors.

Remaining, in order of how soon each stops a run (updated 2026-10-11):

1. **Unreal out-of-memory after Continue (current first blocker).** On the descriptor route the
   run now goes past the old 43-second crash point and ends at about 86 seconds with Unreal's
   `LowLevelFatalError`: "Ran out of memory allocating 262144 bytes with alignment 65536",
   `AvailablePhysical: 65536`, `UsedPhysical: 13107068928` (run `shtsm-nullset-fix-1`). Unreal
   reserves the whole 12.5 GiB direct-memory pool at boot by halving its requests, so this is the
   game's own sub-allocator running dry. Suspects: deferred frees gated on GPU fences/labels that
   signal late or never, and the steady 64 KB batch-map page moves before the abort. Instrument
   first; also A/B the 64 MB and 128 MB scratch sizes.
2. **Gameplay acceptance is not proven.** The descriptor route has stage 2 evidence only; it needs
   a repeatable Continue transition, gameplay, a 10-minute soak, and reference screenshots.
3. **Physical-BDA route remains unstable.** Without the opt-in descriptor route, MoltenVK can still
   report `kIOGPUCommandBufferCallbackErrorInvalidResource` on the first emulated mesh draw. The
   descriptor route is intentionally opt-in because it also requires disabling Metal3 argument
   buffers in MoltenVK 1.4.2.
4. **GPU-driven rendering needs GPU to CPU coherence.** The title builds `SET_SH_REG_INDIRECT`
   tables with compute shaders; with tracking disabled the PM4 parser reads zeros
   (`unknown sh reg at 00100`). Page tracking cannot simply be turned off for this title.

Fixed: the post-Continue "guest null access" (`address=0x10`, `runtimeLinker.cpp:756`) was a null
MoltenVK descriptor set on depth-only emulated mesh draws, not guest code (`8ea17d3d`).

Animal Well is unaffected (3 of 3 regression runs clean, title content visible, about 60 FPS).
