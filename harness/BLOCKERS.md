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
host-path samples only
while the first input transitions from the title to gameplay; the log shows a
batch of first-use pixel-shader compilation in that window. Warm immediate-present
runs stay below 35 ms host-to-present maximum, with one 225 ms guest-poll outlier.
Prewarming or moving those first-use compilations off the input/render path remains
unresolved.

The startup-only 2.88–2.99 s gap is separately characterized by a macOS process
sample: MoltenVK/Metal command encoding (`beginRendering`, image clears, and draws)
and timeline-semaphore waits dominate the host GPU thread while the guest executes.
`AgcSuspendPoint` itself waits only 25.5 ms initially and 8–18 ms afterward. A
guest barrier bypass is therefore not justified without a rendering-correctness
test.
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
