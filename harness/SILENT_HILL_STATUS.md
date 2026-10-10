# Silent Hill: The Short Message (PPSA10112) — status, 2026-10-10

## Where it is now

- The game **boots and shows real screens**. The user reached the grey screen and the
  brightness-adjustment screen. After a few screens it goes **black**, then usually ends with a
  GPU error.
- Harness, 90 s runs: 4 to 10 frames with content per run, about 17–22 FPS, 2 of 4 runs reached
  the 90 s timeout with no fatal error. The other runs ended in a GPU "device lost" error.
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

## What fixed what

| Problem | Fix | Commit |
|---|---|---|
| Metal rejects aliased buffer bindings | single 32-bit storage-buffer view on Apple | `5f8b23d4` |
| Vertex subgroup builtins | Apple vertex fallback extended to this title | `652d4247` |
| No mesh shaders in MoltenVK | guest NGG/GS program runs as compute, a generated vertex shader draws the result | `a3302568` |
| Metal rejects ballot/shuffle in vertex functions | one-lane subgroup mode for Apple vertex programs | `a3302568` |
| MoltenVK push descriptors break storage buffers | ordinary descriptor sets on every Apple title | `a3302568` |
| My bug: compute pass got no push constants | `eCompute` added to the shared push-constant range | `a3302568` |
| **Rosetta abort on split 256-bit AVX stores to protected pages** (root cause in `ROSETTA_AVX_PAGE_PROTECTION.md`) | upstream KytyPS5 PR #984 by Bartlomiej Wolynczyk, cherry-picked: rewrites every 256-bit guest store into two 128-bit halves at load time (178,772 sites in `eboot.bin`, 822 in `libc.prx`) | `240ba3d1`..`068bc695` |

Tests: `macos_wide_store` passes on this Mac. Animal Well 3 of 3 clean before the PR #984
cherry-pick; **not yet re-checked after it.**

## Open problems, most likely first

1. **Black screen after the brightness screen, then GPU device lost.**
   - Error: `kIOGPUCommandBufferCallbackErrorInvalidResource` (code 9), sometimes `...Hang`.
   - It starts after the first buffer-device-address draw, which is the first emulated mesh draw.
     `KYTY_MESH_EMU_SKIP=all` removes the error; skipping only the compute half or only the draw
     half does not.
   - The black screen is probably the same problem: the screen after brightness is likely the
     first one using the emulated mesh (geometry shader) path, and its output or the GPU fault
     leaves nothing visible.
   - Next step: enable Metal per-encoder error status (command buffer `errorOptions` with
     encoder execution status) to name the failing encoder; dump the scratch buffer after one
     emulated draw and check triangle counts and positions.
2. **Mesh emulation output is unverified.** Never confirmed on screen.
3. **Performance:** host-visible scratch runs are about 23–26 FPS warm, with 2.3 s stalls.
4. The stale-data issue from GPU-driven indirect register tables only appears with
   `KYTY_NO_PAGE_PROTECT=1`; do not use that flag for this game.

## How to run

```bash
cd /Users/aviral/Documents/ChatGPT/PS5/KytyPS5
cmake --build _Build/macos-x86_64 --target kyty_emulator -j8
KYTY_GAME_DIR=/Users/aviral/Documents/ChatGPT/PS5/SilentHill.ShortMessage-extracted/PPSA10112-app \
KYTY_FRAME_STATS=1 KYTY_FRAME_DUMP=1 KYTY_PRINTF_DIRECTION=File KYTY_TIMEOUT_SECONDS=90 \
./harness/run.sh shtsm-NAME
```

Useful switches: `KYTY_MESH_EMU_SKIP=compute|draw|all`, `KYTY_MESH_TRACE=1`,
`KYTY_MESH_VS_DUMP=<dir>`, `KYTY_FAULT_LOG=1`, `KYTY_SPLIT_WIDE_STORES=0|1` (PR #984),
`KYTY_DISABLE_MESH_EMULATION=1`. Runs from this session: `harness/runs/shtsm-widesplit-*`,
`harness/runs/shtsm-frames-1` (18 frame dumps).

More detail: `HANDOFF_SILENT_HILL.md`, `BLOCKERS.md`, `ROSETTA_AVX_PAGE_PROTECTION.md`.
