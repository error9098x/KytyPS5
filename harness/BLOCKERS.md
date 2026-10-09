# Active blockers

## Apple vertex subgroup fallback

MoltenVK 1.4.2 rejects `SubgroupLocalInvocationId` in Animal Well's first
vertex shader. The PPSA22520-only Apple path substitutes `gl_VertexIndex` so
the process can continue. This is an evidence-driven compatibility fallback,
not yet a visual correctness proof. A screenshot/reference comparison is
required before retaining it as an accepted improvement.

## Unresolved load-time imports and black render output

109 unresolved imports are patched at load (Agc_v1 77, AgcDriver_v1 22, libkernel 8,
Ngs2 1, LibcInternalExt 1), but no unresolved-import calls were observed. The current
draw trace shows real draws and successful pipelines, so the causal role of these imports
is unproven. Resolve only imports that appear in a call trace before implementing them.
See `harness/runs/flow-check/guest-printf.log` for the relocation list.

## MoltenVK push descriptors (fixed for Animal Well)

The black-frame blocker was reproduced as a descriptor-binding issue. MoltenVK's
push-descriptor path left Animal Well's storage-buffer/image resources unusable in
the guest shaders, while ordinary descriptor sets produced the expected title art.
Kyty now selects ordinary descriptor sets only for `PPSA22520` on Apple hosts.
Keep the workaround title-gated and validate other titles separately.

## Internal render-texture chain

The final 4K video pass samples internal 320x180 render textures and completes
without Vulkan errors. Pixel-kill and vertex-lane A/B experiments stayed fully
black. The next investigation must read back or validate the internal texture
after its producing pass, rather than adding more import stubs blindly.

The readback proves that some intermediate 8-bit targets are non-black while the
final video target is black. The next fix should trace the final composite source
and image barriers/format handling, with a targeted non-black readback as the
keep/revert gate.

The first vertex shader's scalar descriptors are valid in guest memory; the binding
path, rather than S_LOAD/component offset lowering, was the issue. Do not change
S_LOAD/component offset lowering based on the earlier black-frame evidence.
