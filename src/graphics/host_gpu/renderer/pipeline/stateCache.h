#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_STATECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_STATECACHE_H_

// KYTY_STATE_CACHE=1 (opt-in): per-command-buffer shadow of the graphics state that the draw path
// re-records for every draw (bound graphics pipeline, dynamic state, vertex and index buffers).
// A command is skipped only when the shadow proves that the command buffer already holds exactly
// the same value for that state, set by an earlier command of the same recording session, with
// nothing in between that could have disturbed it.
//
// Vulkan rules relied on (Vulkan 1.3 spec, "Pipeline Dynamic State" and "Vertex Input Description"):
//  * dynamic state persists for the lifetime of the command buffer, across vkCmdBeginRendering /
//    vkCmdEndRendering, barriers and compute dispatches, until it is set again or a pipeline that
//    has that state *static* is bound (then it becomes undefined);
//  * the pipeline bound to VK_PIPELINE_BIND_POINT_GRAPHICS persists across render passes and is
//    not affected by compute binds;
//  * vertex / index buffer bindings persist across pipeline binds and render passes.
// The shadow is therefore invalidated (all bits cleared) when recording begins (CommandBuffer::
// Begin) and whenever code outside the shadowed path binds a graphics pipeline (BlitHelper).
// Every graphics pipeline created by PipelineCache shares one dynamic-state list (shaders.cpp),
// so binding one of them never makes shadowed dynamic state undefined.
//
// Values are compared bit-exactly (memcmp), never with float ==, so -0.0/+0.0 and NaN payloads
// are treated as different values and re-emitted.
//
// Header-only so the sandbox harness compiles this exact code.

#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace Libs::Graphics::StateCache {

[[nodiscard]] inline bool ParseFlag(const char* value) noexcept {
	return value != nullptr &&
	       (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0);
}

// Parsed once; strictly "1" or "true".
[[nodiscard]] inline bool Enabled() noexcept {
	// The shadow is not updated by the KYTY_MESH_BATCH replay or the KYTY_GPU_INDIRECT draw path,
	// so the cache is forced off when either is on.
	static const bool enabled = ParseFlag(std::getenv("KYTY_STATE_CACHE")) &&
	                            !ParseFlag(std::getenv("KYTY_MESH_BATCH")) &&
	                            !ParseFlag(std::getenv("KYTY_GPU_INDIRECT"));
	return enabled;
}

// KYTY_SPEC_READ_HOIST=1 (opt-in, independent of KYTY_STATE_CACHE): CommitBindings scalar-read
// overlap check with the written ranges collected once per call (WrittenRanges below). Pure CPU
// refactor: same checks, same order, same EXITs; records nothing different into Vulkan.
[[nodiscard]] inline bool SpecReadHoistEnabled() noexcept {
	static const bool enabled = ParseFlag(std::getenv("KYTY_SPEC_READ_HOIST"));
	return enabled;
}

struct Stats {
	uint64_t draws          = 0;
	uint64_t emitted        = 0; // shadowed commands recorded
	uint64_t skipped        = 0; // shadowed commands proven redundant and not recorded
	uint64_t pipeline_binds = 0;
	uint64_t pipeline_skips = 0;
	uint64_t vertex_skips   = 0;
	uint64_t index_skips    = 0;
	uint64_t invalidations  = 0;
	uint64_t spec_reads     = 0; // CommitBindings scalar-read overlap check: reads checked
	uint64_t spec_bound     = 0; //   bound images the default loop walks per read
	uint64_t spec_ranges    = 0; //   written ranges the hoisted loop walks per read
};

[[nodiscard]] inline Stats& GetStats() noexcept {
	static Stats stats;
	return stats;
}

template <typename T>
[[nodiscard]] inline bool SameBits(const T& a, const T& b) noexcept {
	return std::memcmp(&a, &b, sizeof(T)) == 0;
}

class GraphicsStateShadow {
public:
	static constexpr uint32_t MaxViewports     = 16;
	static constexpr uint32_t MaxVertexBuffers = 32;

	void Invalidate() noexcept {
		m_valid = 0;
		Flush();
		GetStats().invalidations++;
	}

	// Counters are kept in the shadow (hot cache line) and folded into the global Stats here.
	void Flush() noexcept {
		auto& s = GetStats();
		s.emitted += m_emitted;
		s.skipped += m_skipped;
		s.pipeline_binds += m_pipeline_binds;
		s.pipeline_skips += m_pipeline_skips;
		s.vertex_skips += m_vertex_skips;
		s.index_skips += m_index_skips;
		m_emitted = m_skipped = m_pipeline_binds = m_pipeline_skips = m_vertex_skips = m_index_skips = 0;
	}

	// Each call returns true when the command must be recorded (and records the new value), false
	// when the command buffer provably already holds this exact value.
	bool Pipeline(vk::Pipeline pipeline) noexcept {
		m_pipeline_binds++;
		const VkPipeline raw = pipeline;
		if (Test(BitPipeline) && m_pipeline == raw) {
			m_pipeline_skips++;
			return Skip();
		}
		m_pipeline = raw;
		return Emit(BitPipeline);
	}

	bool ViewportWithCount(uint32_t count, const vk::Viewport* viewports) noexcept {
		if (count > MaxViewports) return EmitUncached(BitViewport);
		if (Test(BitViewport) && m_viewport_count == count &&
		    std::memcmp(m_viewports.data(), viewports, sizeof(vk::Viewport) * count) == 0) {
			return Skip();
		}
		m_viewport_count = count;
		std::memcpy(m_viewports.data(), viewports, sizeof(vk::Viewport) * count);
		return Emit(BitViewport);
	}

	bool ScissorWithCount(uint32_t count, const vk::Rect2D* scissors) noexcept {
		if (count > MaxViewports) return EmitUncached(BitScissor);
		if (Test(BitScissor) && m_scissor_count == count &&
		    std::memcmp(m_scissors.data(), scissors, sizeof(vk::Rect2D) * count) == 0) {
			return Skip();
		}
		m_scissor_count = count;
		std::memcpy(m_scissors.data(), scissors, sizeof(vk::Rect2D) * count);
		return Emit(BitScissor);
	}

	bool LineWidth(float width) noexcept { return Scalar(BitLineWidth, m_line_width, width); }

	bool BlendConstants(const float* constants) noexcept {
		std::array<float, 4> value {};
		std::memcpy(value.data(), constants, sizeof(value));
		return Scalar(BitBlend, m_blend, value);
	}

	bool DepthTestEnable(vk::Bool32 v) noexcept { return Scalar(BitDepthTest, m_depth_test, v); }
	bool DepthWriteEnable(vk::Bool32 v) noexcept { return Scalar(BitDepthWrite, m_depth_write, v); }
	bool DepthCompareOp(vk::CompareOp v) noexcept {
		return Scalar(BitDepthCompare, m_depth_compare, v);
	}
	bool DepthBoundsTestEnable(vk::Bool32 v) noexcept {
		return Scalar(BitDepthBoundsTest, m_depth_bounds_test, v);
	}
	bool DepthBounds(float min, float max) noexcept {
		return Scalar(BitDepthBounds, m_depth_bounds, std::array<float, 2> {min, max});
	}
	bool DepthBiasEnable(vk::Bool32 v) noexcept { return Scalar(BitDepthBiasEnable, m_bias_enable, v); }
	bool DepthBias(float constant, float clamp, float slope) noexcept {
		return Scalar(BitDepthBias, m_bias, std::array<float, 3> {constant, clamp, slope});
	}
	bool StencilTestEnable(vk::Bool32 v) noexcept {
		return Scalar(BitStencilTest, m_stencil_test, v);
	}

	// Kyty records stencil state with exactly one face bit per call (eFront or eBack). Any other
	// mask is not shadowed: it is emitted and the affected faces are invalidated.
	bool StencilOp(vk::StencilFaceFlagBits face, vk::StencilOp fail, vk::StencilOp pass,
	               vk::StencilOp depth_fail, vk::CompareOp compare) noexcept {
		const std::array<uint32_t, 4> value {static_cast<uint32_t>(fail), static_cast<uint32_t>(pass),
		                                     static_cast<uint32_t>(depth_fail),
		                                     static_cast<uint32_t>(compare)};
		const int f = Face(face);
		if (f < 0) return EmitUncached(BitStencilOpF | BitStencilOpB);
		return Scalar(f == 0 ? BitStencilOpF : BitStencilOpB, m_stencil_op[f], value);
	}
	bool StencilCompareMask(vk::StencilFaceFlagBits face, uint32_t mask) noexcept {
		const int f = Face(face);
		if (f < 0) return EmitUncached(BitStencilCmpF | BitStencilCmpB);
		return Scalar(f == 0 ? BitStencilCmpF : BitStencilCmpB, m_stencil_compare[f], mask);
	}
	bool StencilWriteMask(vk::StencilFaceFlagBits face, uint32_t mask) noexcept {
		const int f = Face(face);
		if (f < 0) return EmitUncached(BitStencilWriteF | BitStencilWriteB);
		return Scalar(f == 0 ? BitStencilWriteF : BitStencilWriteB, m_stencil_write[f], mask);
	}
	bool StencilReference(vk::StencilFaceFlagBits face, uint32_t reference) noexcept {
		const int f = Face(face);
		if (f < 0) return EmitUncached(BitStencilRefF | BitStencilRefB);
		return Scalar(f == 0 ? BitStencilRefF : BitStencilRefB, m_stencil_reference[f], reference);
	}

	bool FeedbackLoop(vk::ImageAspectFlags aspects) noexcept {
		return Scalar(BitFeedback, m_feedback, static_cast<VkImageAspectFlags>(aspects));
	}

	bool ColorWriteEnable(uint32_t count, const vk::Bool32* enables) noexcept {
		if (count > 8) return EmitUncached(BitColorWrite);
		std::array<vk::Bool32, 8> value {};
		std::memcpy(value.data(), enables, sizeof(vk::Bool32) * count);
		if (Test(BitColorWrite) && m_color_write_count == count && SameBits(m_color_write, value)) {
			return Skip();
		}
		m_color_write_count = count;
		m_color_write       = value;
		return Emit(BitColorWrite);
	}

	// vkCmdBindVertexBuffers2(first=0, count, buffers, offsets, sizes, strides=nullptr)
	bool VertexBuffers(uint32_t count, const vk::Buffer* buffers, const vk::DeviceSize* offsets,
	                   const vk::DeviceSize* sizes) noexcept {
		if (count > MaxVertexBuffers || sizes == nullptr) return EmitUncached(BitVertex);
		if (Test(BitVertex) && m_vertex_count == count &&
		    std::memcmp(m_vertex_buffers.data(), buffers, sizeof(vk::Buffer) * count) == 0 &&
		    std::memcmp(m_vertex_offsets.data(), offsets, sizeof(vk::DeviceSize) * count) == 0 &&
		    std::memcmp(m_vertex_sizes.data(), sizes, sizeof(vk::DeviceSize) * count) == 0) {
			m_vertex_skips++;
			return Skip();
		}
		m_vertex_count = count;
		std::memcpy(m_vertex_buffers.data(), buffers, sizeof(vk::Buffer) * count);
		std::memcpy(m_vertex_offsets.data(), offsets, sizeof(vk::DeviceSize) * count);
		std::memcpy(m_vertex_sizes.data(), sizes, sizeof(vk::DeviceSize) * count);
		return Emit(BitVertex);
	}

	bool IndexBuffer(vk::Buffer buffer, vk::DeviceSize offset, vk::IndexType type) noexcept {
		const VkBuffer raw = buffer;
		if (Test(BitIndex) && m_index_buffer == raw && m_index_offset == offset &&
		    m_index_type == type) {
			m_index_skips++;
			return Skip();
		}
		m_index_buffer = raw;
		m_index_offset = offset;
		m_index_type   = type;
		return Emit(BitIndex);
	}

	[[nodiscard]] uint64_t ValidMask() const noexcept { return m_valid; }

private:
	enum : uint64_t {
		BitPipeline        = 1ull << 0,
		BitViewport        = 1ull << 1,
		BitScissor         = 1ull << 2,
		BitLineWidth       = 1ull << 3,
		BitBlend           = 1ull << 4,
		BitDepthTest       = 1ull << 5,
		BitDepthWrite      = 1ull << 6,
		BitDepthCompare    = 1ull << 7,
		BitDepthBoundsTest = 1ull << 8,
		BitDepthBounds     = 1ull << 9,
		BitDepthBiasEnable = 1ull << 10,
		BitDepthBias       = 1ull << 11,
		BitStencilTest     = 1ull << 12,
		BitStencilOpF      = 1ull << 13,
		BitStencilOpB      = 1ull << 14,
		BitStencilCmpF     = 1ull << 15,
		BitStencilCmpB     = 1ull << 16,
		BitStencilWriteF   = 1ull << 17,
		BitStencilWriteB   = 1ull << 18,
		BitStencilRefF     = 1ull << 19,
		BitStencilRefB     = 1ull << 20,
		BitFeedback        = 1ull << 21,
		BitColorWrite      = 1ull << 22,
		BitVertex          = 1ull << 23,
		BitIndex           = 1ull << 24,
	};

	[[nodiscard]] bool Test(uint64_t bit) const noexcept { return (m_valid & bit) == bit; }
	bool Emit(uint64_t bit) noexcept {
		m_valid |= bit;
		m_emitted++;
		return true;
	}
	bool EmitUncached(uint64_t bits) noexcept {
		m_valid &= ~bits;
		m_emitted++;
		return true;
	}
	bool Skip() noexcept {
		m_skipped++;
		return false;
	}
	static int Face(vk::StencilFaceFlagBits face) noexcept {
		if (face == vk::StencilFaceFlagBits::eFront) return 0;
		if (face == vk::StencilFaceFlagBits::eBack) return 1;
		return -1;
	}
	template <typename T>
	bool Scalar(uint64_t bit, T& slot, const T& value) noexcept {
		if (Test(bit) && SameBits(slot, value)) {
			return Skip();
		}
		slot = value;
		return Emit(bit);
	}

	uint64_t                                   m_valid = 0;
	uint32_t                                   m_emitted = 0, m_skipped = 0, m_pipeline_binds = 0;
	uint32_t                                   m_pipeline_skips = 0, m_vertex_skips = 0, m_index_skips = 0;
	VkPipeline                                 m_pipeline {};
	uint32_t                                   m_viewport_count = 0;
	uint32_t                                   m_scissor_count  = 0;
	std::array<vk::Viewport, MaxViewports>     m_viewports {};
	std::array<vk::Rect2D, MaxViewports>       m_scissors {};
	float                                      m_line_width = 0.0f;
	std::array<float, 4>                       m_blend {};
	vk::Bool32                                 m_depth_test  = 0;
	vk::Bool32                                 m_depth_write = 0;
	vk::CompareOp                              m_depth_compare {};
	vk::Bool32                                 m_depth_bounds_test = 0;
	std::array<float, 2>                       m_depth_bounds {};
	vk::Bool32                                 m_bias_enable = 0;
	std::array<float, 3>                       m_bias {};
	vk::Bool32                                 m_stencil_test = 0;
	std::array<std::array<uint32_t, 4>, 2>     m_stencil_op {};
	std::array<uint32_t, 2>                    m_stencil_compare {};
	std::array<uint32_t, 2>                    m_stencil_write {};
	std::array<uint32_t, 2>                    m_stencil_reference {};
	VkImageAspectFlags                         m_feedback = 0;
	uint32_t                                   m_color_write_count = 0;
	std::array<vk::Bool32, 8>                  m_color_write {};
	uint32_t                                   m_vertex_count = 0;
	std::array<vk::Buffer, MaxVertexBuffers>   m_vertex_buffers {};
	std::array<vk::DeviceSize, MaxVertexBuffers> m_vertex_offsets {};
	std::array<vk::DeviceSize, MaxVertexBuffers> m_vertex_sizes {};
	VkBuffer                                   m_index_buffer {};
	vk::DeviceSize                             m_index_offset = 0;
	vk::IndexType                              m_index_type {};
};

// CommitBindings scalar-read overlap check, hoisted form. The default code walks, for every
// captured scalar read, every bound image (slot lookup + binding flags + three ranges) and every
// written buffer of every stage. The set of written ranges does not depend on the read and cannot
// change during the check (no side effects in between), so it is collected once, in the same
// order, and every read is tested against that flat list: images first, then buffers, exactly
// the order in which the default loop would report the first overlap.
struct WrittenRanges {
	struct Range {
		uint64_t address;
		uint64_t size;
	};
	std::vector<Range> images;
	std::vector<Range> buffers;
	void               Clear() {
		images.clear();
		buffers.clear();
	}
};

} // namespace Libs::Graphics::StateCache

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_STATECACHE_H_
