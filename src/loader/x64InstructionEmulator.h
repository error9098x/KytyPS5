#ifndef KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_
#define KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_

#include <cstdint>
#include <vector>

namespace Loader::X64InstructionEmulator {

[[nodiscard]] bool TryEmulate(void* native_context);
// Total SIGILL redirects through ud2-patched wide-store sites since start (statistics only).
[[nodiscard]] uint64_t WideStoreTrapHits() noexcept;
struct WideStoreSplitResult {
	uint64_t candidates = 0;  // plain 256-bit vector stores found
	uint64_t patched    = 0;  // rewritten into two 128-bit stores
	uint64_t too_short   = 0;  // shorter than a rel32 jump and no int3 padding within rel8 reach
	uint64_t via_cave    = 0;  // short stores patched with a rel8 jump through int3 padding
	uint64_t trapped          = 0;  // short stores patched to ud2, redirected by the SIGILL handler
	uint64_t relocated        = 0; // hot short stores patched by moving the next instructions along
	uint64_t unsupported = 0; // segment override, encoder failure or trampoline area full
	uint64_t trampoline_bytes = 0;
	uint64_t notrap           = 0; // KYTY_WIDE_STORE_NOTRAP: short stores patched trap-free
	uint64_t notrap_kept      = 0; // KYTY_WIDE_STORE_NOTRAP: short stores still patched to ud2
};

// KYTY_WIDE_STORE_NOTRAP=1 (macOS/Rosetta, opt-in). A short (< 5 byte) store that HEAD turns
// into ud2 is instead overwritten, together with the first byte of the instruction after it, by
// a rel32 jump to a trampoline holding the split store, a relocated copy of that next
// instruction, and a jump back. The trampoline is placed so the jump's last displacement byte
// (which lands on the next instruction's first byte) is `sentinel`, a one-byte opcode that is
// invalid in 64-bit mode. Falling through the store never executes it; a jump into the next
// instruction (a jump table, an unwinder landing pad, anything the sweep cannot see) executes the
// sentinel, raises SIGILL and is redirected to the relocated copy like a ud2 site. The rest of
// the next instruction's bytes are left intact, so later instruction boundaries still hold the
// original code.
struct WideStoreNoTrapZone {
	uint64_t              begin    = 0; // [begin, end) is mapped RWX, within +/-2 GiB of the code
	uint64_t              end      = 0;
	uint8_t               sentinel = 0; // the zone sits at displacement [sentinel << 24, +16 MiB)
	std::vector<uint64_t> chunk_used;   // bump cursor per 1 MiB chunk of the zone
};
[[nodiscard]] bool WideStoreNoTrapEnabled();
// Per-title default, used when KYTY_WIDE_STORE_NOTRAP is unset (=0/=1 always wins). Must be set
// before the first WideStoreNoTrapEnabled() call (the result is cached).
void SetWideStoreNoTrapDefault(bool enabled);
// The first sentinel whose 16 MiB window starts at least min_distance past the code base, or 0.
[[nodiscard]] uint8_t WideStoreNoTrapSentinel(uint64_t min_distance);
// Executes a sentinel byte in the zone once through the real SIGILL path. Needs the host
// exception handler installed. Returns false (feature stays off) if the redirect did not happen.
[[nodiscard]] bool WideStoreNoTrapSelfTest(WideStoreNoTrapZone* zone);

// Rosetta aborts the process when a 256-bit store that crosses into a write-protected page
// faults after writing its first half. 128-bit stores fault cleanly. Rewrite each plain
// 256-bit vector store in [address, address + size) into a jump to a trampoline that performs
// the store as two 128-bit halves and jumps back. Trampolines are written from *cursor up to
// trampoline_end, which must be within +/-2 GiB of the code.
// With notrap != nullptr, short stores that would become ud2 are patched trap-free instead where
// possible (see WideStoreNoTrapZone).
WideStoreSplitResult SplitWideStores(uint64_t address, uint64_t size, uint64_t* cursor,
                                     uint64_t trampoline_end, WideStoreNoTrapZone* notrap = nullptr);

} // namespace Loader::X64InstructionEmulator

#endif /* KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_ */
