#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTHREADSTATS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTHREADSTATS_H_

// KYTY_GPU_THREAD_STATS=1: wall-time split of the guest GPU thread (the thread that runs
// GuestGpu::ThreadRun). Scopes are nested and accounted EXCLUSIVELY: time spent in an inner scope
// is charged to the inner category only, so the categories plus "other" add up to the wall time.
// Every ~2 s the GPU thread logs one line:
//   GpuThreadStats: dt_ms=N busy=P% idle=.. drain=.. [drain sites] submit=.. encode=.. commit=..
//                   pipeline=.. prep=.. bda=.. rt=.. cmd=.. other=.. draws=N dispatches=N
// values are milliseconds in the interval and (percent of wall).
//   idle     : waiting for guest work (m_work_available)
//   drain    : CommandScheduler::Wait/Finish/FlushAndWait/WaitPriorityOperations (CPU sleeps on the
//              host GPU timeline), split by SyncAudit site
//   submit   : CommandScheduler::Submit (vkEndCommandBuffer + vkQueueSubmit + bookkeeping)
//   encode   : command recording after resources are prepared (dynamic state, BeginRendering,
//              bindPipeline, draw/dispatch, barriers). With MVK PREFILL=2 this is where MoltenVK
//              encodes Metal commands inline.
//   commit   : RenderExecutor::CommitBindings (overlap checks, descriptor writes/pushes, binds)
//   pipeline : GetGraphicsPrograms/GetComputeProgram/GetGraphicsPipeline/GetComputePipeline
//   prep     : PrepareBindings/PrepareGraphicsBindings and the compute equivalent (buffer/texture
//              cache lookups, uploads, image rebinds), excluding bda
//   bda      : RenderContext::PrepareBda
//   rt       : AcquireRenderTargets
//   cmd      : SendCommand/SendCommandSync closures run on the GPU thread (faults, unmaps, ...)
//   other    : everything else on the thread (PM4 parsing, register state, ...)
// Off: one cached bool test per scope.

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/abToggle.h"

namespace Libs::Graphics::GpuThreadStats {

enum class Cat : int { Other = 0, Idle, Drain, Submit, Encode, Commit, Pipeline, Prep, Bda, Rt, Cmd, Count };

// KYTY_GPU_THREAD_STATS=1 logs the stats lines; KYTY_CLEAR_META_NOSYNC=auto also collects them
// (it is driven by them, see AbToggle::AutoUpdate) but logs nothing unless the former is set.
inline bool LogEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GPU_THREAD_STATS");
		return value != nullptr && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0);
	}();
	return enabled;
}

inline bool Enabled() {
	static const bool enabled = LogEnabled() || AbToggle::AutoWanted();
	return enabled;
}

constexpr int kDrainSites = 12; // == SyncAudit::Site::Count (static_assert in the .cpp users)

struct ThreadState {
	bool                                         attached = false;
	int64_t                                      interval_start_ns = 0;
	int64_t                                      last_ns           = 0;   // start of the current slice
	int                                          depth             = 0;
	std::array<Cat, 32>                          stack {};
	std::array<int, 32>                          site_stack {};
	std::array<uint64_t, static_cast<int>(Cat::Count)> ns {};
	std::array<uint64_t, kDrainSites>            drain_site_ns {};
	uint64_t                                     draws = 0, dispatches = 0;
};

inline thread_local ThreadState t_state;

inline int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

// Called once by the GPU thread. Only an attached thread records anything.
inline void Attach() {
	if (!Enabled()) {
		return;
	}
	t_state.attached          = true;
	t_state.interval_start_ns = NowNs();
	t_state.last_ns           = t_state.interval_start_ns;
	t_state.depth             = 0;
}

inline void Charge(int64_t now) {
	auto&      s   = t_state;
	const auto cat = s.depth > 0 ? s.stack[s.depth - 1] : Cat::Other;
	const auto d   = static_cast<uint64_t>(now - s.last_ns);
	s.ns[static_cast<int>(cat)] += d;
	if (cat == Cat::Drain) {
		const int site = s.site_stack[s.depth - 1];
		if (site >= 0 && site < kDrainSites) {
			s.drain_site_ns[site] += d;
		}
	}
	s.last_ns = now;
}

struct Scope {
	explicit Scope(Cat cat, int site = 0) {
		if (!Enabled() || !t_state.attached || t_state.depth >= 32) {
			return;
		}
		active = true;
		Charge(NowNs());
		t_state.stack[t_state.depth]      = cat;
		t_state.site_stack[t_state.depth] = site;
		t_state.depth++;
	}
	~Scope() {
		if (!active) {
			return;
		}
		Charge(NowNs());
		t_state.depth--;
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;
	bool active = false;
};

inline void NoteDraw() {
	if (Enabled() && t_state.attached) {
		t_state.draws++;
	}
}
inline void NoteDispatch() {
	if (Enabled() && t_state.attached) {
		t_state.dispatches++;
	}
}

// Call from the GPU thread loop (outside any scope). Logs and resets every 2 s.
inline void MaybePrint() {
	if (!Enabled() || !t_state.attached) {
		return;
	}
	auto&      s   = t_state;
	const auto now = NowNs();
	if (now - s.interval_start_ns < 2000000000LL) {
		return;
	}
	Charge(now);
	const double wall = static_cast<double>(now - s.interval_start_ns);
	static const char* cat_names[] = {"other", "idle", "drain", "submit", "encode", "commit",
	                                  "pipeline", "prep", "bda", "rt", "cmd"};
	static_assert(std::size(cat_names) == static_cast<size_t>(Cat::Count));
	static const char* site_names[] = {"other", "rb_fault", "rb_self", "rb_clear", "rb_indirect", "rb_inval",
	                                   "gc",    "faultbuf", "stream",  "unmap",    "gds",         "pending"};
	static_assert(std::size(site_names) == kDrainSites);
	auto ms  = [](uint64_t ns) { return static_cast<double>(ns) / 1e6; };
	auto pct = [&](uint64_t ns) { return 100.0 * static_cast<double>(ns) / wall; };
	const uint64_t idle = s.ns[static_cast<int>(Cat::Idle)];
	AbToggle::AutoUpdate(static_cast<double>(s.drain_site_ns[3]) / wall, static_cast<double>(idle) / wall,
	                     static_cast<double>(s.ns[static_cast<int>(Cat::Drain)]) / wall,
	                     static_cast<double>(s.draws) * 1e9 / wall);
	if (!LogEnabled()) {
		s.ns.fill(0);
		s.drain_site_ns.fill(0);
		s.draws = s.dispatches = 0;
		s.interval_start_ns    = now;
		return;
	}
	char line[1400];
	int n = std::snprintf(line, sizeof(line), "GpuThreadStats: dt_ms=%.0f busy=%.1f%%", wall / 1e6,
	                      100.0 - pct(idle));
	for (int i = 1; i < static_cast<int>(Cat::Count); i++) {
		n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " %s=%.0f(%.1f%%)", cat_names[i],
		                   ms(s.ns[i]), pct(s.ns[i]));
		if (i == static_cast<int>(Cat::Drain)) {
			n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " [");
			for (int k = 0; k < kDrainSites; k++) {
				if (s.drain_site_ns[k] >= 1000000) {
					n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), "%s=%.0f ", site_names[k],
					                   ms(s.drain_site_ns[k]));
				}
			}
			n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), "]");
		}
	}
	n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
	                   " other=%.0f(%.1f%%) draws=%" PRIu64 " dispatches=%" PRIu64, ms(s.ns[0]), pct(s.ns[0]),
	                   s.draws, s.dispatches);
	LOGF("%s\n", line);
	s.ns.fill(0);
	s.drain_site_ns.fill(0);
	s.draws = s.dispatches = 0;
	s.interval_start_ns    = now;
}

} // namespace Libs::Graphics::GpuThreadStats

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTHREADSTATS_H_
