#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include "common/emulatorConfig.h"

#include <chrono>
#include <cstdio>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

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

namespace {

// KYTY_SEM_SPIN statistics, printed every 2 s while the flag is on.
struct SemSpinCounters {
	std::atomic<uint64_t> waits {0};      // Wait() calls that found the tick not yet signaled
	std::atomic<uint64_t> spin_hits {0};  // ...that the spin satisfied (no vkWaitSemaphores)
	std::atomic<uint64_t> spin_us {0};    // total microseconds spent spinning
	std::atomic<uint64_t> blocked_us {0}; // total microseconds inside vkWaitSemaphores
	std::atomic<int64_t>  last_print_us {0};
};
SemSpinCounters g_sem_spin;

int64_t SteadyMicros() {
	return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	                                std::chrono::steady_clock::now().time_since_epoch())
	                                .count());
}

void CpuRelax() {
#if defined(__x86_64__) || defined(_M_X64)
	_mm_pause();
#elif defined(__aarch64__)
	__asm__ __volatile__("yield");
#endif
}

void MaybePrintSemSpin(int64_t now_us) {
	auto last = g_sem_spin.last_print_us.load(std::memory_order_relaxed);
	if (last == 0) {
		g_sem_spin.last_print_us.compare_exchange_strong(last, now_us);
		return;
	}
	if (now_us - last < 2000000 ||
	    !g_sem_spin.last_print_us.compare_exchange_strong(last, now_us)) {
		return;
	}
	const auto waits   = g_sem_spin.waits.exchange(0);
	const auto hits    = g_sem_spin.spin_hits.exchange(0);
	const auto spin_us = g_sem_spin.spin_us.exchange(0);
	const auto blk_us  = g_sem_spin.blocked_us.exchange(0);
	LOGF("SemSpin: dt_ms=%" PRId64 " waits=%" PRIu64 " spin_hits=%" PRIu64 " blocked=%" PRIu64
	     " spin_us=%" PRIu64 " blocked_us=%" PRIu64 " budget_us=%u\n",
	     (now_us - last) / 1000, waits, hits, waits - hits, spin_us, blk_us,
	     Config::SemSpinMicros());
}

} // namespace

void MasterSemaphore::Wait(uint64_t tick) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	// KYTY_SEM_SPIN=1: poll the counter (a plain MTLSharedEvent.signaledValue read on MoltenVK)
	// for a bounded time before the blocking wait. Only the waiting thread spins; the GPU
	// timeline is untouched, and the return condition (counter >= tick) is the same as below.
	const bool spin    = Config::SemSpinEnabled();
	int64_t    t_block = 0;
	if (spin) {
		g_sem_spin.waits.fetch_add(1, std::memory_order_relaxed);
		const int64_t start    = SteadyMicros();
		const int64_t deadline = start + static_cast<int64_t>(Config::SemSpinMicros());
		int64_t       now      = start;
		for (;;) {
			for (int i = 0; i < 16; i++) {
				CpuRelax();
			}
			Refresh();
			now = SteadyMicros();
			if (IsFree(tick)) {
				g_sem_spin.spin_hits.fetch_add(1, std::memory_order_relaxed);
				g_sem_spin.spin_us.fetch_add(static_cast<uint64_t>(now - start),
				                             std::memory_order_relaxed);
				MaybePrintSemSpin(now);
				return;
			}
			if (now >= deadline) {
				break;
			}
		}
		g_sem_spin.spin_us.fetch_add(static_cast<uint64_t>(now - start), std::memory_order_relaxed);
		t_block = now;
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
	if (spin) {
		const int64_t now = SteadyMicros();
		g_sem_spin.blocked_us.fetch_add(static_cast<uint64_t>(now - t_block),
		                                std::memory_order_relaxed);
		MaybePrintSemSpin(now);
	}
}

} // namespace Libs::Graphics
