# Rosetta 2 aborts on 256-bit AVX accesses to tracked pages

**Status:** root cause confirmed with a 40-line reproduction; no complete fix exists inside the
current architecture. Affects any title that runs AVX2 code (Unreal Engine titles such as
Silent Hill: The Short Message, PPSA10112). Animal Well (PPSA22520) is not affected in practice.

## Symptom

The emulator process dies with exit code -6 (SIGABRT) and this line in `kyty.log`:

```
rosetta error: unexpectedly need to EmulateForward on a synchronous exception
x86_rip=0x9049a8930 arm_pc=0xfc0421e1d8 num_insts=3 inst_index=2
x86 instruction bytes: c5 fc 11 00 (vmovups [rax], ymm0)  48 83 c3 20  39 dc  0f 84 ...
```

It comes from Rosetta itself, not Kyty, and happens **before** Kyty's SIGSEGV/SIGBUS handler runs.

## Cause

Kyty tracks CPU access to GPU-visible memory by write- or access-protecting host pages and
resolving the fault in a signal handler (`PageManager`, `RenderContext::HandleFault`).

Rosetta translates a 256-bit AVX store/load into **two 128-bit halves**. If the first half
completes and the second half then faults, Rosetta cannot reconstruct precise x86 state and
aborts. That requires the two halves to land on different protection states:

| Access | Result |
|---|---|
| SSE 16-byte store across two protected pages | fine (single instruction, restarted) |
| AVX 32-byte store, both halves in one page | fine |
| AVX 32-byte store aligned to a page boundary | fine |
| AVX 32-byte store, first half on a writable page, second half on a protected page | **abort** |
| AVX 32-byte store across two protected pages, handler unprotects only the first | **abort** |

Any sequential 256-bit loop (memset/memcpy/zeroing) that is not 32-byte aligned crosses page
boundaries with such a store, so it aborts at the first boundary where the next page is protected.

Reproduction: `harness/repro/rosetta_avx_straddle.c` (mode 1 aborts, modes 0 and 2 pass).

## Why Silent Hill hits it constantly

Before buffer-device-address (BDA) mode starts, a run has about 126 tracking protections and 1
unprotect. Once the first BDA draw happens (`RenderContext::PrepareBda`), every later BDA draw
re-synchronizes and re-tracks all buffers while the game keeps writing memory:
**2,433 tracking protections and 3,418 unprotects** in the same run (`KYTY_PAGE_PROTECT_LOG=1`).
Every handled fault is another chance for the abort. All observed aborts were the same guest loop
(`vmovups [rax], ymm0; add rbx, 0x20; cmp esp, ebx`) at the same address.

## What was tried

| Attempt | Outcome |
|---|---|
| Resolve the next page when the fault is within 32 bytes of a page end | 2 of 3 runs still abort |
| Detect VEX.256 at the faulting RIP and resolve the whole contiguous protected run ahead | removes the chained-fault case; runs still abort |
| Same, over a 4 MB window of sparse protected runs | 5 of 6 runs still abort |
| `KYTY_NO_PAGE_PROTECT=1` (no tracking at all) | no abort, but GPU-written data never reaches the CPU, so the PM4 parser reads zeroed indirect register tables |

The remaining aborts are the **entry-boundary** case: a loop starting on an untracked page and
walking into a tracked one never faults before the boundary, so no handler can run ahead of it.
Any boundary between a writable and a protected page is a hazard for an unaligned 256-bit store,
so no page-protection layout can remove it.

The run-ahead logic is kept but opt-in (`KYTY_ROSETTA_RUNAHEAD=1`) because it over-invalidates
tracked memory and did not change the outcome.

## Real options

1. **Do not use page protection for coherence.** CPU to GPU: validate by hashing or comparing the
   used range at submit time. GPU to CPU: download GPU-modified ranges when the fence or label
   write is processed (a CPU read before synchronization is already invalid on real hardware).
   Large change to `BufferCache`/`TextureCache`.
2. **Native arm64 Kyty** with an x86-64 interpreter or JIT for guest code, removing Rosetta.
   Very large; `hostException.cpp` and `x64InstructionEmulator.cpp` are x86-only today.
3. **Rewrite guest AVX instructions** at load time into two separate 128-bit x86 instructions
   (each precise under Rosetta). Needs a relocating patcher; loads and memory-operand arithmetic
   have the same hazard, so the scope is every AVX-256 instruction with a memory operand.

## Diagnostics added

| Variable | Effect |
|---|---|
| `KYTY_PAGE_PROTECT_LOG=1` | log every tracking protection change |
| `KYTY_FAULT_LOG=1` | log every handled CPU fault (access, address, offset in page) |
| `KYTY_NO_PAGE_PROTECT=1` | skip all tracking protections |
| `KYTY_ROSETTA_RUNAHEAD=1` | resolve protected runs ahead of a faulting 256-bit access |
| `KYTY_BUFFER_LOG=1` | log every device-address buffer allocation |
