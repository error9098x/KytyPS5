#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_WRITEFAULTSTATS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_WRITEFAULTSTATS_H_

// KYTY_WRITE_FAULT_STATS=1: measure-only per-page write-fault frequency counter.
//
// Answers the questions an adaptive write-protection policy (KYTY_ADAPTIVE_PROTECT) depends on and
// that no existing log answers:
//   * how many tracked write faults per second the guest takes (wfault),
//   * how many of them hit pages that faulted in >= 3 of the last 4 frames (hot) - the pages such a
//     policy would stop re-protecting,
//   * how many are a 2nd+ fault of the same page within one frame (repeat),
//   * how many pages the tracker write-protects / unprotects (reprot / unprot, from PageManager),
//   * how many SIGILL wide-store trap redirects happened in the same window (sigill) - the other
//     signal source that shows up as `_sigtramp -> Rosetta JIT` in the macOS samples.
// One `WriteFaultStats:` log line every 2 s. With the variable unset nothing is recorded and the
// only cost is one static-bool test per write fault and per PageManager watcher update.
//
// The table is lock-free and fixed-size (namespace-scope, constant-initialised) because it is
// updated from the SIGSEGV handler. It is statistics only: concurrent updates may race benignly.

#include "common/logging/log.h"
#include "loader/x64InstructionEmulator.h"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace Libs::Graphics::WriteFaultStats {

inline bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_WRITE_FAULT_STATS");
		return value != nullptr && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0);
	}();
	return enabled;
}

struct PageEntry {
	std::atomic<uint64_t> key {0};           // page number + 1; 0 = empty
	std::atomic<uint32_t> window_faults {0}; // faults since the last print
	std::atomic<uint32_t> last_frame {0};
	std::atomic<uint32_t> frame_bits {0};    // bit i = faulted in frame (last_frame - i)
};

inline constexpr size_t   kCapacity = size_t {1} << 16; // 64 Ki distinct pages (256 MiB)
inline constexpr uint32_t kMaxProbe = 32;

inline std::array<PageEntry, kCapacity> g_pages {};
inline std::atomic<uint64_t>            g_wfaults {0};
inline std::atomic<uint64_t>            g_hot_faults {0};
inline std::atomic<uint64_t>            g_repeat_faults {0};
inline std::atomic<uint64_t>            g_dropped {0};
inline std::atomic<uint64_t>            g_reprotect_pages {0};
inline std::atomic<uint64_t>            g_unprotect_pages {0};

// Called from RenderContext::HandleFault for a write fault on mapped (tracked) guest memory.
inline void NoteWriteFault(uint64_t vaddr, uint32_t frame) {
	g_wfaults.fetch_add(1, std::memory_order_relaxed);
	const uint64_t key  = (vaddr >> 12u) + 1;
	uint64_t       slot = (key * 0x9E3779B97F4A7C15ull) >> 48u; // 16 bits
	PageEntry*     entry = nullptr;
	for (uint32_t i = 0; i < kMaxProbe; i++, slot = (slot + 1) & (kCapacity - 1)) {
		auto&    candidate = g_pages[slot];
		uint64_t current   = candidate.key.load(std::memory_order_relaxed);
		if (current == 0 &&
		    candidate.key.compare_exchange_strong(current, key, std::memory_order_relaxed)) {
			current = key;
		}
		if (current == key) {
			entry = &candidate;
			break;
		}
	}
	if (entry == nullptr) {
		g_dropped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	entry->window_faults.fetch_add(1, std::memory_order_relaxed);
	const uint32_t last = entry->last_frame.exchange(frame, std::memory_order_relaxed);
	uint32_t       bits = entry->frame_bits.load(std::memory_order_relaxed);
	if (last == frame && bits != 0) {
		g_repeat_faults.fetch_add(1, std::memory_order_relaxed);
	} else {
		const uint32_t delta = frame - last;
		bits                 = (delta >= 32 ? 0u : (bits << delta)) | 1u;
		entry->frame_bits.store(bits, std::memory_order_relaxed);
	}
	if (std::popcount(bits & 0xfu) >= 3) {
		g_hot_faults.fetch_add(1, std::memory_order_relaxed);
	}
}

inline void NoteProtectionEdges(uint64_t reprotected, uint64_t unprotected) {
	if (reprotected != 0) {
		g_reprotect_pages.fetch_add(reprotected, std::memory_order_relaxed);
	}
	if (unprotected != 0) {
		g_unprotect_pages.fetch_add(unprotected, std::memory_order_relaxed);
	}
}

// Prints at most once per 2 s; the caller that wins the CAS prints. Like MaybePrintReadbackStats
// (bufferCache.cpp), this may run on a faulting guest thread.
inline void MaybePrint(uint32_t frame) {
	static std::atomic<int64_t>  last_us {0};
	static std::atomic<uint32_t> last_frame {0};
	static std::atomic<uint64_t> last_traps {0};
	const auto now_us = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	                                             std::chrono::steady_clock::now().time_since_epoch())
	                                             .count());
	auto last = last_us.load(std::memory_order_relaxed);
	if (last == 0) {
		if (last_us.compare_exchange_strong(last, now_us)) {
			last_frame.store(frame, std::memory_order_relaxed);
			last_traps.store(Loader::X64InstructionEmulator::WideStoreTrapHits(),
			                 std::memory_order_relaxed);
		}
		return;
	}
	if (now_us - last < 2000000 || !last_us.compare_exchange_strong(last, now_us)) {
		return;
	}
	uint64_t pages = 0;
	uint64_t hot_pages = 0;
	std::array<std::pair<uint32_t, uint64_t>, 6> top {};
	for (auto& entry: g_pages) {
		const uint32_t hits = entry.window_faults.exchange(0, std::memory_order_relaxed);
		if (hits == 0) {
			continue;
		}
		pages++;
		hot_pages += std::popcount(entry.frame_bits.load(std::memory_order_relaxed) & 0xfu) >= 3;
		const uint64_t page = (entry.key.load(std::memory_order_relaxed) - 1) << 12u;
		if (hits > top.back().first) {
			top.back() = {hits, page};
			for (size_t i = top.size() - 1; i > 0 && top[i].first > top[i - 1].first; i--) {
				std::swap(top[i], top[i - 1]);
			}
		}
	}
	const uint64_t traps      = Loader::X64InstructionEmulator::WideStoreTrapHits();
	const uint64_t prev_traps = last_traps.exchange(traps, std::memory_order_relaxed);
	const uint32_t prev_frame = last_frame.exchange(frame, std::memory_order_relaxed);
	char line[512];
	int  n = std::snprintf(
        line, sizeof(line),
        "WriteFaultStats: dt_ms=%" PRId64 " frames=%u wfault=%" PRIu64 " hot=%" PRIu64
        " repeat=%" PRIu64 " pages=%" PRIu64 " hot_pages=%" PRIu64 " reprot=%" PRIu64
        " unprot=%" PRIu64 " sigill=%" PRIu64 " dropped=%" PRIu64 " top=",
        (now_us - last) / 1000, frame - prev_frame, g_wfaults.exchange(0), g_hot_faults.exchange(0),
        g_repeat_faults.exchange(0), pages, hot_pages, g_reprotect_pages.exchange(0),
        g_unprotect_pages.exchange(0), traps - prev_traps, g_dropped.exchange(0));
	for (size_t i = 0; i < top.size() && top[i].first != 0 && n > 0 &&
	                   n < static_cast<int>(sizeof(line)) - 40;
	     i++) {
		n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), "%s0x%" PRIx64 ":%u",
		                   i == 0 ? "" : ",", top[i].second, top[i].first);
	}
	LOGF("%s\n", line);
}

} // namespace Libs::Graphics::WriteFaultStats

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_WRITEFAULTSTATS_H_
