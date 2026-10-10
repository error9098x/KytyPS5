#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_

#include "common/common.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <memory>
#include <utility>
#include <vector>

namespace Libs::Graphics {

enum class PageFaultAccess { Read, Write, Execute, Unknown };

class PageManager final {
public:
	PageManager();
	// The owner must stop all PageManager callers before destruction.
	~PageManager();

	KYTY_CLASS_NO_COPY(PageManager);

	[[nodiscard]] uint64_t GetPageSize() const;
	// Number of consecutive tracked (read- or access-protected) pages starting at `vaddr`,
	// stopping at the first unprotected page or after `max_pages`.
	[[nodiscard]] uint64_t CountProtectedPagesFrom(uint64_t vaddr, uint64_t max_pages) const;
	// Tracked (protected) pages inside [vaddr, vaddr + window_pages * page size), merged into
	// {address, page count} runs. At most `max_runs` runs are returned.
	[[nodiscard]] std::vector<std::pair<uint64_t, uint64_t>>
	ProtectedRunsFrom(uint64_t vaddr, uint64_t window_pages, size_t max_runs) const;

	template <bool track>
	void UpdatePageWatchers(uint64_t vaddr, uint64_t size);
	template <bool track, bool is_read = false>
	void UpdatePageWatchersForRegion(uint64_t base_addr, RegionBits& mask);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
