# Animal Well on KytyPS5: Harness Specification

## Scope

Target the legally dumped Animal Well package (`PPSA22520`, content version
`01.000.011`) on this machine, while preserving existing KytyPS5 behavior for
other games and homebrew. Game-specific workarounds must be gated by an
explicit per-game condition. Missing functionality should be implemented
properly where practical; every compatibility stub must be logged.

## Baseline environment

- Validated KytyPS5 source baseline: `d3781f1c` (`codex/animal-well-compat`,
  2026-10-10)
- Host: macOS 26.5.1, Apple Silicon `Mac17,3`, ARM64, 10 CPU cores, 16 GiB RAM
- Intended build: x86-64 under Rosetta 2, Vulkan 1.3 through MoltenVK
- Game package: extracted under `Animal.Well-extracted-unar/`, title ID
  `PPSA22520`, version `01.000.011`

## Repository map

- CPU/loader/JIT: `src/loader` (ELF/runtime linker/JIT), guest instruction
  patching and emulation
- GPU: `src/graphics/guest_gpu`, `src/graphics/host_gpu`
- Shader translation: `src/graphics/shader/recompiler`
- Kernel and libraries: `src/kernel`, `src/libs`
- Configuration and logging: `src/common/emulatorConfig.*`,
  `src/common/logging/*`, CLI in `src/main.cpp`
- Build: CMake + Ninja, C++20, Clang; no-Qt target is `kyty_emulator`

## Baseline findings

1. ARM64 native configuration fails during `src/common/hostException.cpp`:
   x86 thread-state members (`__rip`, `__err`) are unavailable in ARM headers.
2. The documented x86-64 configuration builds successfully. MoltenVK 1.4.2's
   universal library is supplied explicitly because Homebrew's ARM-only library
   cannot load into the x86-64 Rosetta process.
3. The unmodified x86-64 runtime rejects the Apple M5 GPU's unsupported Vulkan
   features before game loading (`image view minLod`, buffer int64 atomics,
   cull distance).
4. After the Apple capability fallback, the game reaches NGS2 audio and then
   the first graphics pipelines. NGS2 gain (`0x30000004`) and the missing
   rack/voice query exports were implemented from public ABI evidence.
5. The first vertex pipeline then fails on MoltenVK's inability to compile a
   subgroup builtin in a vertex function. A PPSA22520-only Apple fallback uses
   `gl_VertexIndex` as the subgroup lane value in vertex stages; title artwork is
   now visible under the descriptor-set workaround.
6. Upstream documentation identifies macOS as experimental and expects the
   x86-64/Rosetta + MoltenVK path.
7. No Animal Well-specific KytyPS5 compatibility report was found in the
   initial upstream research.

## Current failure chain

1. The Apple/MoltenVK push-descriptor path makes Animal Well resources unusable;
   the title-gated ordinary-descriptor workaround restores title and gameplay
   pixels.
2. NGS2 streamed audio now reaches SDL and the selected playback device. The
   host was muted at zero volume during diagnosis and was restored to 50%,
   unmuted; physical audibility still needs user-side confirmation.
3. The first title-to-gameplay input can trigger first-use shader work, producing
   roughly one-second host-to-poll/present samples. Warm gameplay probes are
   generally below 35 ms host-to-present.
4. Startup retains a repeatable 2.88–2.99 s host GPU gap. Process sampling
   attributes it to MoltenVK/Metal command encoding and timeline synchronization;
   Kyty-side shader, direct-memory, PM4-splitting, and tested MoltenVK settings
   did not improve it.
5. Fixed-point captures are consistent, but Stage 4 cannot be claimed without an
   independent PS5/reference capture. The remaining load-time imports have not
   appeared in a call trace and remain deferred.

The current compatibility inventory is recorded in `BLOCKERS.md` and `LOG.md`:
the remaining 108 load-time imports are patched stubs with no observed calls in
the measured route; the Apple descriptor-set and NGS2/audio fixes are title- or
path-specific, with NGS2 unit coverage and runtime title-art/audio evidence.
New stubs must be added to that inventory and traced before implementation.

## Acceptance stages

1. First frame without a crash, stable across three runs.
2. Title/menu reached, stable across three runs.
3. Gameplay survives 10 minutes, stable across three runs.
4. No major visual or missing-effect defects against fixed screenshots.
5. Performance target: first establish baseline; then target >=30 FPS and
   investigate 60 FPS, recording frame-time and shader-stutter spikes.

## Boundaries

- Use only the user's legally dumped copy.
- Do not distribute game files, system software, keys, or bypass protections.
- Do not make unmeasured changes; retain before/after logs.
- Back up emulator configs and saves before runtime testing.
- Keep Apple workarounds title-gated (`PPSA22520`) and keep the low-latency
  presentation override scoped to that title; preserve the default behavior for
  other games and homebrew.
