#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <cstdio>

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

// Device loss (typically Metal residency failure / out of memory on unified-memory Macs) is
// unrecoverable. Report it clearly with the memory state instead of EXIT_NOT_IMPLEMENTED, and stop
// before more garbage frames are presented from a dead device.
static void FatalGpuResult(GraphicContext& graphics, vk::Result result, const char* where) {
	const bool lost = result == vk::Result::eErrorDeviceLost;
	const bool oom  = result == vk::Result::eErrorOutOfDeviceMemory ||
	                 result == vk::Result::eErrorOutOfHostMemory;
	std::fprintf(stderr, "GPU fatal: %s returned %s%s\n", where, vk::to_string(result).c_str(),
	             lost ? " (device lost; on macOS usually GPU memory exhaustion)" : (oom ? " (out of memory)" : ""));
	std::fprintf(stderr, "GPU fatal: device memory usage %" PRIu64 " MiB, cache budget %" PRIu64 " MiB\n",
	             graphics.GetDeviceMemoryUsage() >> 20u, graphics.GetTotalMemoryBudget() >> 20u);
	std::fflush(stderr);
	graphics.LogMemoryBudget();
	EXIT("GPU device lost or out of memory (%s in %s); see the memory lines above\n",
	     vk::to_string(result).c_str(), where);
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	if (result != vk::Result::eSuccess) {
		FatalGpuResult(m_graphics, result, "getSemaphoreCounterValue");
	}

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	const auto result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	if (result != vk::Result::eSuccess) {
		FatalGpuResult(m_graphics, result, "waitSemaphores");
	}
	Refresh();
}

} // namespace Libs::Graphics
