# Silent Hill: The Short Message (PPSA10112) — status, 2026-10-11

## Where it is now

**Update 2026-10-11 16:30 IST (supersedes conflicting lines below):**
- Boots, plays the intro video, shows the warning screen, accepts Cross, and reaches the title menu
  (`/tmp/titlerun.sh`). The OOM after Continue is fixed (`78019f9f`).
- The title-menu 3D background is still black (first blocker; see BLOCKERS.md). The tonemap LUT is
  now correct (`86ec37b3`) and exposure is consistent with the scene; the black starts in the DOF
  chain (DOF recombine output ~4.4x too dark, gather ~0.8% coverage, some indirect DOF dispatches 0 groups).
- Route: `KYTY_MESH_SCRATCH_HOST=1 KYTY_MESH_SCRATCH_MB=256 KYTY_MESH_SCRATCH_DESCRIPTOR=1
  MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0 MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2`
  (PREFILL works around an intermittent MoltenVK compute use-after-free; clean 5/5).
- Animal Well regression 3/3 after 46914434 (aw-regress-*-forceexp: stage 2, 33/43 content frames; 34/44 before, same 10 black boot frames).
- Still stage 2. No gameplay soak or visual reference check has passed.

- The game **boots and shows real screens**. The harness now captures the full warning/Continue
  screen, including its background and prompt.
- The default physical-BDA scratch path still reaches Metal `InvalidResource`. A new opt-in
  descriptor-backed scratch path avoids that GPU fault.
- No Rosetta aborts any more.
- A new opt-in `KYTY_MESH_SCRATCH_HOST=1` path places the mesh-emulation scratch buffer in
  host-visible memory. Three 35-second runs reached the timeout with content frames and no
  `InvalidResource`/device-loss error; the fixed-point capture shows the real suicide/self-harm
  warning screen with the Continue prompt. Warm pacing is about 23–26 FPS, so this is a
  correctness/progress workaround with a substantial performance cost, not a final solution.
- A late automated Continue test with `KYTY_MESH_SCRATCH_MB=64` survived 60 seconds after the
  warning-screen transition, with no GPU loss or Unreal OOM. The 256 MB host-visible setting
  later hit a 13 GB physical-memory OOM after longer streaming, so 64 MB is the current practical
  launch setting while the mesh resource path is redesigned.
- A 90-second captured Immediate-present route shows the sequence clearly: Konami intro, warning
  screen, a long black loading/streaming interval after Continue, then content frames return.
  The run eventually ends in Unreal's memory allocator after reaching the host's physical-memory
  ceiling; the black interval itself is not a permanent shader failure.

An attempted automatic PPSA10112/Apple default for host-visible scratch was reverted after a
short run still hit `InvalidResource`. The stable route remains explicitly opt-in with
`KYTY_MESH_SCRATCH_HOST=1`; 256 MB is the most repeatable setting, while 64 MB reduces memory
use but remains experimental.
- A 128 MB host-visible run survived the delayed Continue route without GPU loss, Unreal OOM,
  or a fatal error and exited cleanly after about 51 seconds. It is the current memory/stability
  experiment; it has not yet passed the 10-minute gate.

### Descriptor-backed mesh scratch (new A/B)

The first emulated mesh draw used a physical buffer-device address for its scratch output. On
MoltenVK/Metal that resource was intermittently reported as `InvalidResource` even when the
scratch allocation was host-visible. The opt-in route binds scratch as an ordinary storage-buffer
descriptor shared by the compute and generated vertex stages:

```bash
export KYTY_MESH_SCRATCH_HOST=1
export KYTY_MESH_SCRATCH_MB=256
export KYTY_MESH_SCRATCH_DESCRIPTOR=1
export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0
```

The 45-second gated run `harness/runs/shtsm-descriptor-gated-1` timed out normally, produced 13
content frames including the warning/Continue screen, and reported **zero GPU validation/device
loss errors**. A 90-second run also reached the timeout with no GPU loss. The Metal argument-buffer
switch is required because MoltenVK 1.4.2 cannot convert this extra descriptor in its Metal3
argument-buffer mode. The legacy BDA path remains the default for other titles.

The scripted Cross probe delivered the button to guest polls (19.88 ms median in the short probe),
but the captured image remained on the Continue screen; a longer held/repeated probe later stopped
on a guest null access at `runtimeLinker.cpp:756`. Whether the title accepts this synthetic Cross
path still needs confirmation with a real controller event.

### Upstream comparison (2026-10-11)

The local branch already contains official `origin/main` (`4c8fc53`) as an ancestor, so there is
no newer official Silent Hill fix to merge from the main repository. EmK530's fork has no
PPSA10112-specific commit. Its renderer branch adds a generic periodic draw flush and SRT cleanup
optimization (`097e7c2`, followed by `532efa3` and `41f7794`). A flush-only transplant was tested
here and regressed the controlled route (stage 0 at 40.5 seconds, warm pacing about 21 FPS), so it
was reverted. The coupled SRT changes were left out rather than applying an unverified partial port.

## What fixed what

| Problem | Fix | Commit |
|---|---|---|
| Metal rejects aliased buffer bindings | single 32-bit storage-buffer view on Apple | `5f8b23d4` |
| Vertex subgroup builtins | Apple vertex fallback extended to this title | `652d4247` |
| No mesh shaders in MoltenVK | guest NGG/GS program runs as compute, a generated vertex shader draws the result | `a3302568` |
| Metal rejects ballot/shuffle in vertex functions | one-lane subgroup mode for Apple vertex programs | `a3302568` |
| MoltenVK push descriptors break storage buffers | ordinary descriptor sets on every Apple title | `a3302568` |
| My bug: compute pass got no push constants | `eCompute` added to the shared push-constant range | `a3302568` |
| MoltenVK null descriptor set on depth-only emulated mesh draws (descriptor route) | always commit graphics bindings on the descriptor route | `8ea17d3d` |
| **Rosetta abort on split 256-bit AVX stores to protected pages** (root cause in `ROSETTA_AVX_PAGE_PROTECTION.md`) | upstream KytyPS5 PR #984 by Bartlomiej Wolynczyk, cherry-picked: rewrites every 256-bit guest store into two 128-bit halves at load time (178,772 sites in `eboot.bin`, 822 in `libc.prx`) | `240ba3d1`..`068bc695` |

Tests: `macos_wide_store` passes on this Mac. Animal Well 3 of 3 clean before the PR #984
cherry-pick; **not yet re-checked after it.**

## Open problems, most likely first

1. **Unreal out-of-memory after Continue (first blocker).** With the null descriptor set fixed
   (`8ea17d3d`), the descriptor route runs about 86 seconds and then Unreal aborts:
   "Ran out of memory allocating 262144 bytes with alignment 65536", `UsedPhysical: 13107068928`
   of its 12.5 GiB pool (run `shtsm-nullset-fix-1`). Suspects: deferred frees waiting on GPU
   fences/labels, and the 64 KB batch-map page moves. Being instrumented.
2. **Gameplay stability and visual correctness are unverified.** The current evidence is stage 2;
   no 10-minute gameplay soak or reference comparison has passed.
3. **Performance:** descriptor-backed host scratch is about 26 FPS warm, with multi-second
   streaming stalls during the Continue transition.
4. The stale-data issue from GPU-driven indirect register tables only appears with
   `KYTY_NO_PAGE_PROTECT=1`; do not use that flag for this game.

Fixed 2026-10-11: the post-Continue "guest null access" at `runtimeLinker.cpp:756` was a host fault
in MoltenVK (`bindMetalResources`, null `_descriptorSets[set]`) because depth-only emulated mesh
draws skipped committing graphics bindings on the descriptor route (`8ea17d3d`).

## How to run

```bash
cd /Users/aviral/Documents/ChatGPT/PS5/KytyPS5
cmake --build _Build/macos-x86_64 --target kyty_emulator -j8
KYTY_GAME_DIR=/Users/aviral/Documents/ChatGPT/PS5/SilentHill.ShortMessage-extracted/PPSA10112-app \
KYTY_MESH_SCRATCH_HOST=1 KYTY_MESH_SCRATCH_MB=256 KYTY_MESH_SCRATCH_DESCRIPTOR=1 \
MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0 \
KYTY_FRAME_STATS=1 KYTY_FRAME_DUMP=1 KYTY_PRINTF_DIRECTION=File KYTY_TIMEOUT_SECONDS=90 \
./harness/run.sh shtsm-NAME
```

Useful switches: `KYTY_MESH_EMU_SKIP=compute|draw|all`, `KYTY_MESH_TRACE=1`,
`KYTY_MESH_VS_DUMP=<dir>`, `KYTY_FAULT_LOG=1`, `KYTY_SPLIT_WIDE_STORES=0|1` (PR #984),
`KYTY_DISABLE_MESH_EMULATION=1`. Runs from this session: `harness/runs/shtsm-widesplit-*`,
`harness/runs/shtsm-frames-1` (18 frame dumps).

More detail: `HANDOFF_SILENT_HILL.md`, `BLOCKERS.md`, `ROSETTA_AVX_PAGE_PROTECTION.md`.
