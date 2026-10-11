#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SYNCAUDIT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SYNCAUDIT_H_

// KYTY_SYNC_AUDIT=1: per-callsite counters for every place a CPU thread waits on the host GPU
// timeline (CommandScheduler::Wait / Finish / FlushAndWait / WaitPriorityOperations). Callers tag
// the site with a Scope; untagged waits count as "other". Every 2 s one line is logged:
//   SyncAudit: dt_ms=N <site>=calls/drains/blocked/wait_us/max_us ...
// calls   = waits entered at this site
// drains  = waits that had to submit the open command buffer first (tick == CurrentTick / Finish)
// blocked = waits whose tick was not yet signalled on entry (the CPU really slept)
// wait_us = total wall time inside the wait, max_us = longest single wait in the interval
// Header-only so the build needs no source-list change. Off: one static bool test per wait.

#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/logging/log.h"

namespace Libs::Graphics::SyncAudit {

enum class Site : int {
	Other = 0,
	ReadbackFault,    // BufferCache::ReadMemory, guest-thread fault (via SendCommandSync)
	ReadbackSelf,     // BufferCache::ReadMemory, GPU-thread self fault (e.g. CPU-read indirect args)
	ReadbackClear,    // TextureCache::MaterializeColorClear
	ReadbackIndirect, // BindSharedMemory indirect dispatch args
	ReadbackInval,    // write fault on a GPU-dirty page (InvalidateMemory)
	Gc,               // BufferCache::RunGarbageCollector drain
	FaultBuffer,      // FaultManager::ProcessFaultBuffer ring slot reuse
	StreamWrap,       // StreamBuffer::WaitPendingOperations
	Unmap,            // RenderContext::UnmapMemory cached / pending-priority path
	GdsEop,           // CommandProcessor::SynchronizeGpu (GDS EOP)
	Pending,          // PopPendingOperations -> WaitPriorityOperations (draw/dispatch prologue)
	Count
};

inline bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_SYNC_AUDIT");
		return value != nullptr && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0);
	}();
	return enabled;
}

struct Counters {
	std::atomic<uint64_t> calls {0};
	std::atomic<uint64_t> drains {0};
	std::atomic<uint64_t> blocked {0};
	std::atomic<uint64_t> wait_us {0};
	std::atomic<uint64_t> max_us {0};
};

inline std::array<Counters, static_cast<size_t>(Site::Count)> g_counters;
inline thread_local Site                                    g_site = Site::Other;

struct Scope {
	explicit Scope(Site site): prev(g_site) { g_site = site; }
	~Scope() { g_site = prev; }
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;
	Site prev;
};

inline void MaybePrint() {
	static std::atomic<int64_t> last_us {0};
	const auto now_us = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	                                             std::chrono::steady_clock::now().time_since_epoch())
	                                             .count());
	auto last = last_us.load(std::memory_order_relaxed);
	if (last == 0) {
		last_us.compare_exchange_strong(last, now_us);
		return;
	}
	if (now_us - last < 2000000 || !last_us.compare_exchange_strong(last, now_us)) {
		return;
	}
	static const char* names[] = {"other", "rb_fault", "rb_self", "rb_clear", "rb_indirect", "rb_inval",
	                              "gc",    "faultbuf", "stream",  "unmap",    "gds",         "pending"};
	static_assert(std::size(names) == static_cast<size_t>(Site::Count));
	char line[1024];
	int  n = std::snprintf(line, sizeof(line), "SyncAudit: dt_ms=%" PRId64, (now_us - last) / 1000);
	for (size_t i = 0; i < g_counters.size() && n < static_cast<int>(sizeof(line)) - 80; i++) {
		auto&      c     = g_counters[i];
		const auto calls = c.calls.exchange(0);
		const auto drains = c.drains.exchange(0);
		const auto blocked = c.blocked.exchange(0);
		const auto wait_us = c.wait_us.exchange(0);
		const auto max_us  = c.max_us.exchange(0);
		if (calls == 0) {
			continue;
		}
		n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
		                   " %s=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "us/%" PRIu64 "us",
		                   names[i], calls, drains, blocked, wait_us, max_us);
	}
	LOGF("%s\n", line);
}

inline void Record(bool drain, bool blocked, std::chrono::steady_clock::time_point start) {
	const auto us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	                                          std::chrono::steady_clock::now() - start)
	                                          .count());
	auto& c = g_counters[static_cast<size_t>(g_site)];
	c.calls.fetch_add(1, std::memory_order_relaxed);
	c.drains.fetch_add(drain ? 1 : 0, std::memory_order_relaxed);
	c.blocked.fetch_add(blocked ? 1 : 0, std::memory_order_relaxed);
	c.wait_us.fetch_add(us, std::memory_order_relaxed);
	auto prev = c.max_us.load(std::memory_order_relaxed);
	while (prev < us && !c.max_us.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
	}
	MaybePrint();
}

} // namespace Libs::Graphics::SyncAudit

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SYNCAUDIT_H_
