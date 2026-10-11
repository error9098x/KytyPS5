#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/meshBatchPolicy.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/stateCache.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

struct GraphicContext;
struct ShaderBufferResource;
struct ShaderComputeInputInfo;
struct RenderDepthInfo;
struct RenderColorInfo;
struct DrawCallInfo;
struct DrawEmitInfo;
struct DrawIndexBufferSource;
struct DrawRenderState;
class RenderContext;
class CommandScheduler;
struct RenderExecutorTestAccess;

enum class CommandBufferDebugOp : uint32_t {
	DispatchDirect,
	DrawIndex,
	DrawIndexAuto,
	EopWrite,
	EopInterrupt,
	EopWriteBack,
	EopFlip,
	EopWriteBackFlip,
	EopOnlyFlip,
	DispatchIndirect,
	Unknown,
};

enum class DrawOffsetSource : uint8_t {
	DrawState,
	IndirectArgs,
};

struct DrawIndexArgs {
	uint32_t         index_count                = 0;
	const void*      index_addr                 = nullptr;
	uint32_t         instance_count             = 0;
	uint32_t         index_type_and_size        = 0;
	int32_t          base_vertex                = 0;
	uint32_t         first_instance             = 0;
	DrawOffsetSource offset_source              = DrawOffsetSource::DrawState;
	uint32_t         render_target_slice_offset = 0;
};

struct DrawAutoArgs {
	uint32_t         vertex_count               = 0;
	uint32_t         instance_count             = 0;
	uint32_t         first_vertex               = 0;
	uint32_t         first_instance             = 0;
	DrawOffsetSource offset_source              = DrawOffsetSource::DrawState;
	uint32_t         render_target_slice_offset = 0;
};

// KYTY_GPU_INDIRECT=1: a single guest indirect draw executed from GPU-resident arguments.
struct GpuIndirectDraw {
	static constexpr uint32_t RoleCount = 3; // vertex offset, instance offset, index offset

	uint64_t                         args_addr                  = 0;
	bool                             indexed                    = false;
	uint32_t                         index_type_and_size        = 0;
	uint64_t                         index_base_addr            = 0;
	uint32_t                         index_buffer_size          = 0;
	std::array<int32_t, RoleCount>   patch_slots                = {-1, -1, -1}; // GS user-data slot
	uint32_t                         render_target_slice_offset = 0;
};

// Where the GPU wrote a converted draw's raw arguments, for a later lazy CPU read.
struct GpuIndirectHistoryRef {
	uint64_t tick     = 0;
	uint32_t index    = 0;
	uint32_t sequence = 0;
};

struct GpuIndirectHistoryValues {
	uint32_t count          = 0;
	uint32_t instances      = 0;
	uint32_t vertex_offset  = 0; // base vertex (indexed) or start vertex
	uint32_t start_instance = 0;
	uint32_t start_index    = 0;
};

struct SubmitInfo {
	static constexpr uint32_t MaxSemaphores = 3;

	std::array<vk::Semaphore, MaxSemaphores>          wait_semaphores {};
	std::array<uint64_t, MaxSemaphores>               wait_ticks {};
	std::array<vk::PipelineStageFlags, MaxSemaphores> wait_stages {};
	std::array<vk::Semaphore, MaxSemaphores>          signal_semaphores {};
	std::array<uint64_t, MaxSemaphores>               signal_ticks {};
	uint32_t                                          num_wait_semaphores   = 0;
	uint32_t                                          num_signal_semaphores = 0;

	void AddWait(vk::Semaphore semaphore, uint64_t tick = 1,
	             vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eAllCommands) {
		EXIT_IF(semaphore == nullptr || num_wait_semaphores >= MaxSemaphores);
		wait_semaphores[num_wait_semaphores] = semaphore;
		wait_ticks[num_wait_semaphores]      = tick;
		wait_stages[num_wait_semaphores++]   = stage;
	}

	void AddSignal(vk::Semaphore semaphore, uint64_t tick = 1) {
		EXIT_IF(semaphore == nullptr || num_signal_semaphores >= MaxSemaphores);
		signal_semaphores[num_signal_semaphores] = semaphore;
		signal_ticks[num_signal_semaphores++]    = tick;
	}
};

class CommandBuffer {
public:
	~CommandBuffer() = default;

	KYTY_CLASS_NO_COPY(CommandBuffer);

	[[nodiscard]] bool IsInvalid() const;
	[[nodiscard]] bool IsRendering() const noexcept { return m_rendering; }

	void SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0 = 0, uint32_t arg1 = 0,
	                  uint32_t arg2 = 0, uint32_t arg3 = 0, uint64_t arg4 = 0);
	void BeginRendering(const RenderState& state) const;
	void EndRendering() const;

	[[nodiscard]] vk::CommandBuffer Handle() const;

	// KYTY_MESH_BATCH: a pending batch of captured mesh draws registers a hook that replays them
	// before anything else is recorded. Handle(), BeginRendering() and EndRendering() run it
	// first; RawHandle() does not (only for code that orders itself against the batch). With
	// the flag unset the hook is never set and these are the unchanged functions.
	using FlushHook = void (*)(void* context, const CommandBuffer& buffer,
	                           MeshBatch::FlushReason reason);
	void SetFlushHook(FlushHook hook, void* context) const noexcept {
		m_flush_hook         = hook;
		m_flush_hook_context = context;
	}
	[[nodiscard]] bool HasFlushHook() const noexcept { return m_flush_hook != nullptr; }
	void               RunFlushHook(MeshBatch::FlushReason reason) const {
		if (m_flush_hook != nullptr) {
			const auto hook    = m_flush_hook;
			m_flush_hook       = nullptr;
			hook(m_flush_hook_context, *this, reason);
		}
	}
	[[nodiscard]] vk::CommandBuffer RawHandle() const;
	[[nodiscard]] GraphicContext&   GetGraphics() const noexcept { return m_graphics; }
	[[nodiscard]] RenderContext&    GetContext() const noexcept { return m_context; }
	[[nodiscard]] HW::Context&      GetRegisters() const noexcept { return *m_registers; }
	[[nodiscard]] HW::UserConfig&   GetUserConfig() const noexcept { return *m_user_config; }
	[[nodiscard]] HW::Shader&       GetShaders() const noexcept { return *m_shaders; }
	// KYTY_STATE_CACHE: graphics state already recorded into the current recording session.
	[[nodiscard]] StateCache::GraphicsStateShadow& StateShadow() const noexcept {
		return m_state_shadow;
	}

private:
	explicit CommandBuffer(CommandScheduler& scheduler);
	void Bind(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders) noexcept {
		m_registers   = &registers;
		m_user_config = &user_config;
		m_shaders     = &shaders;
	}

	void Begin();
	void End() const;

	RenderContext&      m_context;
	GraphicContext&     m_graphics;
	vk::CommandBuffer   m_buffer          = nullptr;
	uint32_t            m_debug_op        = 0;
	uint64_t            m_debug_submit_id = 0;
	uint32_t            m_debug_arg0      = 0;
	uint32_t            m_debug_arg1      = 0;
	uint32_t            m_debug_arg2      = 0;
	uint32_t            m_debug_arg3      = 0;
	uint64_t            m_debug_arg4      = 0;
	mutable RenderState m_render_state;
	mutable bool        m_rendering   = false;
	mutable FlushHook   m_flush_hook  = nullptr;
	mutable void*       m_flush_hook_context = nullptr;
	HW::Context*        m_registers   = nullptr;
	HW::UserConfig*     m_user_config = nullptr;
	HW::Shader*         m_shaders     = nullptr;
	mutable StateCache::GraphicsStateShadow m_state_shadow;

	friend class CommandScheduler;
};

// KYTY_MESH_BATCH: the graphics-bind-point part of CommitBindings, captured instead of recorded.
struct MeshGraphicsCapture {
	vk::PipelineLayout                    layout = nullptr;
	vk::ShaderStageFlags                  push_stages;
	bool                                  has_push_data = false;
	ShaderRecompiler::IR::PushData        push_data;
	bool                                  push_descriptors = false;
	vk::DescriptorSet                     set              = nullptr;
	std::vector<vk::WriteDescriptorSet>   writes;
	std::vector<vk::DescriptorBufferInfo> buffer_infos;
	std::vector<vk::DescriptorImageInfo>  image_infos;
	// Per write: index of its first buffer/image info, or UINT32_MAX.
	std::vector<std::pair<uint32_t, uint32_t>> info_starts;

	void Record(vk::CommandBuffer cmd) const;
};

// KYTY_MESH_BATCH: the dynamic state SetGraphicsDynamicParams records, captured by value. The
// member functions mirror the vk::CommandBuffer calls it makes, so the same template records into
// either; Record() replays them in the original order.
struct MeshDynamicState {
	static constexpr uint32_t MaxViewports = 16;
	uint32_t                                viewport_count = 0;
	std::array<vk::Viewport, MaxViewports>  viewports {};
	std::array<vk::Rect2D, MaxViewports>    scissors {};
	float                                   line_width = 1.0f;
	std::array<float, 4>                    blend {};
	vk::Bool32                              depth_test  = VK_FALSE;
	vk::Bool32                              depth_write = VK_FALSE;
	vk::CompareOp                           depth_compare {};
	bool                                    has_bounds_test = false;
	vk::Bool32                              bounds_test     = VK_FALSE;
	bool                                    has_bounds      = false;
	float                                   bounds_min = 0.0f, bounds_max = 0.0f;
	vk::Bool32                              bias_enable = VK_FALSE;
	bool                                    has_bias    = false;
	float                                   bias_constant = 0.0f, bias_clamp = 0.0f,
	                                        bias_slope = 0.0f;
	vk::Bool32                              stencil_test = VK_FALSE;
	struct StencilFace {
		bool          op_set = false, compare_set = false, write_set = false, ref_set = false;
		vk::StencilOp fail {}, pass {}, depth_fail {};
		vk::CompareOp compare {};
		uint32_t      compare_mask = 0, write_mask = 0, reference = 0;
	};
	std::array<StencilFace, 2>                              faces {};
	uint32_t                                                color_write_count = 0;
	std::array<vk::Bool32, RENDER_COLOR_ATTACHMENTS_MAX>    color_write {};

	void setViewportWithCount(uint32_t count, const vk::Viewport* v);
	void setScissorWithCount(uint32_t count, const vk::Rect2D* r);
	void setLineWidth(float w) { line_width = w; }
	void setBlendConstants(const float* c) { std::copy(c, c + 4, blend.begin()); }
	void setDepthTestEnable(vk::Bool32 v) { depth_test = v; }
	void setDepthWriteEnable(vk::Bool32 v) { depth_write = v; }
	void setDepthCompareOp(vk::CompareOp op) { depth_compare = op; }
	void setDepthBoundsTestEnable(vk::Bool32 v) {
		has_bounds_test = true;
		bounds_test     = v;
	}
	void setDepthBounds(float mn, float mx) {
		has_bounds = true;
		bounds_min = mn;
		bounds_max = mx;
	}
	void setDepthBiasEnable(vk::Bool32 v) { bias_enable = v; }
	void setDepthBias(float c, float clamp, float slope) {
		has_bias      = true;
		bias_constant = c;
		bias_clamp    = clamp;
		bias_slope    = slope;
	}
	void setStencilTestEnable(vk::Bool32 v) { stencil_test = v; }
	void setStencilOp(vk::StencilFaceFlagBits face, vk::StencilOp f, vk::StencilOp p,
	                  vk::StencilOp d, vk::CompareOp c);
	void setStencilCompareMask(vk::StencilFaceFlagBits face, uint32_t m);
	void setStencilWriteMask(vk::StencilFaceFlagBits face, uint32_t m);
	void setStencilReference(vk::StencilFaceFlagBits face, uint32_t r);
	void setColorWriteEnableEXT(uint32_t count, const vk::Bool32* enable);
	void setColorWriteEnableEXT(vk::ArrayProxy<const vk::Bool32> enable) {
		setColorWriteEnableEXT(enable.size(), enable.data());
	}

	void Record(vk::CommandBuffer cmd) const;
};

struct PendingMeshDraw {
	vk::Pipeline                     pipeline = nullptr;
	vk::PipelineLayout               layout   = nullptr;
	MeshGraphicsCapture              bindings;
	bool                             commit_bindings = false;
	std::array<uint32_t, ShaderRecompiler::IR::PushData::MeshDrawDwordCount> draw_data {};
	MeshDynamicState                 dynamic;
	bool                             set_feedback = false;
	vk::ImageAspectFlags             feedback_aspects;
	RenderState                      rendering;
	uint32_t                         vertex_count = 0;
};

class RenderExecutor {
public:
	explicit RenderExecutor(RenderContext& context): m_context(context) {}
	~RenderExecutor();
	KYTY_CLASS_NO_COPY(RenderExecutor);

	void DispatchDirect(uint64_t submit_id, CommandBuffer& buffer, uint32_t thread_group_x,
	                    uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode);
	void DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer, uint64_t args_addr,
	                      uint32_t mode);

	void PrepareBindings(const ShaderStageRuntime& runtime, PreparedBindings& prepared);
	void                           FindBuffers(std::span<PreparedBindings* const> stages);
	void                           RebindBuffers(PreparedBindings& bindings);
	void                           RebindImages(PreparedBindings& bindings);
	void CommitBindings(CommandBuffer& buffer, vk::PipelineBindPoint pipeline_bind_point,
	                    const PipelineCache::Pipeline&     pipeline,
	                    std::span<PreparedBindings* const> bindings,
	                    MeshGraphicsCapture*               capture = nullptr);
	// KYTY_MESH_BATCH: replay the captured mesh draws (no-op when none are pending).
	void FlushMeshBatch(const CommandBuffer& buffer, MeshBatch::FlushReason reason);

private:
	// KYTY_GPU_INDIRECT (renderDraw.cpp). Returns false, before recording anything that the CPU
	// path would not also record, when the draw cannot run from GPU-resident arguments.
	[[nodiscard]] bool DrawIndirectGpu(uint64_t submit_id, CommandBuffer& buffer,
	                                   const GpuIndirectDraw& args);
	// GS user SGPRs / NUM_INSTANCES whose current value is only known to the GPU.
	struct GpuPendingRegister {
		GpuIndirectHistoryRef ref;
		uint32_t              role          = 0; // 0 vertex offset, 1 instance offset, 2 index offset
		uint64_t              ref_args_addr = 0; // guest record (fallback when the slot recycled)
		bool                  ref_indexed   = false;
	};
	void ResolveGpuPendingRegisters(CommandBuffer& buffer, uint32_t slots);
	[[nodiscard]] bool ReadGpuIndirectHistory(const GpuIndirectHistoryRef& ref,
	                                          GpuIndirectHistoryValues& values);
	[[nodiscard]] uint32_t GpuPendingSlots(CommandBuffer& buffer);
	[[nodiscard]] uint32_t GpuPendingSlotsReadBy(CommandBuffer& buffer, const DrawRenderState& state);
	void                   EnsureGpuIndirect();
	void                   LogGpuIndirectStats();

	void DrawIndex(uint64_t submit_id, CommandBuffer& buffer, const DrawIndexArgs& args);
	void DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args);

	struct GraphicsBindings {
		std::array<PreparedBindings, 3> vertex;
		std::optional<PreparedBindings> pixel;
	};

	[[nodiscard]] TextureBinding ResolveTexture(const ShaderRecompiler::IR::ImageResource& resource,
	                                            const ShaderRecompiler::IR::DescriptorValue& value);
	void PrepareGraphicsBindings(std::span<PreparedBindings* const> stages,
	                             std::span<RenderColorInfo> colors);
	void ResolveRenderColorTarget(CommandBuffer& buffer, RenderColorInfo& target,
	                              uint32_t render_target_slice_offset, uint32_t render_target_slot,
	                              bool ignore_target_mask = false, bool exact_format = false);
	void ResolveRenderDepthTarget(CommandBuffer& buffer, RenderDepthInfo& target);
	[[nodiscard]] bool DepthStencilCopy(CommandBuffer& buffer);
	[[nodiscard]] bool PrepareDrawRenderState(CommandBuffer& buffer,
	                                          const DrawCallInfo& draw,
	                                          uint32_t            render_target_slice_offset,
	                                          DrawRenderState& state);
	void ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer, const DrawCallInfo& draw,
	                         DrawRenderState& state, vk::PrimitiveTopology topology,
	                         const DrawEmitInfo& emit, const DrawIndexBufferSource& index_source,
	                         bool primitive_restart_enable);
	[[nodiscard]] RenderState AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
	                                               uint32_t color_count, RenderDepthInfo& depth,
	                                               vk::ImageAspectFlags& feedback_aspects,
	                                               std::span<PreparedBindings* const> stages = {});
	[[nodiscard]] bool        ResolveColorTargets(CommandBuffer& buffer,
	                                              uint32_t render_target_slice_offset);
	void                      BindImage(ImageId id, bool storage);
	void                      BindRenderTarget(ImageId id);
	void                      ResetBindings();
	[[nodiscard]] bool        TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
	                                                     const CommandBuffer&          buffer);
	[[nodiscard]] bool TryConsumeComputeImageClear(const ShaderComputeInputInfo& input,
	                                              CommandBuffer& command, uint32_t group_x,
	                                              uint32_t group_y, uint32_t group_z, uint32_t mode);

	// Mesh emulation (no VK_EXT_mesh_shader): compute pass + generated vertex draw.
	void ExecuteEmulatedMeshDraw(CommandBuffer& buffer, const DrawCallInfo& draw,
	                             DrawRenderState& state, PipelineCache::Pipeline& pipeline,
	                             std::span<PreparedBindings* const> stages,
	                             const RenderState& rendering, vk::ImageAspectFlags feedback_aspects,
	                             std::span<const uint32_t> draw_data, uint32_t mesh_groups);
	[[nodiscard]] Buffer& MeshEmulationScratch();
	[[nodiscard]] bool MeshDrawBatchable(const DrawCallInfo& draw, DrawRenderState& state,
	                                     std::span<PreparedBindings* const> stages,
	                                     uint32_t mesh_groups, uint64_t& scratch_bytes);
	void ExecuteBatchedMeshDraw(CommandBuffer& buffer, const DrawCallInfo& draw,
	                            DrawRenderState& state, PipelineCache::Pipeline& pipeline,
	                            std::span<PreparedBindings* const> stages,
	                            const RenderState& rendering, vk::ImageAspectFlags feedback_aspects,
	                            std::span<const uint32_t> draw_data, uint32_t mesh_groups,
	                            uint64_t scratch_bytes);
	static void MeshBatchFlushHook(void* context, const CommandBuffer& buffer,
	                               MeshBatch::FlushReason reason);

	RenderContext&                        m_context;
	std::unique_ptr<Buffer>               m_mesh_scratch;
	// KYTY_GPU_INDIRECT state (all GPU-thread only).
	std::unique_ptr<Buffer>                       m_gpu_indirect_commands;
	std::unique_ptr<Buffer>                       m_gpu_indirect_history;
	vk::DescriptorSetLayout                       m_gpu_indirect_set_layout      = nullptr;
	vk::PipelineLayout                            m_gpu_indirect_pipeline_layout = nullptr;
	vk::Pipeline                                  m_gpu_indirect_pipeline        = nullptr;
	uint32_t                                      m_gpu_indirect_cursor          = 0;
	uint32_t                                      m_gpu_indirect_sequence        = 0;
	std::array<GpuPendingRegister, 32>            m_gpu_pending {};
	uint32_t                                      m_gpu_pending_mask             = 0;
	GpuPendingRegister                            m_gpu_last_ref {}; // NUM_INSTANCES source
	GraphicsBindings                     m_graphics_bindings;
	PreparedBindings                     m_compute_bindings;
	std::vector<ImageId>                  m_bound_images;
	std::vector<vk::DescriptorImageInfo>  m_descriptor_images;
	std::vector<vk::WriteDescriptorSet>   m_descriptor_writes;
	// KYTY_MESH_BATCH state. m_mesh_scratch_offset is the scratch byte offset CommitBindings
	// binds (always 0 unless a batched draw is being recorded).
	uint64_t                              m_mesh_scratch_offset = 0;
	std::vector<PendingMeshDraw>          m_mesh_pending;
	MeshBatch::PendingTargets             m_mesh_pending_targets;
	MeshBatch::ScratchRing                m_mesh_ring;
	MeshBatch::Stats                      m_mesh_stats;
	bool                                  m_mesh_flushing = false;
	StateCache::WrittenRanges             m_written_ranges;

	friend class CommandProcessor;
	friend struct RenderExecutorTestAccess;
};

// KYTY_GPU_INDIRECT statistics for packets the command processor keeps on the CPU path:
// a multi/count-indirect packet, or a patch destination outside GS user data / unaligned record.
void GpuIndirectNoteCpuMulti();
void GpuIndirectNoteCpuDestination();

[[nodiscard]] bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                                            uint32_t group_y, uint32_t group_z, uint32_t mode,
                                            ShaderBufferResource& descriptor,
                                            uint32_t& packed_clear, uint64_t& size);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_ */
