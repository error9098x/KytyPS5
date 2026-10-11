#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GUESTIMPORT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GUESTIMPORT_H_

// KYTY_GUEST_IMPORT=1 (opt-in): guest buffer ranges are imported as VkBuffers over the guest
// direct-memory backing (VK_EXT_external_memory_host -> MoltenVK newBufferWithBytesNoCopy), so the
// buffer cache keeps no second, device-local copy of them and never uploads or downloads them.
//
// Coherence model (see PROOF.md section 6). Kyty writes EOP labels when the command processor
// records them, before the host GPU has executed the work. The guest CPU can therefore run ahead
// of the host GPU, so an imported page still needs ordering against in-flight GPU work:
//   * GPU write -> CPU read : the page stays GPU-dirty (read-protected) until its last writing
//     submission has completed. A fault waits for that submission only (no copy). A deferred
//     operation lifts the protection once the writer has retired, so late readers never fault.
//   * GPU read -> CPU write : clean imported pages stay write-protected. A CPU write fault waits
//     for the last submission that may read the page (write-after-read), then proceeds.
//     Imports that keep taking such waits are demoted to the copying path.
// HazardTicks records, per 16 KiB page, the newest submission tick that reads or writes it.
// Everything that is not import-specific (tracker states, protection, BDA table) is unchanged.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "common/logging/log.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::GuestImport {

enum class Class : uint32_t {
	Storage  = 1u << 0u,
	Vertex   = 1u << 1u,
	Index    = 1u << 2u,
	Constant = 1u << 3u,
	Bda      = 1u << 4u,
	Indirect = 1u << 5u,
	Other    = 1u << 6u,
};

inline constexpr uint32_t kAllClasses     = 0x7fu;
inline constexpr uint32_t kDefaultClasses = static_cast<uint32_t>(Class::Storage) |
                                            static_cast<uint32_t>(Class::Vertex) |
                                            static_cast<uint32_t>(Class::Index) |
                                            static_cast<uint32_t>(Class::Indirect) |
                                            static_cast<uint32_t>(Class::Bda);

// Parses KYTY_GUEST_IMPORT_CLASSES ("storage,vertex,index,constant,bda,indirect,other,all").
// nullptr or an empty string gives kDefaultClasses; unknown words are ignored.
[[nodiscard]] inline uint32_t ParseClassMask(const char* text) noexcept;
[[nodiscard]] inline bool     Enabled() noexcept;      // KYTY_GUEST_IMPORT=1|true, parsed once
[[nodiscard]] inline bool     StatsEnabled() noexcept; // KYTY_GUEST_IMPORT_STATS=1|true, parsed once
[[nodiscard]] inline bool     ClassAllowed(Class c) noexcept;

// Host page size under Rosetta may be 4 KiB (sysconf), but the Apple GPU maps 16 KiB pages and
// the buffer cache works in 16 KiB pages; imports are always 16 KiB aligned in pointer and size.
inline constexpr uint64_t kPageBits = 14;
inline constexpr uint64_t kPageSize = uint64_t {1} << kPageBits;
// Uses that cover more than this many pages update one whole-buffer tick instead of per page.
inline constexpr uint64_t kBulkPages = 64;
// Write-after-read waits an import may take before it is demoted to the copying path.
inline constexpr uint32_t kWarDemoteThreshold = 2;

[[nodiscard]] constexpr bool IsAlignedRange(uint64_t host, uint64_t size, uint64_t alignment) noexcept {
	return alignment != 0 && size != 0 && host % alignment == 0 && size % alignment == 0;
}

// Per-import hazard ticks. Written on the GPU thread at record time, read by any thread.
class HazardTicks {
public:
	HazardTicks(uint64_t base, uint64_t size);

	void NoteUse(uint64_t vaddr, uint64_t size, uint64_t tick, bool write) noexcept;
	// Newest tick that reads or writes any page of [vaddr, vaddr+size) (0 = never used).
	[[nodiscard]] uint64_t LastUse(uint64_t vaddr, uint64_t size) const noexcept;
	// Newest tick that writes any page of [vaddr, vaddr+size).
	[[nodiscard]] uint64_t LastWrite(uint64_t vaddr, uint64_t size) const noexcept;
	// Carries over another import's ticks for the overlapping range (both alias the same memory).
	void MergeFrom(const HazardTicks& other) noexcept;
	[[nodiscard]] uint64_t Base() const noexcept { return m_base; }
	[[nodiscard]] uint64_t Size() const noexcept { return m_size; }
	[[nodiscard]] uint64_t Serial() const noexcept { return m_serial; }

	std::atomic<uint32_t> war_waits {0};
	std::atomic<bool>     demote_requested {false};
	// Tick at which the buffer left the cache (0 = live). A retired import stays in the registry
	// until every submission that could still access it has completed (see Registry).
	std::atomic<uint64_t> retired_tick {0};
	// Tick at which it entered the BDA page table; BDA shaders recorded earlier cannot reach it.
	std::atomic<uint64_t> created_tick {0};
	uint64_t              unmark_queued_tick = 0; // GPU thread only

private:
	[[nodiscard]] bool Clamp(uint64_t vaddr, uint64_t size, uint64_t& first,
	                         uint64_t& last) const noexcept;
	[[nodiscard]] uint64_t Max(const std::atomic<uint64_t>* ticks, const std::atomic<uint64_t>& bulk,
	                           uint64_t vaddr, uint64_t size) const noexcept;

	uint64_t                                 m_base;
	uint64_t                                 m_size;
	uint64_t                                 m_pages;
	uint64_t                                 m_serial;
	std::unique_ptr<std::atomic<uint64_t>[]> m_use;
	std::unique_ptr<std::atomic<uint64_t>[]> m_write;
	std::atomic<uint64_t>                    m_bulk_use {0};
	std::atomic<uint64_t>                    m_bulk_write {0};
};

// Thread-safe index of imports by guest range, read by CPU fault handlers on guest threads.
// Live imports never overlap (as in the cache), but a retired import stays listed until its last
// access has completed, and a new import is listed before the ones it absorbs are retired, so
// entries may overlap and share a base: a multimap scanned from (vaddr - largest size).
class Registry {
public:
	void Insert(const std::shared_ptr<HazardTicks>& ticks);   // idempotent
	void Erase(const HazardTicks* ticks);
	[[nodiscard]] bool Empty() const noexcept { return m_count.load(std::memory_order_acquire) == 0; }

	template <typename Func>
	void ForEachOverlap(uint64_t vaddr, uint64_t size, Func&& func) const {
		if (Empty() || size == 0) {
			return;
		}
		std::shared_lock lock(m_mutex);
		auto it = m_map.lower_bound(vaddr - std::min(vaddr, m_max_size));
		for (; it != m_map.end() && it->first < vaddr + size; ++it) {
			const auto& ticks = *it->second;
			if (ticks.Base() + ticks.Size() > vaddr) {
				func(*it->second);
			}
		}
	}

private:
	mutable std::shared_mutex                              m_mutex;
	std::multimap<uint64_t, std::shared_ptr<HazardTicks>> m_map;
	uint64_t                                               m_max_size = 0;
	std::atomic<size_t>                                    m_count {0};
};

// Newest submission tick that recorded a shader which can reach any registered buffer through
// the BDA page table (every registered buffer is in the table).
inline std::atomic<uint64_t> g_bda_use_tick {0};
// Set when an imported range was GPU-written in the open command buffer: the next submission
// ends with a memory dependency to the host domain, so the CPU may read the bytes after the wait.
inline std::atomic<bool> g_host_barrier_pending {false};
// Called by CommandScheduler::Submit (GPU thread) before the open submission is closed, with the
// tick of the submission that opens next. Installed by the BufferCache when the flag is on.
inline void (*g_submit_hook)(void* context, uint64_t next_tick) = nullptr;
inline void* g_submit_hook_context                               = nullptr;

// Newest submission that may still access [vaddr, vaddr+size) through this import: its own reads
// and writes, plus any BDA shader recorded while it was in the page table.
[[nodiscard]] inline uint64_t PendingUse(const HazardTicks& ticks, uint64_t vaddr, uint64_t size) noexcept {
	const auto retired = ticks.retired_tick.load();
	const auto bda     = g_bda_use_tick.load();
	const auto via_bda = bda < ticks.created_tick.load() ? 0 : retired == 0 ? bda : std::min(bda, retired);
	return std::max(ticks.LastUse(vaddr, size), via_bda);
}

struct Stats {
	std::atomic<uint64_t> created {0};
	std::atomic<uint64_t> live_bytes {0};
	std::atomic<uint64_t> peak_bytes {0};
	std::atomic<uint64_t> fallback_class {0};
	std::atomic<uint64_t> fallback_backing {0};   // unmapped, flexible, or discontiguous backing
	std::atomic<uint64_t> fallback_align {0};
	std::atomic<uint64_t> fallback_vulkan {0};
	std::atomic<uint64_t> fallback_demoted {0};
	std::atomic<uint64_t> upload_bytes_skipped {0};
	std::atomic<uint64_t> download_bytes_skipped {0};
	std::atomic<uint64_t> read_waits {0};         // CPU read/write of GPU-dirty page: writer pending
	std::atomic<uint64_t> read_free {0};          // ... writer already retired: no wait at all
	std::atomic<uint64_t> war_waits {0};          // CPU write while a reader is pending
	std::atomic<uint64_t> war_free {0};
	std::atomic<uint64_t> war_self_pending {0};  // GPU-thread write into a range its own unrecorded packet uses
	std::atomic<uint64_t> proactive_unmarks {0};  // pages unprotected after their writer retired
	std::atomic<uint64_t> gc_import_retires {0};
	std::atomic<uint64_t> gc_import_waits {0};
	std::atomic<uint64_t> demotions {0};
	std::atomic<uint64_t> unmap_retires {0};
	std::atomic<uint64_t> gpu_path_redirects {0}; // CPU fill/copy turned into GPU ops (ordering)
};
inline Stats& GetStats() noexcept;
inline void   NoteLive(int64_t delta) noexcept;
inline void   MaybeLogStats(uint64_t device_usage_bytes);

// ---- implementation (header-only: no build-system change) ----


namespace Detail {

inline bool EnvFlag(const char* name) noexcept {
	const char* value = std::getenv(name);
	return value != nullptr && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0);
}

inline std::atomic<uint64_t> g_serial {0};

} // namespace Detail

inline uint32_t ParseClassMask(const char* text) noexcept {
	if (text == nullptr || *text == '\0') {
		return kDefaultClasses;
	}
	struct Word {
		const char* name;
		uint32_t    bits;
	};
	static constexpr Word words[] = {
	    {"storage", static_cast<uint32_t>(Class::Storage)},
	    {"vertex", static_cast<uint32_t>(Class::Vertex)},
	    {"index", static_cast<uint32_t>(Class::Index)},
	    {"constant", static_cast<uint32_t>(Class::Constant)},
	    {"bda", static_cast<uint32_t>(Class::Bda)},
	    {"indirect", static_cast<uint32_t>(Class::Indirect)},
	    {"other", static_cast<uint32_t>(Class::Other)},
	    {"all", kAllClasses},
	};
	uint32_t    mask = 0;
	const char* p    = text;
	while (*p != '\0') {
		const char* end = p;
		while (*end != '\0' && *end != ',') {
			++end;
		}
		const auto length = static_cast<size_t>(end - p);
		for (const auto& word: words) {
			if (std::strlen(word.name) == length && std::strncmp(word.name, p, length) == 0) {
				mask |= word.bits;
			}
		}
		p = *end == ',' ? end + 1 : end;
	}
	return mask;
}

inline bool Enabled() noexcept {
	static const bool enabled = Detail::EnvFlag("KYTY_GUEST_IMPORT");
	return enabled;
}

inline bool StatsEnabled() noexcept {
	static const bool enabled = Detail::EnvFlag("KYTY_GUEST_IMPORT_STATS");
	return enabled;
}

inline bool ClassAllowed(Class c) noexcept {
	static const uint32_t mask = ParseClassMask(std::getenv("KYTY_GUEST_IMPORT_CLASSES"));
	return (mask & static_cast<uint32_t>(c)) != 0;
}

inline HazardTicks::HazardTicks(uint64_t base, uint64_t size)
    : m_base(base), m_size(size), m_pages((size + kPageSize - 1) >> kPageBits),
      m_serial(Detail::g_serial.fetch_add(1, std::memory_order_relaxed) + 1),
      m_use(new std::atomic<uint64_t>[m_pages]), m_write(new std::atomic<uint64_t>[m_pages]) {
	for (uint64_t i = 0; i < m_pages; ++i) {
		m_use[i].store(0, std::memory_order_relaxed);
		m_write[i].store(0, std::memory_order_relaxed);
	}
}

inline bool HazardTicks::Clamp(uint64_t vaddr, uint64_t size, uint64_t& first,
                        uint64_t& last) const noexcept {
	const auto begin = std::max(vaddr, m_base);
	const auto end   = std::min(vaddr + size, m_base + m_size);
	if (size == 0 || begin >= end) {
		return false;
	}
	first = (begin - m_base) >> kPageBits;
	last  = ((end - m_base) + kPageSize - 1) >> kPageBits;
	return true;
}

inline void HazardTicks::NoteUse(uint64_t vaddr, uint64_t size, uint64_t tick, bool write) noexcept {
	uint64_t first = 0;
	uint64_t last  = 0;
	if (!Clamp(vaddr, size, first, last)) {
		return;
	}
	// Ticks only grow (recorded in submission order on the GPU thread); seq_cst so that a fault
	// handler that observed a label written after this use also observes the tick.
	// Single writer (the GPU thread), so load-compare-store keeps every tick monotonic.
	const auto raise = [tick](std::atomic<uint64_t>& value) {
		if (value.load() < tick) {
			value.store(tick);
		}
	};
	if (last - first > kBulkPages) {
		raise(m_bulk_use);
		if (write) {
			raise(m_bulk_write);
		}
		return;
	}
	for (uint64_t page = first; page < last; ++page) {
		raise(m_use[page]);
		if (write) {
			raise(m_write[page]);
		}
	}
}

inline void HazardTicks::MergeFrom(const HazardTicks& other) noexcept {
	const auto begin = std::max(m_base, other.m_base);
	const auto end   = std::min(m_base + m_size, other.m_base + other.m_size);
	if (begin >= end) {
		return;
	}
	for (auto page = begin; page < end; page = std::min(end, (page & ~(kPageSize - 1)) + kPageSize)) {
		const auto bytes = std::min(end, (page & ~(kPageSize - 1)) + kPageSize) - page;
		NoteUse(page, bytes, other.LastUse(page, bytes), false);
		NoteUse(page, bytes, other.LastWrite(page, bytes), true);
	}
}

inline uint64_t HazardTicks::Max(const std::atomic<uint64_t>* ticks, const std::atomic<uint64_t>& bulk,
                          uint64_t vaddr, uint64_t size) const noexcept {
	uint64_t first = 0;
	uint64_t last  = 0;
	if (!Clamp(vaddr, size, first, last)) {
		return 0;
	}
	uint64_t result = bulk.load();
	for (uint64_t page = first; page < last; ++page) {
		result = std::max(result, ticks[page].load());
	}
	return result;
}

inline uint64_t HazardTicks::LastUse(uint64_t vaddr, uint64_t size) const noexcept {
	return Max(m_use.get(), m_bulk_use, vaddr, size);
}

inline uint64_t HazardTicks::LastWrite(uint64_t vaddr, uint64_t size) const noexcept {
	return Max(m_write.get(), m_bulk_write, vaddr, size);
}

inline void Registry::Insert(const std::shared_ptr<HazardTicks>& ticks) {
	std::unique_lock lock(m_mutex);
	const auto [first, last] = m_map.equal_range(ticks->Base());
	for (auto it = first; it != last; ++it) {
		if (it->second == ticks) {
			return;
		}
	}
	m_map.emplace(ticks->Base(), ticks);
	m_max_size = std::max(m_max_size, ticks->Size());
	m_count.store(m_map.size(), std::memory_order_release);
}

inline void Registry::Erase(const HazardTicks* ticks) {
	std::unique_lock lock(m_mutex);
	const auto [first, last] = m_map.equal_range(ticks->Base());
	for (auto it = first; it != last; ++it) {
		if (it->second.get() == ticks) {
			m_map.erase(it);
			break;
		}
	}
	m_count.store(m_map.size(), std::memory_order_release);
}

inline Stats& GetStats() noexcept {
	static Stats stats;
	return stats;
}

inline void NoteLive(int64_t delta) noexcept {
	auto&      s   = GetStats();
	const auto now = s.live_bytes.fetch_add(static_cast<uint64_t>(delta)) + static_cast<uint64_t>(delta);
	auto       peak = s.peak_bytes.load(std::memory_order_relaxed);
	while (now > peak && !s.peak_bytes.compare_exchange_weak(peak, now)) {
	}
}

inline void MaybeLogStats(uint64_t device_usage_bytes) {
	if (!StatsEnabled()) {
		return;
	}
	static std::atomic<int64_t> last_us {0};
	const auto now_us = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	                                             std::chrono::steady_clock::now().time_since_epoch())
	                                             .count());
	auto last = last_us.load(std::memory_order_relaxed);
	if (now_us - last < 2000000 || !last_us.compare_exchange_strong(last, now_us)) {
		return;
	}
	auto&      s  = GetStats();
	const auto ex = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
	LOGF("GuestImport: live_mib=%" PRIu64 " peak_mib=%" PRIu64 " gpu_usage_mib=%" PRIu64
	     " created=%" PRIu64 " fb_class=%" PRIu64 " fb_backing=%" PRIu64 " fb_align=%" PRIu64
	     " fb_vk=%" PRIu64 " fb_demoted=%" PRIu64 " up_skip_mib=%" PRIu64 " dl_skip_kib=%" PRIu64
	     " rd_wait=%" PRIu64 " rd_free=%" PRIu64 " war_wait=%" PRIu64 " war_free=%" PRIu64
	     " war_self=%" PRIu64 " unmark=%" PRIu64 " gc_retire=%" PRIu64 " gc_wait=%" PRIu64
	     " demote=%" PRIu64 " unmap=%" PRIu64 " redirect=%" PRIu64 "\n",
	     s.live_bytes.load() >> 20u, s.peak_bytes.load() >> 20u, device_usage_bytes >> 20u,
	     ex(s.created), ex(s.fallback_class), ex(s.fallback_backing), ex(s.fallback_align),
	     ex(s.fallback_vulkan), ex(s.fallback_demoted), ex(s.upload_bytes_skipped) >> 20u,
	     ex(s.download_bytes_skipped) >> 10u, ex(s.read_waits), ex(s.read_free), ex(s.war_waits),
	     ex(s.war_free), ex(s.war_self_pending), ex(s.proactive_unmarks), ex(s.gc_import_retires), ex(s.gc_import_waits),
	     ex(s.demotions), ex(s.unmap_retires), ex(s.gpu_path_redirects));
}

} // namespace Libs::Graphics::GuestImport

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GUESTIMPORT_H_
