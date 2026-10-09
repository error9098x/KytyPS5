# Active blockers

## Apple vertex subgroup fallback

MoltenVK 1.4.2 rejects `SubgroupLocalInvocationId` in Animal Well's first
vertex shader. The PPSA22520-only Apple path substitutes `gl_VertexIndex` so
the process can continue. This is an evidence-driven compatibility fallback,
not yet a visual correctness proof. A screenshot/reference comparison is
required before retaining it as an accepted improvement.

## Missing AGC imports (primary suspect for black output)

109 unresolved imports stubbed at load (Agc_v1 77, AgcDriver_v1 22, libkernel 8, Ngs2 1,
LibcInternalExt 1). Animal Well is a PS5 title using AGC; the stubs likely return without
building command buffers, so presented frames are black. Resolve NIDs to names, then implement
in dependency order. See `harness/runs/flow-check/guest-printf.log` (grep `unresolved PLT import`).
