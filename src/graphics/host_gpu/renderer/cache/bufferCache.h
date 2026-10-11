#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/guestImport.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/cache/writeTickMap.h"

#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class TextureCache;

using BufferId = Common::SlotId;
inline constexpr BufferId NULL_BUFFER_ID {0};

// KYTY_READBACK_STATS=1: per-origin counters for synchronous GPU->CPU buffer readbacks
// (count, bytes, caller-side wall time, GPU wait), printed every 2 s. Callers tag the origin
// with a Scope; untagged readbacks count as "other".
namespace ReadbackStats {
enum class Origin : int { Other = 0, Fault, ClearMeta, IndirectArgs, Invalidate, Count };
bool   Enabled();
Origin Current();
void   NoteCpuFill(uint64_t bytes);
// Counters printed on the same ReadbackStats line: CPU write faults (all, plus the hottest
// pages) and render-pass churn (real begins, begins that replaced an open pass with a different
// state, explicit ends of an open pass).
void   NoteWriteFault(uint64_t vaddr);
void   NoteRenderPass(bool begin, bool replaced_open_pass, const void* caller0 = nullptr,
                      const void* caller1 = nullptr);
struct Scope {
	explicit Scope(Origin origin);
	~Scope();
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;
	Origin prev;
};
} // namespace ReadbackStats

class BufferCache {
public:
	static constexpr uint32_t CACHING_PAGEBITS  = 14;
	static constexpr uint64_t CACHING_PAGESIZE  = uint64_t {1} << CACHING_PAGEBITS;
	static constexpr uint64_t CACHING_NUMPAGES  = (LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemorySize) >> CACHING_PAGEBITS;
	static constexpr uint64_t BDA_PAGETABLE_SIZE =
	    CACHING_NUMPAGES * sizeof(vk::DeviceAddress);

	static constexpr uint64_t PageIndex(uint64_t address) {
		return (address < LOWER_ADDRESS_SIZE
		            ? address
		            : address - LibKernel::Memory::kExtendedMemoryBase + LOWER_ADDRESS_SIZE) >>
		       CACHING_PAGEBITS;
	}
	static constexpr uint64_t GuestAddress(uint64_t offset) {
		return offset < LOWER_ADDRESS_SIZE
		           ? offset
		           : offset - LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemoryBase;
	}

	BufferCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	            TextureCache& texture_cache);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	void                   InvalidateMemory(uint64_t vaddr, uint64_t size);
	void                   ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false);
	[[nodiscard]] Buffer&  GetBuffer(BufferId id) { return m_slot_buffers[id]; }
	// import_class only matters with KYTY_GUEST_IMPORT=1, when it decides whether a buffer that
	// this call creates may be imported (KYTY_GUEST_IMPORT_CLASSES).
	[[nodiscard]] BufferId FindBuffer(uint64_t vaddr, uint64_t size,
	                                  GuestImport::Class import_class = GuestImport::Class::Other);
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBuffer(
	    uint64_t vaddr, uint64_t size, bool is_written, bool is_texel_buffer = false,
	    BufferId id = {}, GuestImport::Class import_class = GuestImport::Class::Other);
	[[nodiscard]] StreamBuffer&                GetUtilityBuffer(MemoryUsage usage) noexcept {
		switch (usage) {
			case MemoryUsage::Upload: return m_staging_buffer;
			case MemoryUsage::Stream: return m_stream_buffer;
			case MemoryUsage::Download: return m_download_buffer;
			case MemoryUsage::DeviceLocal: return m_device_buffer;
		}
		EXIT("BufferCache: invalid utility-buffer usage\n");
	}
	[[nodiscard]] const Buffer* GetGdsBuffer() const noexcept { return &m_gds_buffer; }
	[[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept { return &m_bda_pagetable_buffer; }
	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return m_fault_manager.GetFaultBuffer(); }
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferForImage(uint64_t vaddr, uint64_t size);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	void               SettleCallerReadbacks(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	void               ProcessFaultBuffer();
	void               SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size);
	void               RunGarbageCollector();
	// KYTY_GUEST_IMPORT: guest unmap of a range (GPU thread, after its users have finished).
	void RetireImportsInRange(uint64_t vaddr, uint64_t size);
	// KYTY_GUEST_IMPORT: a shader that can read any registered buffer through BDA is recorded.
	void NoteBdaUse() noexcept;
	// KYTY_GUEST_IMPORT: the current PM4 packet is fully recorded (see m_import_pending).
	void CommitImportUses() noexcept {
		m_import_pending.clear();
		m_import_bda_pending = false;
	}

	// KYTY_READBACK_PRODUCER_TICK=1: a synchronous readback whose GPU-dirty bytes were all written
	// by an already-submitted command buffer copies them in a side command buffer and waits only
	// for that, instead of splitting the open render pass and draining the open command buffer.
	// KYTY_READBACK_PRODUCER_TICK_STATS=1 keeps the bookkeeping and the counters but always takes
	// the legacy drain (measurement only). Both are off by default.
	[[nodiscard]] static bool ProducerTickTracking();
	[[nodiscard]] static bool ProducerTickEnabled();
	// Called at a PM4 packet boundary: every GPU command for writes obtained so far is recorded.
	void CommitPendingWrites() {
		if (!m_pending_writes.empty()) {
			CommitPendingWritesSlow();
		}
	}

private:
	friend struct BufferCacheTestAccess;

	bool IsBufferInvalid(BufferId id) const {
		const auto* buffer = m_slot_buffers.try_get(id);
		return buffer == nullptr || buffer->is_deleted;
	}

	using BufferMap = std::map<uint64_t, BufferId>;
	struct OverlapResult {
		BufferMap::iterator first;
		BufferMap::iterator last;
		uint64_t            begin;
		uint64_t            end;
		bool                has_stream_leap;
	};

	using PageTable = MultiLevelPageTable<BufferId, CACHING_PAGEBITS, 44, 20>;
	static_assert(CACHING_PAGESIZE == (uint64_t {1} << PageTable::kPageBits));
	void WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source, uint64_t size);
	void TouchBuffer(const Buffer& buffer);
	[[nodiscard]] OverlapResult ResolveOverlaps(uint64_t vaddr, uint64_t size);
	void GrowJoinedBuffer(OverlapResult& overlap, uint64_t request_begin, uint64_t request_end,
	                      bool stats);
	void JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score);
	[[nodiscard]] BufferId CreateBuffer(uint64_t vaddr, uint64_t size,
	                                    GuestImport::Class import_class = GuestImport::Class::Other);
	// KYTY_GUEST_IMPORT helpers.
	[[nodiscard]] bool     TryPrepareImport(uint64_t vaddr, uint64_t size, GuestImport::Class import_class,
	                                        Buffer::ImportedMemory& imported);
	void                   NoteImportUse(Buffer& buffer, uint64_t vaddr, uint64_t size, bool write);
	[[nodiscard]] uint64_t ImportPendingUse(uint64_t vaddr, uint64_t size) const;
	[[nodiscard]] bool     ImportHasPendingUse(uint64_t vaddr, uint64_t size);
	void                   ImportWaitTick(uint64_t tick);
	void                   ImportBeforeCpuWrite(uint64_t vaddr, uint64_t size);
	void                   ProcessImportUnmarks();
	void                   ProcessImportDemotions();
	void                   RetireImport(BufferId id);
	void                   Register(BufferId id);
	void Unregister(BufferId id);
	template <bool insert>
	void ChangeRegister(BufferId id);
	void DeleteBuffer(BufferId id);
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer);
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);
	void CommitPendingWritesSlow();
	// A GPU command writing cache-buffer bytes [vaddr, vaddr+size) has just been recorded.
	void NoteGpuWriteRecorded(uint64_t vaddr, uint64_t size);

	// KYTY_FAULT_WAIT_ON_CALLER=1: a guest thread's fault readback is recorded and submitted by
	// the GPU thread, but the faulting thread waits for the GPU itself; the GPU thread then only
	// publishes the bytes and updates page state (FinalizeCallerReadback) at a command boundary.
	struct CallerReadback {
		uint64_t                                    tick       = 0;
		uint64_t                                    start      = 0;
		uint64_t                                    size       = 0;
		uint64_t                                    write_addr = 0;
		uint64_t                                    write_size = 0;
		bool                                        is_write   = false;
		bool                                        done       = false;
		std::vector<std::pair<uint64_t, uint64_t>> ranges; // GPU-dirty bytes taken (addr, size)
		Common::UniqueFunction<void>                publish;
	};
	void FinalizeCallerReadback(CallerReadback& readback);
	void CompleteCallerReadbacks(uint64_t vaddr, uint64_t size);
	std::vector<std::shared_ptr<CallerReadback>> m_caller_readbacks;
	CallerReadback*                              m_caller_capture = nullptr;

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	MemoryTracker                                     m_memory_tracker;
	StreamBuffer                                      m_staging_buffer;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;
	WriteTickMap                                      m_write_ticks;
	std::vector<std::pair<uint64_t, uint64_t>>        m_pending_writes;
	std::unique_ptr<CommandScheduler>                 m_readback_scheduler;
	// KYTY_GUEST_IMPORT state. The registry is read by CPU fault handlers on guest threads; the
	// rest is GPU-thread only, except the demotion requests (guest threads push, GPU thread pops).
	struct ImportUnmark {
		BufferId id;
		uint64_t serial;
		uint64_t tick;
	};
	GuestImport::Registry     m_import_registry;
	RangeSet                  m_import_demoted;
	std::vector<ImportUnmark> m_import_unmarks;
	// Uses noted while the current PM4 packet is being prepared. Its commands may land in a later
	// submission when something submits mid-packet (a drain, a ring wrap); every Submit moves these
	// uses to the next tick until the packet boundary commits them.
	struct PendingImportUse {
		std::shared_ptr<GuestImport::HazardTicks> ticks;
		uint64_t                                  vaddr;
		uint64_t                                  size;
		bool                                      write;
		BufferId                                  id;
	};
	std::vector<PendingImportUse> m_import_pending;
	bool                          m_import_bda_pending  = false; // g_bda_use_tick noted this packet
	bool                          m_import_sync_from_bda = false;
	void                          BumpPendingImportUses(uint64_t next_tick);
	std::vector<std::shared_ptr<GuestImport::HazardTicks>> m_import_graveyard;
	std::mutex                m_import_demote_mutex;
	std::vector<uint64_t>     m_import_demote_requests;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
