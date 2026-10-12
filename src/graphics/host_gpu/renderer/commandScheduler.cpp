#include "graphics/host_gpu/renderer/commandScheduler.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/syncAudit.h"
#include "graphics/host_gpu/renderer/threadQos.h"
#include "graphics/host_gpu/renderer/cache/guestImport.h"
#include "graphics/host_gpu/renderer/gpuThreadStats.h"

static_assert(Libs::Graphics::GpuThreadStats::kDrainSites == static_cast<int>(Libs::Graphics::SyncAudit::Site::Count));

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace Libs::Graphics {

static thread_local CommandScheduler* g_deferred_callback_scheduler = nullptr;

namespace {

void ReportVulkanFatal(const char* what, vk::Result result, uint64_t tick, uint32_t debug_op,
                       uint64_t debug_submit, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                       uint32_t arg3, uint64_t arg4) {
	LOGF("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	     debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::printf("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	            what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	            debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::fflush(stdout);
}

} // namespace

CommandScheduler::CommandPool::CommandPool(GraphicContext& graphics, MasterSemaphore& master)
    : m_graphics(graphics), m_master(master) {
	EXIT_IF(graphics.queue_family == static_cast<uint32_t>(-1));
	vk::CommandPoolCreateInfo create {};
	create.queueFamilyIndex = graphics.queue_family;
	create.flags            = vk::CommandPoolCreateFlagBits::eTransient |
	                          vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	const auto result       = graphics.device.createCommandPool(&create, nullptr, &m_pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_pool == nullptr);
}

CommandScheduler::CommandPool::~CommandPool() {
	for (auto pool: m_own_pools) {
		m_graphics.device.destroyCommandPool(pool, nullptr);
	}
	m_graphics.device.destroyCommandPool(m_pool, nullptr);
}

size_t CommandScheduler::CommandPool::Grow() {
	const auto first = m_ticks.size();
	m_ticks.resize(first + GrowStep);
	m_buffers.resize(first + GrowStep);

	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = m_pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = static_cast<uint32_t>(GrowStep);
	if (SubmitThreadEnabled()) {
		vk::CommandPoolCreateInfo create {};
		create.queueFamilyIndex = m_graphics.queue_family;
		create.flags            = vk::CommandPoolCreateFlagBits::eTransient |
		                          vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
		allocate.commandBufferCount = 1;
		for (size_t index = first; index < first + GrowStep; ++index) {
			EXIT_IF(m_graphics.device.createCommandPool(&create, nullptr, &allocate.commandPool) !=
			        vk::Result::eSuccess);
			m_own_pools.push_back(allocate.commandPool);
			EXIT_IF(m_graphics.device.allocateCommandBuffers(&allocate, &m_buffers[index]) !=
			        vk::Result::eSuccess);
		}
		return first;
	}
	EXIT_IF(m_graphics.device.allocateCommandBuffers(&allocate, m_buffers.data() + first) !=
	        vk::Result::eSuccess);
	return first;
}

vk::CommandBuffer CommandScheduler::CommandPool::Commit() {
	auto       gpu_tick = m_master.KnownGpuTick();
	const auto search   = [this, &gpu_tick](size_t begin, size_t end) -> std::optional<size_t> {
		for (size_t index = begin; index < end; ++index) {
			if (gpu_tick >= m_ticks[index]) {
				m_ticks[index] = m_master.CurrentTick();
				return index;
			}
		}
		return std::nullopt;
	};

	auto found = search(m_hint, m_ticks.size());
	if (!found) {
		m_master.Refresh();
		gpu_tick = m_master.KnownGpuTick();
		found    = search(m_hint, m_ticks.size());
	}
	if (!found) {
		found = search(0, m_hint);
	}
	if (!found) {
		found           = Grow();
		m_ticks[*found] = m_master.CurrentTick();
	}

	m_hint = (*found + 1) % m_ticks.size();
	return m_buffers[*found];
}

bool CommandScheduler::InDeferredOperation() noexcept {
	return g_deferred_callback_scheduler != nullptr;
}

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics)
    : m_master(graphics), m_context(context), m_graphics(graphics),
      m_command_pool(graphics, m_master), m_command(*this),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {}

CommandScheduler::~CommandScheduler() {
	Shutdown();
}

void CommandScheduler::Shutdown() {
	{
		std::unique_lock lock(m_operation_mutex);
		if (m_operation_state == OperationState::Closed) {
			return;
		}
		if (g_deferred_callback_scheduler == this) {
			EXIT_IF(m_operation_state == OperationState::Open);
			// A priority callback cannot join its own runner, while a normal callback can be
			// executing inside the shutdown owner's final PopPendingOperations. The owning
			// thread will finish shutdown after this callback returns.
			return;
		}
		if (m_operation_state == OperationState::Draining) {
			m_operation_available.wait(
			    lock, [this] { return m_operation_state == OperationState::Closed; });
			return;
		}
		m_operation_state = OperationState::Draining;
	}
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	PopPendingOperations();
	DrainPriorityOperations();
	m_priority_thread.request_stop();
	m_operation_available.notify_all();
	if (m_priority_thread.joinable()) {
		m_priority_thread.join();
	}
	StopSubmitThread();
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(!m_pending_operations.empty() || !m_priority_operations.empty() ||
		        m_priority_active);
		m_operation_state = OperationState::Closed;
	}
	m_operation_available.notify_all();
}

void CommandScheduler::Begin(HW::Context& registers, HW::UserConfig& user_config,
                             HW::Shader& shaders) {
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(m_operation_state != OperationState::Open);
	}
	m_command.Bind(registers, user_config, shaders);

	if (m_command.IsInvalid()) {
		BeginNext();
	}
}

void CommandScheduler::BeginRendering(const RenderState& state) {
	Current().BeginRendering(state);
}

void CommandScheduler::EndRendering() {
	if (Active() && !m_command.IsInvalid()) {
		Current().EndRendering();
	}
}

void CommandScheduler::Flush() {
	SubmitInfo submit;
	Flush(submit);
}

void CommandScheduler::Flush(SubmitInfo& submit) {
	Submit(submit);
	BeginNext();
}

void CommandScheduler::FlushAndWait() {
	GpuThreadStats::Scope gts_drain(GpuThreadStats::Cat::Drain, static_cast<int>(SyncAudit::g_site));
	const bool audit = SyncAudit::Enabled();
	const auto start = audit ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
	const auto tick = Submit();
	const bool blocked = audit && !m_master.IsFree(tick);
	m_master.Wait(tick);
	if (audit) {
		SyncAudit::Record(true, blocked, start);
	}
	BeginNext();
}

void CommandScheduler::Finish() {
	GpuThreadStats::Scope gts_drain(GpuThreadStats::Cat::Drain, static_cast<int>(SyncAudit::g_site));
	CheckActive();
	const bool audit = SyncAudit::Enabled();
	const auto start = audit ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
	const bool drain = !m_command.IsInvalid();
	if (drain) {
		Submit();
	}
	const bool blocked = audit && !m_master.IsFree(CurrentTick() - 1);
	m_master.Wait(CurrentTick() - 1);
	if (audit) {
		SyncAudit::Record(drain, blocked, start);
	}
	BeginNext();
	PopPendingOperations();
}

void CommandScheduler::Wait(uint64_t tick) {
	GpuThreadStats::Scope gts_drain(GpuThreadStats::Cat::Drain, static_cast<int>(SyncAudit::g_site));
	EXIT_IF(tick > CurrentTick());
	if (SyncAudit::Enabled()) {
		const auto start   = std::chrono::steady_clock::now();
		const bool drain   = tick == CurrentTick();
		const bool blocked = drain || !m_master.IsFree(tick);
		WaitImpl(tick);
		SyncAudit::Record(drain, blocked, start);
		return;
	}
	WaitImpl(tick);
}

void CommandScheduler::WaitImpl(uint64_t tick) {
	if (tick == CurrentTick()) {
		CheckActive();
		// A stream-buffer wrap can wait while a draw is being prepared through a reference to
		// Current(). The wrapper stays stable while its pooled Vulkan buffer is retired. Deferred
		// resources are released only at the next GPU operation boundary.
		const auto submitted_tick = Submit();
		EXIT_IF(submitted_tick != tick);
		m_master.Wait(tick);
		BeginNext();
	} else {
		m_master.Wait(tick);
	}
}

void CommandScheduler::PopPendingOperations() {
	m_master.Refresh();
	for (;;) {
		PendingOperation operation;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty() ||
			    !m_master.IsFree(m_pending_operations.front().tick)) {
				return;
			}
			operation = std::move(m_pending_operations.front());
			m_pending_operations.pop();
		}
		if (SyncAudit::Enabled() && SyncAudit::g_site == SyncAudit::Site::Other) {
			SyncAudit::Scope audit_scope(SyncAudit::Site::Pending);
			WaitPriorityOperations(operation.tick);
		} else {
			WaitPriorityOperations(operation.tick);
		}
		RunOperation(std::move(operation.callback));
	}
}

void CommandScheduler::DeferOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), false);
}

void CommandScheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), true);
}

void CommandScheduler::QueueOperation(Common::UniqueFunction<void>&& operation, bool priority) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		auto& queue = priority ? m_priority_operations : m_pending_operations;
		queue.push({std::move(operation), CurrentTick()});
		lock.unlock();
		if (priority) {
			m_operation_available.notify_one();
		}
		return;
	}
	if (g_deferred_callback_scheduler != this) {
		m_operation_available.wait(lock,
		                           [this] { return m_operation_state == OperationState::Closed; });
	}
	lock.unlock();
	operation();
}

bool CommandScheduler::AcceptsDeferredOperations() {
	std::lock_guard lock(m_operation_mutex);
	return m_operation_state == OperationState::Open;
}

bool CommandScheduler::HasPendingPriorityOperations() {
	std::lock_guard lock(m_operation_mutex);
	return !m_priority_operations.empty() || m_priority_active;
}

void CommandScheduler::PriorityOperationsThread(std::stop_token stop) {
	ApplyCriticalThreadQos("gpu-priority-ops");
	while (!stop.stop_requested()) {
		PendingOperation operation;
		{
			std::unique_lock lock(m_operation_mutex);
			m_operation_available.wait(lock, [this, &stop] {
				return stop.stop_requested() || !m_priority_operations.empty();
			});
			if (stop.stop_requested()) {
				return;
			}
			operation = std::move(m_priority_operations.front());
			m_priority_operations.pop();
			m_priority_active      = true;
			m_priority_active_tick = operation.tick;
		}
		m_master.Wait(operation.tick);
		if (!stop.stop_requested()) {
			RunOperation(std::move(operation.callback));
		}
		{
			std::lock_guard lock(m_operation_mutex);
			m_priority_active      = false;
			m_priority_active_tick = 0;
		}
		m_operation_available.notify_all();
	}
}

void CommandScheduler::DrainPriorityOperations() {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(
	    lock, [this] { return m_priority_operations.empty() && !m_priority_active; });
}

void CommandScheduler::WaitPriorityOperations(uint64_t tick) {
	GpuThreadStats::Scope gts_drain(GpuThreadStats::Cat::Drain, static_cast<int>(SyncAudit::g_site));
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	const auto ready = [this, tick] {
		const bool active_before_or_at = m_priority_active && m_priority_active_tick <= tick;
		const bool queued_before_or_at =
		    !m_priority_operations.empty() && m_priority_operations.front().tick <= tick;
		return !active_before_or_at && !queued_before_or_at;
	};
	if (SyncAudit::Enabled() && !ready()) {
		// Only waits that actually sleep are recorded (blocked, never a drain).
		const auto start = std::chrono::steady_clock::now();
		m_operation_available.wait(lock, ready);
		lock.unlock();
		SyncAudit::Record(false, true, start);
		return;
	}
	m_operation_available.wait(lock, ready);
}

void CommandScheduler::RunOperation(Common::UniqueFunction<void>&& operation) {
	auto* previous                = g_deferred_callback_scheduler;
	g_deferred_callback_scheduler = this;
	operation();
	g_deferred_callback_scheduler = previous;
}

bool CommandScheduler::IsFree(uint64_t tick) {
	if (m_master.IsFree(tick)) {
		return true;
	}
	m_master.Refresh();
	return m_master.IsFree(tick);
}

void CommandScheduler::CheckActive() const {
	EXIT_IF(!Active());
}

CommandBuffer& CommandScheduler::Current() {
	CheckActive();
	return m_command;
}

CommandBuffer& CommandScheduler::BeginCommand() {
	EXIT_IF(!m_command.IsInvalid());
	m_command.m_buffer = m_command_pool.Commit();
	m_command.Begin();
	return m_command;
}

uint64_t CommandScheduler::Submit(SubmitInfo submit) {
	GpuThreadStats::Scope gts_submit(GpuThreadStats::Cat::Submit);
	EXIT_IF(m_command.IsInvalid());
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);
	// KYTY_GUEST_IMPORT: GPU writes to imported guest memory become host-visible at this
	// submission's signal (Vulkan needs an explicit availability operation to the host domain).
	if (GuestImport::g_submit_hook != nullptr) {
		// Uses noted for a packet that is not fully recorded yet move to the next submission.
		GuestImport::g_submit_hook(GuestImport::g_submit_hook_context, CurrentTick() + 1);
	}
	if (GuestImport::g_host_barrier_pending.exchange(false, std::memory_order_relaxed)) {
		m_command.EndRendering();
		vk::MemoryBarrier host_barrier {};
		host_barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
		host_barrier.dstAccessMask = vk::AccessFlagBits::eHostRead | vk::AccessFlagBits::eHostWrite;
		m_command.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                                   vk::PipelineStageFlagBits::eHost, {}, 1, &host_barrier,
		                                   0, nullptr, 0, nullptr);
	}
	if (SubmitThreadEnabled()) {
		// Binary semaphores (swapchain) keep the inline path: a present must never wait on a
		// semaphore whose signal has not been submitted yet. Inline submits drain the FIFO first.
		if (submit.num_wait_semaphores == 0 && submit.num_signal_semaphores == 0) {
			return SubmitAsync(submit);
		}
		DrainSubmitQueue();
	}

	m_command.End();
	const auto buffer   = m_command.m_buffer;
	auto&      graphics = m_graphics;
	EXIT_IF(graphics.queue == nullptr);

	vk::Result result;
	const bool timing = ([] { static const bool kyty_env_set = std::getenv("KYTY_GPU_SUBMIT_TIMING") != nullptr; return kyty_env_set; }());
	const auto submit_start = timing ? std::chrono::steady_clock::now()
	                                : std::chrono::steady_clock::time_point {};
	uint64_t   tick;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		tick = m_master.NextTick();
		submit.AddSignal(m_master.Handle(), tick);

		vk::TimelineSemaphoreSubmitInfo timeline_info {};
		timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
		timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
		timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
		timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

		vk::SubmitInfo submit_info {};
		submit_info.pNext                = &timeline_info;
		submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
		submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
		submit_info.pWaitDstStageMask    = submit.wait_stages.data();
		submit_info.commandBufferCount   = 1;
		submit_info.pCommandBuffers      = &buffer;
		submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
		submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

		result = graphics.queue.submit(1, &submit_info, nullptr);
	}

	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkQueueSubmit", result, tick, m_command.m_debug_op,
		                  m_command.m_debug_submit_id, m_command.m_debug_arg0,
		                  m_command.m_debug_arg1, m_command.m_debug_arg2, m_command.m_debug_arg3,
		                  m_command.m_debug_arg4);
	}
	if (timing) {
		const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
		                            std::chrono::steady_clock::now() - submit_start)
		                            .count();
		LOGF("GpuSubmitTiming: tick=%" PRIu64 " us=%" PRId64 " debug_op=%u debug_submit=%" PRIu64
		     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		     tick, static_cast<int64_t>(elapsed_us), m_command.m_debug_op,
		     m_command.m_debug_submit_id, m_command.m_debug_arg0, m_command.m_debug_arg1,
		     m_command.m_debug_arg2, m_command.m_debug_arg3, m_command.m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	m_command.m_buffer  = nullptr;
	m_work_since_submit = 0;
	return tick;
}

void CommandScheduler::BeginNext() {
	CheckActive();
	BeginCommand();
}

bool CommandScheduler::SubmitThreadEnabled() noexcept {
	static const bool enabled = [] {
		const char* v  = std::getenv("KYTY_SUBMIT_THREAD");
		const auto  is = [v](const char* s) {
			for (size_t i = 0;; ++i) {
				if (v[i] != s[i]) {
					return false;
				}
				if (v[i] == '\0') {
					return true;
				}
			}
		};
		const bool on = v != nullptr && (is("1") || is("true") || is("2"));
		if (on) {
			LOGF("SubmitThread: enabled (%s off the recording thread)\n",
			     is("2") ? "vkQueueSubmit" : "vkEndCommandBuffer + vkQueueSubmit");
		}
		return on;
	}();
	return enabled;
}

// KYTY_SUBMIT_THREAD=2: vkEndCommandBuffer stays on the recording thread and only vkQueueSubmit
// moves to the FIFO thread. Mode 1's end() on another thread races with the recording thread's
// MoltenVK object/descriptor mutations (the 0x20 crash at ~33 s); with PREFILL=0 MoltenVK encodes
// at vkQueueSubmit, so mode 2 still moves the encoding off the recording thread.
bool CommandScheduler::SubmitThreadEndsOnRecorder() noexcept {
	static const bool ends = [] {
		const char* v = std::getenv("KYTY_SUBMIT_THREAD");
		return v != nullptr && std::strcmp(v, "2") == 0;
	}();
	return ends;
}

// Recording thread. The tick is taken here, synchronously, so CurrentTick(), DeferOperation and
// Wait(tick) keep their meaning; the FIFO thread submits in tick order, so the timeline values are
// signalled in increasing submission order. Host waits on a not-yet-submitted timeline value are
// legal (wait-before-signal) and already happen today on the priority thread.
uint64_t CommandScheduler::SubmitAsync(SubmitInfo& submit) {
	const auto start = std::chrono::steady_clock::now();
	SubmitJob job;
	if (SubmitThreadEndsOnRecorder()) {
		m_command.End(); // EndRendering + vkEndCommandBuffer
		job.ended = true;
	} else {
		m_command.EndRendering(); // a recorded command: stays on the recording thread
	}
	job.buffer       = m_command.m_buffer;
	job.submit       = submit;
	job.debug_op     = m_command.m_debug_op;
	job.debug_submit = m_command.m_debug_submit_id;
	job.debug_arg[0] = m_command.m_debug_arg0;
	job.debug_arg[1] = m_command.m_debug_arg1;
	job.debug_arg[2] = m_command.m_debug_arg2;
	job.debug_arg[3] = m_command.m_debug_arg3;
	job.debug_arg4   = m_command.m_debug_arg4;
	uint64_t tick = 0;
	{
		std::lock_guard lock(m_submit_mutex);
		tick     = m_master.NextTick();
		job.tick = tick;
		if (!m_submit_thread.joinable()) {
			m_submit_thread = std::jthread([this](std::stop_token stop) { SubmitThread(stop); });
		}
		m_submit_jobs.push(std::move(job));
		m_submit_stats[0]++;
		m_submit_stats[1] = std::max<uint64_t>(m_submit_stats[1], m_submit_jobs.size());
		m_submit_stats[2] += static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
		                                                         start)
		        .count());
	}
	m_submit_cv.notify_all(); // the cv is shared with DrainSubmitQueue waiters
	m_command.m_buffer  = nullptr;
	m_work_since_submit = 0; // KYTY_FLUSH_DRAWS budget restarts per submission, as in Submit()
	return tick;
}

void CommandScheduler::DrainSubmitQueue() {
	std::unique_lock lock(m_submit_mutex);
	m_submit_cv.wait(lock, [this] { return m_submit_jobs.empty() && !m_submit_busy; });
}

void CommandScheduler::SubmitThread(std::stop_token stop) {
	auto last_log = std::chrono::steady_clock::now();
	std::unique_lock lock(m_submit_mutex);
	for (;;) {
		m_submit_cv.wait(lock, [this, &stop] { return stop.stop_requested() || !m_submit_jobs.empty(); });
		if (m_submit_jobs.empty()) {
			return; // stop requested and nothing left: jobs are never dropped
		}
		auto job = std::move(m_submit_jobs.front());
		m_submit_jobs.pop();
		m_submit_busy = true;
		lock.unlock();
		const auto start = std::chrono::steady_clock::now();
		ExecuteSubmitJob(job);
		const auto now = std::chrono::steady_clock::now();
		lock.lock();
		m_submit_busy = false;
		m_submit_stats[3] += static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count());
		if (now - last_log >= std::chrono::seconds(2)) {
			LOGF("SubmitThreadStats: jobs=%" PRIu64 " max_depth=%" PRIu64
			     " enqueue_us=%" PRIu64 " thread_us=%" PRIu64 "\n",
			     m_submit_stats[0], m_submit_stats[1], m_submit_stats[2] / 1000,
			     m_submit_stats[3] / 1000);
			m_submit_stats[0] = m_submit_stats[1] = m_submit_stats[2] = m_submit_stats[3] = 0;
			last_log                                                                     = now;
		}
		m_submit_cv.notify_all(); // DrainSubmitQueue waiters
	}
}

void CommandScheduler::ExecuteSubmitJob(SubmitJob& job) {
	auto&      graphics = m_graphics;
	vk::Result result   = job.ended ? vk::Result::eSuccess : job.buffer.end();
	if (result == vk::Result::eSuccess) {
		Common::LockGuard lock(graphics.queue_mutex);
		job.submit.AddSignal(m_master.Handle(), job.tick);

		vk::TimelineSemaphoreSubmitInfo timeline_info {};
		timeline_info.signalSemaphoreValueCount = job.submit.num_signal_semaphores;
		timeline_info.pSignalSemaphoreValues    = job.submit.signal_ticks.data();

		vk::SubmitInfo submit_info {};
		submit_info.pNext                = &timeline_info;
		submit_info.commandBufferCount   = 1;
		submit_info.pCommandBuffers      = &job.buffer;
		submit_info.signalSemaphoreCount = job.submit.num_signal_semaphores;
		submit_info.pSignalSemaphores    = job.submit.signal_semaphores.data();

		result = graphics.queue.submit(1, &submit_info, nullptr);
	}
	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkEndCommandBuffer/vkQueueSubmit (submit thread)", result, job.tick,
		                  job.debug_op, job.debug_submit, job.debug_arg[0], job.debug_arg[1],
		                  job.debug_arg[2], job.debug_arg[3], job.debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandScheduler::StopSubmitThread() {
	if (!m_submit_thread.joinable()) {
		return;
	}
	m_submit_thread.request_stop();
	{
		std::lock_guard lock(m_submit_mutex); // no lost wake-up between predicate and wait
	}
	m_submit_cv.notify_all();
	m_submit_thread.join();
}

} // namespace Libs::Graphics
