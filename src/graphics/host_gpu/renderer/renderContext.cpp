#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/syncAudit.h"

#include <cinttypes>

#include <cstdlib>
#include <cstring>

#include <cstdio>

#include "graphics/host_gpu/writeFaultStats.h" // KYTY_WRITE_FAULT_STATS

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"

#include <algorithm>

namespace Libs::Graphics {

RenderContext::RenderContext(GraphicContext& graphics)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr,
                                bool wide_access) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	static const bool  log_faults = std::getenv("KYTY_FAULT_LOG") != nullptr;
	if (log_faults) {
		std::fprintf(stderr, "Fault: access=%c vaddr=0x%016" PRIx64 " page_off=0x%03" PRIx64 " mapped=%d\n",
		             access == PageFaultAccess::Write ? 'W' : 'R', fault_vaddr, fault_vaddr & 0xfffu,
		             IsMapped(fault_vaddr, fault_size) ? 1 : 0);
		std::fflush(stderr);
	}
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	if (access == PageFaultAccess::Write) {
		if (WriteFaultStats::Enabled()) {
			const auto frame = static_cast<uint32_t>(m_gpu != nullptr ? m_gpu->GetFrameNum() : 0);
			WriteFaultStats::NoteWriteFault(fault_vaddr, frame);
			WriteFaultStats::MaybePrint(frame);
		}
		if (ReadbackStats::Enabled()) {
			ReadbackStats::NoteWriteFault(fault_vaddr);
		}
		// KYTY_WRITE_FAULT_SPAN=<pages> (opt-in): guest CPU writers (ring/uniform/vertex
		// streams) walk tracked pages sequentially and take one fault per 4 KiB page. On a
		// write fault, also lift write tracking of the following pages, up to <pages> in total,
		// stopping at the first page that is unmapped, GPU-modified (that would need a
		// readback) or owned by a cached image (that would force a texture re-upload). The
		// extra pages only become CPU-modified, i.e. they are re-uploaded at the next sync.
		static const uint64_t write_span_pages = [] {
			const char* v = std::getenv("KYTY_WRITE_FAULT_SPAN");
			return v == nullptr ? uint64_t {0} : std::min<uint64_t>(std::strtoull(v, nullptr, 10), 256);
		}();
		if (write_span_pages > 1) {
			const uint64_t page_size = m_page_manager.GetPageSize();
			const uint64_t first     = fault_vaddr & ~(page_size - 1u);
			uint64_t       end       = first + page_size;
			while ((end - first) / page_size < write_span_pages && IsMapped(end, page_size) &&
			       !m_buffer_cache.IsRegionGpuModified(end, page_size) &&
			       !m_texture_cache.IsRegionRegistered(end, page_size)) {
				end += page_size;
			}
			m_buffer_cache.InvalidateMemory(first, end - first);
		} else {
			m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		}
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		ReadbackStats::Scope scope(ReadbackStats::Origin::Fault);
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
#if defined(__APPLE__)
	// Rosetta executes a 256-bit AVX load/store as two 128-bit halves. If the first half commits
	// and the second then faults on a still-protected page, Rosetta aborts the process
	// ("unexpectedly need to EmulateForward on a synchronous exception") before this handler can
	// run. A long sequential 256-bit loop (memset/memcpy over tracked memory) would cross into the
	// next protected page after every handled fault, so resolve the whole contiguous protected
	// run ahead of the access in one step. This trades finer dirty tracking for not crashing.
	// Experimental and opt-in (KYTY_ROSETTA_RUNAHEAD=1): it removes the chained-fault case but
	// not the entry-boundary case (a writer crossing from an untracked page into a tracked one
	// never faults first), and it over-invalidates tracked memory.
	static const bool run_ahead_enabled = std::getenv("KYTY_ROSETTA_RUNAHEAD") != nullptr;
	if (wide_access && run_ahead_enabled) {
		// Window the sequential loop is assumed to stay within, and a cap on how many separate
		// protected runs are resolved per fault.
		constexpr uint64_t RunAheadWindowPages = 1024; // 4 MB
		constexpr size_t   MaxRuns             = 512;
		const uint64_t     page_size           = m_page_manager.GetPageSize();
		const uint64_t     next_page = (fault_vaddr & ~(page_size - 1u)) + page_size;
		const auto runs = m_page_manager.ProtectedRunsFrom(next_page, RunAheadWindowPages, MaxRuns);
		uint64_t   resolved_pages = 0;
		for (const auto& [address, count]: runs) {
			uint64_t pages = 0;
			if (IsMapped(address, count * page_size)) {
				pages = count;
			} else {
				while (pages < count && IsMapped(address + pages * page_size, page_size)) {
					++pages;
				}
			}
			if (pages == 0) {
				continue;
			}
			if (access == PageFaultAccess::Write) {
				m_buffer_cache.InvalidateMemory(address, pages * page_size);
				m_texture_cache.InvalidateMemory(address, pages * page_size);
			} else {
				m_buffer_cache.ReadMemory(address, pages * page_size);
			}
			resolved_pages += pages;
		}
		if (log_faults && resolved_pages != 0) {
			std::fprintf(stderr, "Fault: run-ahead resolved %" PRIu64 " pages in %zu runs after 0x%016" PRIx64 "\n",
			             resolved_pages, runs.size(), fault_vaddr);
			std::fflush(stderr);
		}
	}
#endif
	return true;
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
	g_cpu_dirty_generation.fetch_add(1, std::memory_order_acq_rel);
	QueueBdaDirtyRange(vaddr, size);
	BumpBdaSyncEpoch();
}

UnmapCounters& GetUnmapCounters() {
	static UnmapCounters counters;
	return counters;
}

// KYTY_UNMAP_FASTPATH=1: when the GPU thread is parked and nothing cached overlaps the range, do
// the (then trivial) unmap bookkeeping on the guest thread instead of a blocking GPU-thread round
// trip. The GPU thread is held parked meanwhile, so ordering is identical to SendCommandSync.
static bool UnmapFastPathEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_UNMAP_FASTPATH");
		return value != nullptr && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0);
	}();
	return enabled;
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	auto& counters = GetUnmapCounters();
	if (UnmapFastPathEnabled() && m_gpu != nullptr) {
		bool       declined = false;
		const bool done     = m_gpu->TryRunInlineWhileIdle([this, vaddr, size, &declined] {
            // The GPU thread is parked and cannot register cache objects, queue priority
            // operations or mark pages GPU-modified until we return.
            if (m_command_scheduler.HasPendingPriorityOperations() ||
                m_buffer_cache.IsRegionRegistered(vaddr, size) ||
                m_texture_cache.IsRegionRegistered(vaddr, size) ||
                m_buffer_cache.IsRegionGpuModified(vaddr, size)) {
                declined = true;
                return false;
            }
            // Not GPU-modified, so InvalidateRegion only marks pages CPU-modified (no flush,
            // no GPU command). TextureCache::UnmapMemory drops surface metadata under its lock.
            m_buffer_cache.InvalidateMemory(vaddr, size);
            m_texture_cache.UnmapMemory(vaddr, size);
            std::lock_guard lock(m_mapped_ranges_mutex);
            m_mapped_ranges.Subtract(vaddr, size);
            return true;
        });
		if (done) {
			counters.fast.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		(declined ? counters.slow_cached : counters.slow_busy).fetch_add(1, std::memory_order_relaxed);
	}
	const auto unmap = [this, vaddr, size] {
		// Check cache ownership on the GPU thread. Guest-memory callbacks can still
		// access a range with no cached data, so they must finish before it is unmapped.
		if (m_command_scheduler.Active() &&
		    (m_buffer_cache.IsRegionRegistered(vaddr, size) ||
		     m_texture_cache.IsRegionRegistered(vaddr, size) ||
		     m_command_scheduler.HasPendingPriorityOperations())) {
			const auto       tick = m_command_scheduler.CurrentTick();
			SyncAudit::Scope audit_scope(SyncAudit::Site::Unmap);
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.RetireImportsInRange(vaddr, size); // KYTY_GUEST_IMPORT only
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	const auto sync_start = std::chrono::steady_clock::now();
	m_gpu->SendCommandSync(unmap);
	counters.sync_us.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	                                                     std::chrono::steady_clock::now() - sync_start)
	                                                     .count()),
	                           std::memory_order_relaxed);
}

void RenderContext::PrepareBda() {
	m_buffer_cache.NoteBdaUse(); // KYTY_GUEST_IMPORT only; also when the scan below is skipped
	if (!m_bda_logged) {
		Log::WriteToConsoleAndLog("GPU: using buffer device address (BDA) shader memory access.\n");
		m_bda_logged = true;
	}
	// KYTY_BDA_SYNC_SKIP=1: the full scan below only uploads CPU-modified pages of cached buffers.
	// When no page became CPU-modified, no buffer was registered and no range was mapped since
	// the previous complete scan (g_cpu_dirty_generation unchanged), it finds nothing; skip it.
	static const bool skip_enabled = [] {
		const char* value = std::getenv("KYTY_BDA_SYNC_SKIP");
		return value != nullptr && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0);
	}();
	const uint64_t generation = g_cpu_dirty_generation.load(std::memory_order_acquire);
	if (skip_enabled && m_bda_synced_valid && generation == m_bda_synced_generation) {
		m_fault_process_pending = true;
		return;
	}
	static const bool epoch_enabled = [] {
		const char* value = std::getenv("KYTY_BDA_SYNC_EPOCH");
		return value != nullptr && *value != '\0' && *value != '0';
	}();
	const uint64_t epoch = g_bda_sync_epoch.load(std::memory_order_acquire);
	if (epoch_enabled && m_bda_synced_valid && epoch == m_bda_synced_epoch) {
		m_fault_process_pending = true;
		return;
	}
	m_bda_synced_epoch = epoch;
	std::shared_lock lock(m_mapped_ranges_mutex);
	bool full_scan = true;
	if (BdaDirtyRegionsEnabled() && m_bda_synced_valid) {
		std::vector<uint64_t> bases;
		bool                  overflow = false;
		{
			std::lock_guard dirty_lock(g_bda_dirty_regions.mutex);
			bases.swap(g_bda_dirty_regions.bases);
			overflow                     = g_bda_dirty_regions.overflow;
			g_bda_dirty_regions.overflow = false;
		}
		if (!overflow) {
			full_scan = false;
			std::sort(bases.begin(), bases.end());
			bases.erase(std::unique(bases.begin(), bases.end()), bases.end());
			for (const auto base: bases) {
				m_mapped_ranges.ForEachInRange(base, TRACKER_REGION_SIZE,
				                               [this](uint64_t start, uint64_t end) {
					                               m_buffer_cache.SynchronizeBuffersInRange(
					                                   start, end - start);
				                               });
			}
		}
	} else if (BdaDirtyRegionsEnabled()) {
		// First (complete) scan: everything queued so far is covered by it.
		std::lock_guard dirty_lock(g_bda_dirty_regions.mutex);
		g_bda_dirty_regions.bases.clear();
		g_bda_dirty_regions.overflow = false;
	}
	if (full_scan) {
		m_mapped_ranges.ForEach([this](uint64_t start, uint64_t end) {
			m_buffer_cache.SynchronizeBuffersInRange(start, end - start);
		});
	}
	m_bda_synced_generation = generation;
	m_bda_synced_valid      = true;
	m_fault_process_pending = true;
}

void RenderContext::RunGarbageCollector() {
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}
	if (Sync::EopTraceEnabled()) {
		Sync::EopTraceNote(registrations.empty() ? Sync::EopTraceKind::NoRegistration
		                                         : Sync::EopTraceKind::Executed);
	}

	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.event_id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
		if (Sync::EopTraceEnabled()) {
			Sync::EopTraceNote(Sync::EopTraceKind::Delivered);
		}
	}
}

} // namespace Libs::Graphics

