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

## Internal render-texture chain is black

The final 4K video pass samples internal 320x180 render textures and completes
without Vulkan errors. Pixel-kill and vertex-lane A/B experiments stayed fully
black. The next investigation must read back or validate the internal texture
after its producing pass, rather than adding more import stubs blindly.
