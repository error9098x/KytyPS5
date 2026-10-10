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
