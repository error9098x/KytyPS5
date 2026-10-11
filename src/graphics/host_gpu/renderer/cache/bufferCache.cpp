#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;

bool EnvFlag(const char* name) {
	const char* value = std::getenv(name);
	return value != nullptr && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0);
}

// KYTY_FAULT_COALESCE=1: a CPU read or write fault on a GPU-dirty page downloads the whole
// contiguous GPU-dirty run around it (capped) instead of one 4 KiB page, so a CPU loop over an
// N-page GPU-written buffer costs one GPU drain instead of N.
bool FaultCoalesceEnabled() {
	static const bool enabled = EnvFlag("KYTY_FAULT_COALESCE");
	return enabled;
}

constexpr uint64_t FaultCoalesceMaxBytes = 1 * MiB;

bool FaultWaitOnCallerEnabled() {
	static const bool enabled = EnvFlag("KYTY_FAULT_WAIT_ON_CALLER");
	return enabled;
}

struct ReadbackCounters {
	std::atomic<uint64_t> count {0};
	std::atomic<uint64_t> bytes {0};
	std::atomic<uint64_t> wall_us {0};
	std::atomic<uint64_t> wait_us {0};
};
std::array<ReadbackCounters, static_cast<size_t>(ReadbackStats::Origin::Count)> g_readback;
std::atomic<uint64_t> g_cpu_fills {0};
std::atomic<uint64_t> g_cpu_fill_bytes {0};
thread_local ReadbackStats::Origin g_readback_origin = ReadbackStats::Origin::Other;
std::mutex                         g_fault_pages_mutex;
std::unordered_map<uint64_t, uint32_t> g_fault_pages;
std::unordered_map<uint64_t, uint32_t> g_wfault_pages;
std::atomic<uint64_t> g_wfaults {0};
std::atomic<uint64_t> g_rp_begin {0};
std::atomic<uint64_t> g_rp_switch {0};
std::atomic<uint64_t> g_rp_end {0};
std::mutex            g_rp_callers_mutex;
std::unordered_map<uint64_t, uint32_t> g_rp_end_callers; // (caller1 << 32 ^ caller0) folded
std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> g_rp_end_caller_pcs;

uint64_t MicrosSince(std::chrono::steady_clock::time_point start) {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	                                 std::chrono::steady_clock::now() - start)
	                                 .count());
}

void MaybePrintReadbackStats() {
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
	static const char* names[] = {"other", "fault", "clear", "indirect", "inval"};
	char line[1536];
	int  n = std::snprintf(line, sizeof(line), "ReadbackStats: dt_ms=%" PRId64,
	                       (now_us - last) / 1000);
	for (size_t i = 0; i < g_readback.size(); i++) {
		auto& c = g_readback[i];
		n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
		                   " %s=%" PRIu64 "/%" PRIu64 "B/%" PRIu64 "us/%" PRIu64 "us", names[i],
		                   c.count.exchange(0), c.bytes.exchange(0), c.wall_us.exchange(0),
		                   c.wait_us.exchange(0));
	}
	n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
	                   " cpufill=%" PRIu64 "/%" PRIu64 "B", g_cpu_fills.exchange(0),
	                   g_cpu_fill_bytes.exchange(0));
	std::vector<std::pair<uint32_t, uint64_t>> top;
	{
		std::scoped_lock lock {g_fault_pages_mutex};
		for (const auto& [page, hits]: g_fault_pages) {
			top.emplace_back(hits, page);
		}
		g_fault_pages.clear();
	}
	std::sort(top.rbegin(), top.rend());
	n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " fault_pages=%zu top=",
	                   top.size());
	for (size_t i = 0; i < top.size() && i < 6 && n < static_cast<int>(sizeof(line)) - 40; i++) {
		n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), "%s0x%" PRIx64 ":%u",
		                   i == 0 ? "" : ",", top[i].second, top[i].first);
	}
	std::vector<std::pair<uint32_t, uint64_t>> wtop;
	{
		std::scoped_lock lock {g_fault_pages_mutex};
		for (const auto& [page, hits]: g_wfault_pages) {
			wtop.emplace_back(hits, page);
		}
		g_wfault_pages.clear();
	}
	std::sort(wtop.rbegin(), wtop.rend());
	n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
	                   " wfault=%" PRIu64 " wfault_pages=%zu wtop=", g_wfaults.exchange(0),
	                   wtop.size());
	for (size_t i = 0; i < wtop.size() && i < 4 && n < static_cast<int>(sizeof(line)) - 80; i++) {
		n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), "%s0x%" PRIx64 ":%u",
		                   i == 0 ? "" : ",", wtop[i].second, wtop[i].first);
	}
	n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
	                   " rp_begin=%" PRIu64 " rp_switch=%" PRIu64 " rp_end=%" PRIu64,
	                   g_rp_begin.exchange(0), g_rp_switch.exchange(0), g_rp_end.exchange(0));
	{
		std::vector<std::pair<uint32_t, uint64_t>> callers;
		std::scoped_lock lock {g_rp_callers_mutex};
		for (const auto& [key, hits]: g_rp_end_callers) {
			callers.emplace_back(hits, key);
		}
		std::sort(callers.rbegin(), callers.rend());
		for (size_t i = 0; i < callers.size() && i < 6 && n < static_cast<int>(sizeof(line)) - 64;
		     i++) {
			const auto& pcs = g_rp_end_caller_pcs[callers[i].second];
			n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
			                   "%s0x%" PRIx64 "<0x%" PRIx64 ":%u", i == 0 ? " rp_end_by=" : ",",
			                   pcs.first, pcs.second, callers[i].first);
		}
		g_rp_end_callers.clear();
		g_rp_end_caller_pcs.clear();
	}
	LOGF("%s\n", line);
}

} // namespace

namespace ReadbackStats {
bool Enabled() {
	static const bool enabled = EnvFlag("KYTY_READBACK_STATS");
	return enabled;
}
Origin Current() {
	return g_readback_origin;
}
void NoteCpuFill(uint64_t bytes) {
	g_cpu_fills.fetch_add(1, std::memory_order_relaxed);
	g_cpu_fill_bytes.fetch_add(bytes, std::memory_order_relaxed);
}
void NoteWriteFault(uint64_t vaddr) {
	g_wfaults.fetch_add(1, std::memory_order_relaxed);
	{
		std::scoped_lock lock {g_fault_pages_mutex};
		g_wfault_pages[vaddr & ~(TRACKER_PAGE_SIZE - 1)]++;
	}
	MaybePrintReadbackStats();
}
void NoteRenderPass(bool begin, bool replaced_open_pass, const void* caller0,
                    const void* caller1) {
	if (begin) {
		g_rp_begin.fetch_add(1, std::memory_order_relaxed);
		if (replaced_open_pass) {
			g_rp_switch.fetch_add(1, std::memory_order_relaxed);
		}
	} else {
		g_rp_end.fetch_add(1, std::memory_order_relaxed);
		const auto pc0 = reinterpret_cast<uint64_t>(caller0);
		const auto pc1 = reinterpret_cast<uint64_t>(caller1);
		const auto key = pc0 * 0x9E3779B97F4A7C15ull ^ pc1;
		std::scoped_lock lock {g_rp_callers_mutex};
		g_rp_end_callers[key]++;
		g_rp_end_caller_pcs[key] = {pc0, pc1};
	}
}
Scope::Scope(Origin origin): prev(g_readback_origin) {
	g_readback_origin = origin;
}
Scope::~Scope() {
	g_readback_origin = prev;
}
} // namespace ReadbackStats

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		// Copy waits for ring reuse and flushes the fresh host data before submission.
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		const auto destination_offset = buffer.Offset(address);
		EXIT_IF(destination_offset > buffer.Size() || chunk > buffer.Size() - destination_offset);
		m_scheduler.EndRendering();
		const auto command = m_scheduler.Current().Handle();
		vk::BufferMemoryBarrier2 before {};
		before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		before.srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		before.dstStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		before.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer        = buffer.Handle();
		before.offset        = destination_offset;
		before.size          = chunk;
		vk::DependencyInfo dependency {};
		dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers    = &before;
		command.pipelineBarrier2(dependency);
		const vk::BufferCopy copy {offset, destination_offset, chunk};
		command.copyBuffer(m_staging_buffer.Handle(), buffer.Handle(), 1, &copy);
		auto after          = before;
		after.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		after.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		after.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		dependency.pBufferMemoryBarriers = &after;
		command.pipelineBarrier2(dependency);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

void BufferCache::Register(BufferId id) {
	ChangeRegister<true>(id);
}

void BufferCache::Unregister(BufferId id) {
	ChangeRegister<false>(id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	if constexpr (insert) {
		// A new buffer can cover pages that were CPU-dirty before the last full BDA sync but
		// were not owned by any buffer then; force the next PrepareBda to scan.
		g_cpu_dirty_generation.fetch_add(1, std::memory_order_acq_rel);
		QueueBdaDirtyRange(m_slot_buffers[id].CpuAddress(), m_slot_buffers[id].Size());
		BumpBdaSyncEpoch();
	}
	auto& buffer = m_slot_buffers[id];
	PageTable::PageRange pages {};
	EXIT_IF(!(GuestRange {buffer.CpuAddress(), buffer.Size()}.Valid()) ||
	        !PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	const auto table_offset = PageIndex(buffer.CpuAddress()) * sizeof(vk::DeviceAddress);
	if constexpr (insert) {
		const auto [it, inserted] = m_buffers.emplace(buffer.CpuAddress(), id);
		(void)it;
		EXIT_IF(!inserted);
		m_total_used_memory += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		WriteDataBuffer(m_bda_pagetable_buffer, table_offset,
		                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
	} else {
		const auto found = m_buffers.find(buffer.CpuAddress());
		EXIT_IF(found == m_buffers.end() || found->second != id);
		m_buffers.erase(found);
		EXIT_IF(buffer.Size() > m_total_used_memory);
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(table_offset,
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	if (!buffer.is_deleted) {
		m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
	}
}

void BufferCache::DeleteBuffer(BufferId id) {
	if (IsBufferInvalid(id)) {
		return;
	}
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

template <bool async>
bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	if (!m_caller_readbacks.empty() && m_caller_capture == nullptr) {
		// Pages a caller-side readback took are still GPU-dirty in the tracker but no longer in
		// m_gpu_modified_ranges; finish those first so this download sees a consistent state.
		CompleteCallerReadbacks(vaddr, size);
	}
	CallerReadback* const capture = async ? m_caller_capture : nullptr;
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Keep packed ranges on separate cache lines, as in shadPS4.
			    total_size += Common::AlignUp(end - start, 64);
		    });
		    m_gpu_modified_ranges.Subtract(address, bytes);
	    });
	if (copies.empty()) {
		return false;
	}

	uint8_t* mapped = nullptr;
	uint64_t offset = 0;
	if (capture == nullptr) {
		std::tie(mapped, offset) = m_download_buffer.Map(total_size, 64);
	}
	std::unique_ptr<Buffer> temporary;
	if (mapped == nullptr) {
		temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                     vk::BufferUsageFlagBits::eTransferDst, total_size);
		mapped = temporary->Mapped().data();
	} else {
		m_download_buffer.Commit();
	}
	const auto& download = temporary ? *temporary : m_download_buffer;
	for (auto& copy: copies) {
		copy.dstOffset += offset;
	}

	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Handle();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer.Handle();
	before.offset              = 0;
	before.size                = buffer.Size();
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                       nullptr);
	native.copyBuffer(buffer.Handle(), download.Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());

	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = download.Handle();
	after.offset        = offset;
	after.size          = total_size;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	if (capture != nullptr) {
		for (const auto& copy: copies) {
			capture->ranges.emplace_back(buffer_address + copy.srcOffset, copy.size);
		}
	}
	auto publish = [this, mapped, offset, total_size, buffer_address,
	                copies = std::move(copies), owner = std::move(temporary)] {
		(owner ? *owner : m_download_buffer).Invalidate(offset, total_size);
		for (const auto& copy: copies) {
			Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
			                                      mapped + (copy.dstOffset - offset), copy.size);
		}
	};
	if (capture != nullptr) {
		capture->publish = std::move(publish);
	} else if constexpr (async) {
		m_scheduler.DeferPriorityOperation(std::move(publish));
	} else {
		const auto tick       = m_scheduler.CurrentTick();
		const bool stats      = ReadbackStats::Enabled();
		const auto wait_start = std::chrono::steady_clock::now();
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		if (stats) {
			auto& c = g_readback[static_cast<size_t>(g_readback_origin)];
			c.wait_us.fetch_add(MicrosSince(wait_start), std::memory_order_relaxed);
			c.bytes.fetch_add(total_size, std::memory_order_relaxed);
		}
		publish();
	}
	return true;
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 64 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache) {
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	if (!m_graphics.CanReportMemoryUsage()) {
		return;
	}
	constexpr int64_t GiB              = 1024ll * 1024 * 1024;
	constexpr int64_t target_threshold = 8 * GiB;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
	const auto threshold = std::min(budget, target_threshold);
	const auto expected  = std::min(budget - 6 * threshold / 10, budget - GiB);
	const auto critical  = std::min(budget - 2 * threshold / 10, budget - GiB / 2);
	m_trigger_gc_memory  = static_cast<uint64_t>(std::max<int64_t>(expected, GiB));
	m_critical_gc_memory = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * GiB));
}

BufferCache::~BufferCache() {
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
	m_buffers.clear();
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid memory-invalidation range\n");
	}
	m_memory_tracker.InvalidateRegion(vaddr, size, [this, vaddr, size] {
		ReadbackStats::Scope scope(ReadbackStats::Origin::Invalidate);
		ReadMemory(vaddr, size, true);
	});
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto origin     = ReadbackStats::Current();
	const bool stats      = ReadbackStats::Enabled();
	const auto wall_start = stats ? std::chrono::steady_clock::now()
	                              : std::chrono::steady_clock::time_point {};
	if (stats && origin == ReadbackStats::Origin::Fault) {
		std::scoped_lock lock {g_fault_pages_mutex};
		g_fault_pages[vaddr & ~(TRACKER_PAGE_SIZE - 1)]++;
	}
	const bool caller_wait = FaultWaitOnCallerEnabled() && !GuestGpu::IsGpuThread();
	std::shared_ptr<CallerReadback> pending;
	m_scheduler.Context().GetGpu().SendCommandSync([this, vaddr, size, is_write, origin,
	                                                caller_wait, &pending] {
		if (is_write && !IsRegionRegistered(vaddr, size)) {
			return;
		}
		ReadbackStats::Scope scope(origin);
		auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];

		uint64_t start = vaddr;
		uint64_t end   = vaddr + size;
		const bool coalesce = FaultCoalesceEnabled() &&
		                      ((origin == ReadbackStats::Origin::Fault && !is_write) ||
		                       (origin == ReadbackStats::Origin::Invalidate && is_write));
		if (coalesce) {
			// Widen to the contiguous run of GPU-dirty tracker pages inside this buffer. Every
			// page in the run would fault and drain on its own next; downloading them now
			// publishes the same bytes before their read protection is lifted. For a CPU write
			// only the written range becomes CPU-modified; the neighbours become clean (their
			// backing equals the GPU copy), so a later write to them faults without a drain.
			const uint64_t lo = buffer.CpuAddress();
			const uint64_t hi = buffer.CpuAddress() + buffer.Size();
			start             = std::max(lo, vaddr & ~(TRACKER_PAGE_SIZE - 1));
			end               = std::min(hi, Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE));
			while (end - start < FaultCoalesceMaxBytes && end < hi &&
			       m_memory_tracker.IsRegionGpuModified(end, std::min(TRACKER_PAGE_SIZE, hi - end))) {
				end = std::min(hi, end + TRACKER_PAGE_SIZE);
			}
			while (end - start < FaultCoalesceMaxBytes && start > lo &&
			       m_memory_tracker.IsRegionGpuModified(
			           std::max(lo, start - TRACKER_PAGE_SIZE),
			           start - std::max(lo, start - TRACKER_PAGE_SIZE))) {
				start = std::max(lo, start - TRACKER_PAGE_SIZE);
			}
		}
		if (caller_wait) {
			if (!m_caller_readbacks.empty()) {
				CompleteCallerReadbacks(start, end - start);
			}
			auto entry       = std::make_shared<CallerReadback>();
			m_caller_capture = entry.get();
			const bool taken = DownloadBufferMemory<true>(buffer, start, end - start);
			m_caller_capture = nullptr;
			if (taken) {
				// The copy takes whole GPU-dirty tracker pages (their bytes left
				// m_gpu_modified_ranges), so the entry must own the page-aligned extent of
				// everything it took, not just the faulting byte: a later access anywhere in
				// those pages has to finish this readback first.
				uint64_t lo = start & ~(TRACKER_PAGE_SIZE - 1);
				uint64_t hi = Common::AlignUp(end, TRACKER_PAGE_SIZE);
				for (const auto& [address, bytes]: entry->ranges) {
					lo = std::min(lo, address & ~(TRACKER_PAGE_SIZE - 1));
					hi = std::max(hi, Common::AlignUp(address + bytes, TRACKER_PAGE_SIZE));
				}
				entry->start      = lo;
				entry->size       = hi - lo;
				entry->is_write   = is_write;
				entry->write_addr = vaddr;
				entry->write_size = size;
				entry->tick       = m_scheduler.CurrentTick();
				m_scheduler.Flush();
				m_caller_readbacks.push_back(entry);
				pending = std::move(entry);
				return;
			}
		} else if (DownloadBufferMemory<false>(buffer, start, end - start)) {
			m_memory_tracker.UnmarkRegionAsGpuModified(start, end - start);
		}
		if (is_write) {
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		}
	});
	if (pending) {
		// The GPU thread is free to keep recording while this (guest) thread waits.
		const auto wait_start = std::chrono::steady_clock::now();
		m_scheduler.GetMasterSemaphore().Wait(pending->tick);
		if (stats) {
			g_readback[static_cast<size_t>(origin)].wait_us.fetch_add(MicrosSince(wait_start),
			                                                          std::memory_order_relaxed);
		}
		m_scheduler.Context().GetGpu().SendCommandSync(
		    [this, pending] { FinalizeCallerReadback(*pending); });
	}
	if (stats) {
		auto& c = g_readback[static_cast<size_t>(origin)];
		c.count.fetch_add(1, std::memory_order_relaxed);
		c.wall_us.fetch_add(MicrosSince(wall_start), std::memory_order_relaxed);
		MaybePrintReadbackStats();
	}
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return *owner;
		}
	}
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			// Reserve space in the incoming stream's direction of growth.
			// The old buffer extending left of the request predicts growth to the right, and vice versa.
			if (expands_left) {
				end += std::min(StreamLeapSize, (vaddr < LOWER_ADDRESS_SIZE ? LOWER_ADDRESS_SIZE
				                                       : LibKernel::Memory::kExtendedMemoryBase +
				                                             LibKernel::Memory::kExtendedMemorySize) - end);
			}
			if (expands_right) {
				const auto minimum = vaddr < LOWER_ADDRESS_SIZE
				                         ? CACHING_PAGESIZE * 2
				                         : LibKernel::Memory::kExtendedMemoryBase;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	EXIT_IF(m_scheduler.Current().IsInvalid());
	const auto end = Common::AlignUp(vaddr + size, CACHING_PAGESIZE);
	vaddr = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);

	const auto id = m_slot_buffers.insert(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, overlap.end - overlap.begin);
	const auto& buffer = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	vk::Buffer                  source;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    copies.emplace_back(total_size, buffer.Offset(address), bytes);
		    total_size += bytes;
	    },
	    [&]() noexcept { source = UploadCopies(buffer, copies, total_size); });
	if (source) {
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto native = command.Handle();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead |
		                       vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
	}
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size) {
	if (copies.empty()) {
		return nullptr;
	}

	auto [mapped, base_offset] = m_staging_buffer.Map(total_size, 4);
	if (mapped != nullptr) {
		for (auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
			std::memcpy(mapped + copy.srcOffset, reinterpret_cast<const void*>(address), copy.size);
			copy.srcOffset += base_offset;
		}
		m_staging_buffer.Commit();
		return m_staging_buffer.Handle();
	}

	auto temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Upload, 0,
	                                         vk::BufferUsageFlagBits::eTransferSrc, total_size);
	for (const auto& copy: copies) {
		const auto address = buffer.CpuAddress() + copy.dstOffset;
		std::memcpy(temporary->Mapped().data() + copy.srcOffset,
		            reinterpret_cast<const void*>(address), copy.size);
	}
	temporary->Flush(0, total_size);
	const auto handle = temporary->Handle();
	m_scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return handle;
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id) {
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}

	if (!is_written && size <= CACHING_PAGESIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr) {
			std::memcpy(mapped, reinterpret_cast<const void*>(vaddr), size);
			m_stream_buffer.Commit();
			return {&m_stream_buffer, offset};
		}
	}

	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer);
	if (is_written) {
		m_gpu_modified_ranges.Add(vaddr, size);
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid image source\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		return ObtainBuffer(vaddr, size, false, false);
	}

	auto [staging, stage_offset] = m_staging_buffer.Map(size, 16);
	if (staging == nullptr || !Libs::LibKernel::Memory::TryReadSparseBacking(vaddr, staging, size)) {
		EXIT("BufferCache: failed to read mapped guest image backing\n");
	}
	m_staging_buffer.Commit();
	return {&m_staging_buffer, stage_offset};
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
	(void)m_texture_cache.ClearMeta(vaddr);
	if (!IsRegionGpuModified(vaddr, size)) {
		// Access the guest mapping so write faults invalidate cached buffers and images.
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
		std::memcpy(reinterpret_cast<void*>(dst_vaddr), reinterpret_cast<const void*>(src_vaddr),
		            size);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Intersects(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::FinalizeCallerReadback(CallerReadback& readback) {
	if (readback.done) {
		return;
	}
	readback.done = true;
	std::erase_if(m_caller_readbacks, [&](const auto& entry) { return entry.get() == &readback; });
	m_scheduler.Wait(readback.tick);
	m_scheduler.WaitPriorityOperations(readback.tick);
	bool redirtied = false;
	for (const auto& [address, bytes]: readback.ranges) {
		if (m_gpu_modified_ranges.Intersects(address, bytes)) {
			redirtied = true;
			break;
		}
	}
	if (!redirtied) {
		readback.publish();
		m_memory_tracker.UnmarkRegionAsGpuModified(readback.start, readback.size);
		if (readback.is_write) {
			m_memory_tracker.MarkRegionAsCpuModified(readback.write_addr, readback.write_size);
		}
	} else {
		// The GPU wrote some of these bytes again after the copy was recorded: drop this copy and
		// hand the bytes back to the GPU-dirty set, so the retried access downloads fresh data.
		for (const auto& [address, bytes]: readback.ranges) {
			const auto page_lo = address & ~(TRACKER_PAGE_SIZE - 1);
			const auto page_hi = Common::AlignUp(address + bytes, TRACKER_PAGE_SIZE);
			if (m_memory_tracker.IsRegionGpuModified(page_lo, page_hi - page_lo)) {
				m_gpu_modified_ranges.Add(address, bytes);
			}
		}
	}
	readback.publish = {};
}

void BufferCache::CompleteCallerReadbacks(uint64_t vaddr, uint64_t size) {
	const auto end = vaddr + size;
	for (size_t i = 0; i < m_caller_readbacks.size();) {
		auto entry = m_caller_readbacks[i];
		if (entry->start < end && vaddr < entry->start + entry->size) {
			FinalizeCallerReadback(*entry); // erases it from the list
			continue;
		}
		i++;
	}
}

void BufferCache::RunGarbageCollector() {
	const auto tick = m_gc_tick++;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}

	const bool     aggressive = m_total_used_memory >= m_critical_gc_memory;
	const uint64_t age        = std::min<uint64_t>(aggressive ? 80 : 160, tick);
	// KYTY_GC_DRAIN_INTERVAL=<n> (opt-in, default 1): under critical pressure, retiring GPU-dirty
	// buffers needs a full GPU drain (download + wait). With n > 1 that drain runs only on every
	// n-th collection, with a proportionally larger batch (up to 8x), so a 16 GB unified-memory
	// Mac that sits above the critical threshold does not stall the GPU thread every frame.
	// Clean buffers are still retired on every collection.
	static const uint64_t drain_interval = [] {
		const char* value = std::getenv("KYTY_GC_DRAIN_INTERVAL");
		if (value == nullptr) {
			return uint64_t {1};
		}
		const auto n = std::strtoull(value, nullptr, 10);
		return n == 0 ? uint64_t {1} : std::min<uint64_t>(n, 1024);
	}();
	const bool   drain_dirty = aggressive && (drain_interval == 1 || tick % drain_interval == 0);
	const size_t limit       = !aggressive ? 32
	                           : drain_dirty ? 64 * static_cast<size_t>(std::min<uint64_t>(drain_interval, 8))
	                                         : 64;

	std::vector<BufferId> dirty_buffers;
	size_t                retire_count = 0;
	m_lru_cache.ForEachItemBelow(tick - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		EXIT_IF(buffer.is_deleted);
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty && !drain_dirty) {
			return false;
		}
		if (dirty) {
			EXIT_NOT_IMPLEMENTED(!DownloadBufferMemory<true>(buffer, buffer.CpuAddress(), buffer.Size()));
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
		}
		return ++retire_count == limit;
	});
	if (dirty_buffers.empty()) {
		return;
	}

	// Publish all queued downloads before releasing their tracked pages and owners.
	const auto completion_tick = m_scheduler.CurrentTick();
	m_scheduler.Wait(completion_tick);
	m_scheduler.WaitPriorityOperations(completion_tick);
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size()) ||
		    m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: garbage collection retained GPU ownership\n");
		}
		m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto&      buffer = m_slot_buffers[it->second];
		const auto start  = std::max(buffer.CpuAddress(), vaddr);
		const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start < finish) {
			(void)SynchronizeBuffer(buffer, start, finish - start, false, false);
		}
	}
}

} // namespace Libs::Graphics
