#include "graphics/host_gpu/renderer/renderDraw.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/shaders/gpu_indirect_mesh_args_spv.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/meshEmulation.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

// KYTY_INTERNAL_FRAME_MIN_SECONDS=N: internal frame stats only sample draws/dispatches recorded
// N seconds or more after process start (e.g. the title menu instead of boot).
static bool InternalFrameStatsFrameReached(const CommandBuffer& /*buffer*/) {
	static const auto start = std::chrono::steady_clock::now();
	static const int  min_seconds = [] {
		const char* value = std::getenv("KYTY_INTERNAL_FRAME_MIN_SECONDS");
		return value != nullptr ? std::atoi(value) : 0;
	}();
	return min_seconds == 0 ||
	       std::chrono::steady_clock::now() - start >= std::chrono::seconds(min_seconds);
}


std::pair<int32_t, uint32_t> ResolveDrawOffsets(uint32_t index_offset,
	                                           const ShaderVertexInputInfo& vs_input_info) {
	auto     vertex_offset   = static_cast<int32_t>(index_offset);
	uint32_t instance_offset = 0;
	if (!vs_input_info.fetch_embedded) {
		return {vertex_offset, instance_offset};
	}

	EXIT_IF(!vs_input_info.stage);
	const auto& program   = *vs_input_info.stage.program;
	const auto& resources = *vs_input_info.stage.resources;
	if (index_offset == 0 &&
	    program.info.vertex_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.vertex_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			vertex_offset = static_cast<int32_t>(resources.user_data[index]);
		}
	}
	if (program.info.instance_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.instance_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			instance_offset = resources.user_data[index];
		}
	}

	return {vertex_offset, instance_offset};
}

static std::atomic<uint32_t> g_draw_state_log_count   = 0;
static std::atomic<uint32_t> g_draw_input_log_count   = 0;
static std::atomic<uint32_t> g_mrt_state_log_count    = 0;

static std::atomic<uint32_t> g_framebuffer_skip_log_count = 0;

static float ConvertPolygonOffsetConstantFactor(float guest_factor, const HW::PolyOffset& offset,
                                                vk::Format host_depth_format) {
	if (offset.db_is_float_fmt) {
		return guest_factor;
	}

	int host_depth_bits = 0;
	switch (host_depth_format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint: host_depth_bits = 16; break;
		case vk::Format::eD24UnormS8Uint: host_depth_bits = 24; break;
		default:
			// A fixed-point guest bias cannot be represented exactly by a floating-point host
			// attachment without VK_EXT_depth_bias_control.
			return guest_factor;
	}
	return std::ldexp(guest_factor, host_depth_bits + offset.neg_num_db_bits);
}

static const char* RenderColorTypeName(const RenderColorInfo& color) {
	return color.image_id ? "RenderTexture" : "NoColorOutput";
}

static void LogFramebufferSkip(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               uint32_t index_count, uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	auto log_id = g_framebuffer_skip_log_count.fetch_add(1, std::memory_order_relaxed);
	if (log_id >= 128) {
		return;
	}

	LOGF(
	    "DrawFramebufferSkip[%u]: %s color=%s color_addr=0x%010" PRIx64 " color_size=0x%016" PRIx64
	    " color_image=%s depth_format=%s depth_image=%s depth_vaddr_num=%d target_mask=0x%08" PRIx32
	    " prim=%u index_count=%u flags=0x%08" PRIx32 "\n",
	    log_id, draw_name, RenderColorTypeName(color), color.desc.info.data.address,
	    color.desc.info.data.size, color.image_id ? "yes" : "no",
	    vk::to_string(depth.desc.view_info.format).c_str(), depth.image_id ? "yes" : "no",
	    static_cast<int>(!depth.desc.info.data.Empty()) +
	        static_cast<int>(depth.desc.info.HasStencil()),
	    ctx.GetRenderTargetMask(), static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags);
}

static void LogMrtState(const char* draw_name, const CommandBuffer& buffer,
                        const ShaderPixelInputInfo& ps_input_info) {
	const auto& ctx            = buffer.GetRegisters();
	const auto& sh_regs        = ctx.GetShaderRegisters();
	const auto  rt_mask        = ctx.GetRenderTargetMask();
	const auto  cb_shader_mask = sh_regs.m_cbShaderMask;
	const auto& bc0            = ctx.GetBlendControl(0);

	auto log_id = g_mrt_state_log_count.fetch_add(1);
	if (log_id >= 32) {
		return;
	}

	LOGF("MrtState[%u]: %s rt_mask=0x%08" PRIx32 " cb_shader_mask=0x%08" PRIx32
	     " blend0=%s src=%u dst=%u alpha_src=%u alpha_dst=%u sep_alpha=%s\n",
	     log_id, draw_name, rt_mask, cb_shader_mask, bc0.enable ? "true" : "false",
	     bc0.color_srcblend, bc0.color_destblend, bc0.alpha_srcblend, bc0.alpha_destblend,
	     bc0.separate_alpha_blend ? "true" : "false");

	for (uint32_t i = 0; i < 8; i++) {
		const auto& rt  = ctx.GetRenderTarget(i);
		const auto& bc  = ctx.GetBlendControl(i);
		const auto  ctm = (rt_mask >> (i * 4u)) & 0x0fu;
		const auto  csm = (cb_shader_mask >> (i * 4u)) & 0x0fu;

		if (rt.base.addr == 0 && ps_input_info.target_output_mode[i] == 0 && ctm == 0 && csm == 0 &&
		    !bc.enable) {
			continue;
		}

		LOGF("MrtState[%u]: slot=%u addr=0x%010" PRIx64
		     " target_mask=0x%x shader_mask=0x%x out_mode=%u"
		     " fmt=0x%08" PRIx32 " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32
		     " width=%u height=%u tile=%u"
		     " blend=%s src=%u dst=%u alpha_src=%u alpha_dst=%u\n",
		     log_id, i, rt.base.addr, ctm, csm, ps_input_info.target_output_mode[i],
		     static_cast<uint32_t>(rt.info.format), static_cast<uint32_t>(rt.info.channel_type),
		     static_cast<uint32_t>(rt.info.channel_order), rt.attrib2.width + 1,
		     rt.attrib2.height + 1, static_cast<uint32_t>(rt.attrib3.tile_mode),
		     bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.alpha_srcblend,
		     bc.alpha_destblend);
	}
}

static void LogDrawTargetState(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               const ShaderPixelInputInfo& ps_input_info, uint32_t index_count,
                               uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!color.image_id) {
		return;
	}

	auto log_id = g_draw_state_log_count.fetch_add(1);
	if (log_id >= 192) {
		return;
	}

	const auto& cc             = ctx.GetColorControl();
	const auto& bc             = ctx.GetBlendControl(color.target_slot);
	const auto& dc             = ctx.GetDepthControl();
	const auto& vp             = ctx.GetScreenViewport();
	const auto& vp0            = vp.viewports[0];
	const auto& ps_resources   = ps_input_info.stage.program->info;
	const auto  sampled_images = std::count_if(
	    ps_resources.images.begin(), ps_resources.images.end(), [](const auto& image) {
		    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
	    });

	const auto extent = color.Extent();
	const auto sc     = calc_final_scissor(vp, ctx.GetScanModeControl(), extent, 0);

	LOGF(
	    "DrawTargetState[%u]: frame=%d %s target=%s addr=0x%010" PRIx64
	    " extent=%ux%u prim=%u index_count=%u flags=0x%08" PRIx32 " color_mask=0x%08" PRIx32
	    " cc_mode=%u cc_op=0x%02x"
	    " blend=%s src=%u dst=%u comb=%u ps_tex=%d sampled=%d storage=%d ps_kill=%s target_mode0=%u"
	    " depth_test=%s depth_write=%s depth_func=%u depth_clear=%s viewport=(%.1f,%.1f %.1fx%.1f) "
	    "scissor=(%d,%d)-(%d,%d)\n",
	    log_id, buffer.GetContext().GetGpu().GetFrameNum(), draw_name, RenderColorTypeName(color),
	    color.desc.info.data.address, extent.width, extent.height,
	    static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags, ctx.GetRenderTargetMask(),
	    cc.mode, cc.op,
	    bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.color_comb_fcn,
	    static_cast<int>(ps_resources.images.size()), static_cast<int>(sampled_images),
	    static_cast<int>(ps_resources.images.size() - sampled_images),
	    ps_input_info.ps_pixel_kill_enable ? "true" : "false", ps_input_info.target_output_mode[0],
	    dc.z_enable ? "true" : "false", dc.z_write_enable ? "true" : "false", dc.zfunc,
	    depth.depth_clear_enable ? "true" : "false", vp0.xoffset - vp0.xscale,
	    vp0.yoffset - vp0.yscale, vp0.xscale * 2.0f, vp0.yscale * 2.0f, sc.left, sc.top, sc.right,
	    sc.bottom);

	LogMrtState(draw_name, buffer, ps_input_info);
	if (([] { static const bool kyty_env_set = std::getenv("KYTY_DRAW_RESOURCE_STATS") != nullptr; return kyty_env_set; }()) &&
	    ps_input_info.stage.resources != nullptr) {
		const auto& resources = *ps_input_info.stage.resources;
		for (size_t i = 0; i < resources.images.size(); ++i) {
			const auto descriptor = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
			LOGF("DrawResourceState[%u]: image=%zu base=0x%012" PRIx64 " type=%u format=%u extent=%ux%u\n",
			     log_id, i, descriptor.Base40(), static_cast<uint32_t>(descriptor.Type()),
			     static_cast<uint32_t>(descriptor.Format()),
			     static_cast<uint32_t>(descriptor.Width5()) + 1u,
			     static_cast<uint32_t>(descriptor.Height5()) + 1u);
		}
	}
}

static void LogDrawInputState(const CommandBuffer& buffer, const RenderColorInfo& color,
                              const ShaderVertexInputInfo& vs_input_info,
                              uint32_t index_type_and_size, uint32_t index_count,
                              const void* index_addr) {
	// KYTY_DRAW_INPUT_MIN_FRAME=N: start the 64-draw input log at guest frame N (steady state).
	static const int min_frame = [] {
		const char* value = std::getenv("KYTY_DRAW_INPUT_MIN_FRAME");
		return value != nullptr ? std::atoi(value) : 0;
	}();
	if (buffer.GetContext().GetGpu().GetFrameNum() < min_frame) {
		return;
	}
	auto log_id = g_draw_input_log_count.fetch_add(1);
	if (log_id >= 64) {
		return;
	}

	LOGF("DrawInputState[%u]: frame=%d target=%s addr=0x%010" PRIx64
	     " index_type=%u index_count=%u index_addr=0x%016" PRIx64
	     " vs_resources=%d vs_buffers=%d\n",
	     log_id, buffer.GetContext().GetGpu().GetFrameNum(), RenderColorTypeName(color),
	     color.desc.info.data.address, index_type_and_size, index_count,
	     reinterpret_cast<uint64_t>(index_addr), vs_input_info.resources_num,
	     vs_input_info.buffers_num);

	for (int bi = 0; bi < vs_input_info.buffers_num; bi++) {
		const auto& b = vs_input_info.buffers[bi];
		LOGF("DrawInputState[%u]: vb[%d] addr=0x%010" PRIx64
		     " stride=%u records=%u fetch_index=%u\n",
		     log_id, bi, b.addr, b.stride, b.num_records, b.fetch_index);

		const auto* bytes = reinterpret_cast<const uint8_t*>(b.addr);
		if (bytes != nullptr && b.stride != 0) {
			const uint32_t records = std::min<uint32_t>(b.num_records, 4u);
			for (uint32_t rec = 0; rec < records; rec++) {
				const auto* rec_bytes = bytes + static_cast<uint64_t>(rec) * b.stride;
				const auto  dword_num = std::min<uint32_t>(b.stride / 4u, 12u);
				uint32_t    raw[12]   = {};
				float       flt[12]   = {};
				for (uint32_t i = 0; i < dword_num; i++) {
					std::memcpy(&raw[i], rec_bytes + i * 4u, sizeof(raw[i]));
					std::memcpy(&flt[i], rec_bytes + i * 4u, sizeof(flt[i]));
				}
				LOGF("DrawInputState[%u]: vb[%d].rec[%u] stride=%u dwords=%u raw=%08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " f=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f)\n",
				     log_id, bi, rec, b.stride, dword_num, raw[0], raw[1], raw[2], raw[3], raw[4],
				     raw[5], raw[6], raw[7], raw[8], flt[0], flt[1], flt[2], flt[3], flt[4], flt[5],
				     flt[6], flt[7], flt[8]);

				for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
					const auto& r  = vs_input_info.resources[ai];
					const auto& rd = vs_input_info.resources_dst[ai];
					if (rd.buffer_index != bi) {
						continue;
					}
					const auto offset = static_cast<uint32_t>(r.Base48() - b.addr);
					if (offset + 4u <= b.stride &&
					    r.Format() == Prospero::BufferFormat::k8_8_8_8UNorm) {
						uint32_t packed = 0;
						std::memcpy(&packed, rec_bytes + offset, sizeof(packed));
						const auto r8 = (packed >> 0u) & 0xffu;
						const auto g8 = (packed >> 8u) & 0xffu;
						const auto b8 = (packed >> 16u) & 0xffu;
						const auto a8 = (packed >> 24u) & 0xffu;
						LOGF("DrawInputState[%u]: vb[%d].rec[%u].attr[%d] dst=v%d fmt=56 "
						     "rgba8=%02" PRIx32 "%02" PRIx32 "%02" PRIx32 "%02" PRIx32
						     " rgba=(%.3f,%.3f,%.3f,%.3f)\n",
						     log_id, bi, rec, ai, rd.register_start, r8, g8, b8, a8,
						     static_cast<double>(r8) / 255.0, static_cast<double>(g8) / 255.0,
						     static_cast<double>(b8) / 255.0, static_cast<double>(a8) / 255.0);
					}
				}
			}
		}

		for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
			const auto& r  = vs_input_info.resources[ai];
			const auto& rd = vs_input_info.resources_dst[ai];
			if (rd.buffer_index != bi) {
				continue;
			}
			LOGF("DrawInputState[%u]: attr[%d] offset=%u dst=v%d regs=%d fetch_index=%u "
			     "sharp=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			     log_id, ai, static_cast<uint32_t>(r.Base48() - b.addr), rd.register_start,
			     rd.registers_num, rd.fetch_index, r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
		}
	}
}

// Cmd is vk::CommandBuffer (record now) or MeshDynamicState (KYTY_MESH_BATCH capture).
template <typename Cmd>
static void SetGraphicsDynamicParams(const CommandBuffer& buffer, Cmd& vk_buffer,
                                     const ShaderVertexInputInfo& vs_input_info,
                                     const RenderDepthInfo& depth, const RenderState& rendering) {
	KYTY_PROFILER_FUNCTION();

	const auto& ctx = buffer.GetRegisters();
	const auto&        vp  = ctx.GetScreenViewport();
	const vk::Extent2D framebuffer_extent {rendering.width, rendering.height};
	const auto& outputs = vs_input_info.stage.program->info.outputs;
	const bool  indexed_viewports =
	    std::any_of(outputs.begin(), outputs.end(), [](const auto& output) {
		    return output.kind == ShaderRecompiler::IR::StageOutputKind::ViewportIndex;
	    });
	constexpr uint32_t viewport_slots = std::size(HW::ScreenViewport {}.viewports);
	static_assert(viewport_slots <= MeshDynamicState::MaxViewports);
	std::array<vk::Viewport, viewport_slots> viewports {};
	std::array<vk::Rect2D, viewport_slots>   scissors {};
	const uint32_t viewport_count = indexed_viewports ? viewport_slots : 1;
	for (uint32_t i = 0; i < viewport_count; i++) {
		const auto& guest    = vp.viewports[i];
		auto&       viewport = viewports[i];
		if (ctx.GetClipControl().clip_disable) {
			const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
			viewport.width  = static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u));
			viewport.height = static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u));
		} else {
			viewport.x      = guest.xoffset - guest.xscale;
			viewport.y      = guest.yoffset - guest.yscale;
		viewport.width  = guest.xscale * 2.0f;
		viewport.height = guest.yscale * 2.0f;
		if (([] { static const bool kyty_env_set = std::getenv("KYTY_FORCE_POSITIVE_VIEWPORT") != nullptr; return kyty_env_set; }()) && viewport.height < 0.0f) {
			viewport.y      = 0.0f;
			viewport.height = -viewport.height;
		}
		}
		viewport.minDepth =
		    guest.zoffset - (ctx.GetClipControl().dx_clip_space ? 0.0f : guest.zscale);
		viewport.maxDepth = guest.zscale + guest.zoffset;
		// Diagnostics: KYTY_CLAMP_VIEWPORT_DEPTH=1 clamps the depth range to Vulkan's legal [0,1].
		static const bool clamp_viewport_depth = std::getenv("KYTY_CLAMP_VIEWPORT_DEPTH") != nullptr;
		if (clamp_viewport_depth) {
			viewport.minDepth = std::clamp(viewport.minDepth, 0.0f, 1.0f);
			viewport.maxDepth = std::clamp(viewport.maxDepth, 0.0f, 1.0f);
		}
		// KYTY_INTERNAL_FRAME_VERTICES=1: log the Vulkan viewport incl. depth range (first 60).
		static const bool log_viewport = std::getenv("KYTY_INTERNAL_FRAME_VERTICES") != nullptr;
		static std::atomic<uint32_t> viewport_logs {0};
		if (log_viewport && i == 0 && viewport_logs.fetch_add(1) < 60) {
			LOGF("ViewportProbe: x=%.1f y=%.1f w=%.1f h=%.1f depth=[%.4f,%.4f] zscale=%.4f zoffset=%.4f "
			     "dx_clip_space=%d clip_disable=%d\n",
			     viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth,
			     viewport.maxDepth, guest.zscale, guest.zoffset,
			     ctx.GetClipControl().dx_clip_space ? 1 : 0, ctx.GetClipControl().clip_disable ? 1 : 0);
		}

		const auto final_scissor =
		    calc_final_scissor(vp, ctx.GetScanModeControl(), framebuffer_extent, i);
		auto& scissor  = scissors[i];
		scissor.offset = {final_scissor.left, final_scissor.top};
		scissor.extent = {static_cast<uint32_t>(final_scissor.right - final_scissor.left),
		                  static_cast<uint32_t>(final_scissor.bottom - final_scissor.top)};
		if (viewport.width == 0.0f) {
			// Keep empty slots at their guest index; Vulkan requires a positive viewport width.
			viewport.width = 1.0f;
			scissor.extent = {0, 0};
		}
	}
	vk_buffer.setViewportWithCount(viewport_count, viewports.data());
	vk_buffer.setScissorWithCount(viewport_count, scissors.data());

	float line_width = ctx.GetLineWidth();
	if (line_width != 1.0f) {
		static bool logged = false;
		if (!logged) {
			LOGF("Render: temporary: clamping Vulkan line width %f to 1.0 because wideLines is "
			     "not enabled\n",
			     line_width);
			logged = true;
		}
		line_width = 1.0f;
	}
	vk_buffer.setLineWidth(line_width);
	const auto&      blend = ctx.GetBlendColor();
	const std::array blend_constants {blend.red, blend.green, blend.blue, blend.alpha};
	vk_buffer.setBlendConstants(blend_constants.data());
	vk_buffer.setDepthTestEnable(depth.depth_test_enable ? VK_TRUE : VK_FALSE);
	vk_buffer.setDepthWriteEnable(depth.depth_write_enable ? VK_TRUE : VK_FALSE);
	vk_buffer.setDepthCompareOp(depth.depth_compare_op);
#if !defined(__APPLE__)
	vk_buffer.setDepthBoundsTestEnable(depth.depth_bounds_test_enable ? VK_TRUE : VK_FALSE);
	if (depth.depth_bounds_test_enable) {
		vk_buffer.setDepthBounds(ctx.GetDepthBoundsMin(), ctx.GetDepthBoundsMax());
	}
#endif

	const auto& mode              = ctx.GetModeControl();
	const auto& poly_offset       = ctx.GetPolyOffset();
	const bool  use_front         = mode.poly_offset_front_enable && !mode.cull_front;
	const bool  use_back          = mode.poly_offset_back_enable && !mode.cull_back;
	const bool  depth_bias_enable = use_front || use_back;
	vk_buffer.setDepthBiasEnable(depth_bias_enable ? VK_TRUE : VK_FALSE);
	if (depth_bias_enable) {
		// Vulkan has one bias for both faces. Prefer a visible front face when both are enabled.
		const float guest_constant_factor =
		    use_front ? poly_offset.front_offset : poly_offset.back_offset;
		const float constant_factor = ConvertPolygonOffsetConstantFactor(
		    guest_constant_factor, poly_offset, depth.desc.view_info.format);
		const float slope_factor =
		    (use_front ? poly_offset.front_scale : poly_offset.back_scale) / 16.0f;
		vk_buffer.setDepthBias(constant_factor, poly_offset.clamp, slope_factor);
	}

	vk_buffer.setStencilTestEnable(depth.stencil_test_enable ? VK_TRUE : VK_FALSE);
	if (depth.stencil_test_enable) {
		const auto set_stencil = [&](vk::StencilFaceFlagBits face, const vk::StencilOpState& state) {
			vk_buffer.setStencilOp(face, state.failOp, state.passOp, state.depthFailOp, state.compareOp);
			vk_buffer.setStencilCompareMask(face, state.compareMask);
			vk_buffer.setStencilWriteMask(face, state.writeMask);
			vk_buffer.setStencilReference(face, state.reference);
		};
		set_stencil(vk::StencilFaceFlagBits::eFront, depth.stencil_front);
		set_stencil(vk::StencilFaceFlagBits::eBack, depth.stencil_back);
	}

#if defined(__APPLE__)
	// MoltenVK has no VK_EXT_color_write_enable; the pipeline is created without the
	// eColorWriteEnableEXT dynamic state and relies on the static colorWriteMask instead.
#else
	vk::Bool32 enable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t slot = 0; slot < rendering.num_color_attachments; slot++) {
		enable[slot] = rendering.color_attachments[slot].image_view != nullptr;
	}
	if (rendering.num_color_attachments != 0) {
		vk_buffer.setColorWriteEnableEXT(rendering.num_color_attachments, enable);
	}
#endif
}

static bool DrawHasValidVertexShader(const HW::Shader& sh_ctx) {

	const auto& vs = sh_ctx.GetVs();
	return vs.es_regs.data_addr != 0;
}

static bool PixelShaderHasDepthOrCoverageSideEffects(const HW::ShaderRegisters& sh_regs) {
	const auto& db = sh_regs.db_shader_control;
	return db.shader_kill_enable || db.shader_z_export_enable || db.shader_mask_export_enable ||
	       db.shader_dual_export_enable || db.shader_execute_on_noop;
}

struct DrawRenderState {
	RenderDepthInfo       depth_info;
	RenderColorInfo       color_info[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	uint32_t              color_count                              = 0;
	bool                  ps_active                                = true;
	std::array<ShaderVertexInputInfo, 3> vertex_info;
	ShaderPixelInputInfo  ps_input_info;
	PipelineCache::GraphicsPrograms programs;
};

struct DrawCallInfo {
	CommandBufferDebugOp debug_op       = CommandBufferDebugOp::DrawIndex;
	uint32_t             index_count    = 0;
	uint32_t             instance_count = 0;
	uint32_t             first_instance = 0;

	[[nodiscard]] bool IsIndexed() const { return debug_op == CommandBufferDebugOp::DrawIndex; }
	[[nodiscard]] const char* Name() const { return IsIndexed() ? "DrawIndex" : "DrawIndexAuto"; }
};

RenderState RenderExecutor::AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
                                                 uint32_t color_count, RenderDepthInfo& depth,
                                                 vk::ImageAspectFlags& feedback_aspects,
                                                 std::span<PreparedBindings* const> stages) {
	EXIT_IF(colors == nullptr || color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	feedback_aspects = {};
	auto&       cache = m_context.GetTextureCache();
	RenderState state {};
	state.width                 = std::numeric_limits<uint32_t>::max();
	state.height                = std::numeric_limits<uint32_t>::max();
	state.num_layers            = std::numeric_limits<uint32_t>::max();
	state.num_color_attachments = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		auto& target = colors[i];
		EXIT_IF(!target.image_id);
		const auto owner = cache.m_slot_images.try_get(target.image_id);
		if (owner == nullptr || (!owner->registered && !owner->info.data.Empty()) ||
		    owner->binding.needs_rebind) {
			EXIT("color target changed after render-state discovery\n");
		}
		const auto image_view = cache.FindRenderTarget(target.image_id, target.desc);
		auto&      image      = cache.GetImage(target.image_id);
		EXIT_IF(image.backing.samples != target.desc.info.samples || image_view == nullptr);
		const auto& view   = target.desc.view_info;
		const auto  layout = image.binding.is_bound ? vk::ImageLayout::eGeneral
		                                            : vk::ImageLayout::eColorAttachmentOptimal;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access =
		    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
		// KYTY_RT_FEEDBACK_UNION=1 (opt-in): a color target that this draw also samples is
		// requested with the same access CommitBindings asks for (attachment + shader read
		// [+ write]). Otherwise the two requests differ only in their access masks, and every
		// such draw records a redundant image barrier that also ends the open render pass.
		static const bool feedback_union = [] {
			const char* v = std::getenv("KYTY_RT_FEEDBACK_UNION");
			return v != nullptr && *v != '\0' && *v != '0';
		}();
		auto target_access = image.binding.attachment_access;
		if (feedback_union && image.binding.is_bound) {
			target_access |= vk::AccessFlagBits2::eShaderRead |
			                 (image.binding.shader_write ? vk::AccessFlagBits2::eShaderWrite
			                                             : vk::AccessFlags2 {});
		}
		image.Transit(layout, target_access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.RawHandle());
		const auto extent       = target.Extent();
		state.width             = std::min(state.width, extent.width);
		state.height            = std::min(state.height, extent.height);
		state.num_layers        = std::min(state.num_layers, view.layer_count);
		state.num_color_attachments = std::max(state.num_color_attachments, target.target_slot + 1);
		auto& attachment            = state.color_attachments[target.target_slot];
		attachment.image_view   = image_view;
		attachment.image_layout = layout;
	}
	if (depth.image_id) {
		const auto owner = cache.m_slot_images.try_get(depth.image_id);
		if (owner == nullptr || !owner->registered || owner->binding.needs_rebind) {
			EXIT("depth target changed after render-state discovery\n");
		}
		const auto  image_view = cache.FindDepthTarget(depth.image_id, depth.desc);
		const auto& metadata   = depth.desc.info.metadata;
		if (metadata.kind == ImageMetadataKind::Htile && depth.depth_clear_enable &&
		    !cache.ClearMeta(metadata.range.address)) {
			EXIT("failed to acquire HTile metadata for a depth clear\n");
		}
		const bool meta_clear =
		    metadata.kind == ImageMetadataKind::Htile &&
		    cache.IsMetaCleared(metadata.range.address, depth.desc.view_info.base_layer);
		depth.depth_load_clear_enable = depth.depth_clear_enable || meta_clear;
		if (meta_clear &&
		    !cache.TouchMeta(metadata.range.address, depth.desc.view_info.base_layer, false)) {
			EXIT("failed to consume HTile clear state\n");
		}
		auto& image = cache.GetImage(depth.image_id);
		EXIT_IF(image_view == nullptr || image.backing.samples != depth.desc.info.samples);
		const auto draw_writes = depth.AttachmentWriteAspects();
		vk::ImageAspectFlags sampled_aspects;
		for (const auto* stage: stages) {
			for (const auto& binding: stage->images) {
				if (binding.image_id != depth.image_id ||
				    binding.desc.type != TextureCache::BindingType::Texture) continue;
				const auto native =
				    std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
				EXIT_IF(native == image.views.end());
				sampled_aspects |= native->info.aspect;
				feedback_aspects |= DepthFeedbackAspects(draw_writes, depth.desc.view_info,
				                                         native->info);
			}
		}
		if (feedback_aspects && !m_context.GetGraphics().attachment_feedback_loop_enabled) {
			EXIT("depth attachment feedback loop is not supported by the host\n");
		}
		auto layout = depth_attachment_layout(depth);
		if (sampled_aspects & ~DepthReadableAspects(layout)) {
			layout = m_context.GetGraphics().attachment_feedback_loop_enabled
			             ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
			             : vk::ImageLayout::eGeneral;
		}
		// The attachment store writes even when guest depth/stencil tests do not.
		const auto access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
		                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access = access;
		const auto& view                = depth.desc.view_info;
		// KYTY_DEPTH_READ_UNION=1: when this draw samples the depth target only through aspects
		// that its attachment layout keeps read-only (the layout was not widened for sampling, so
		// no draw in this layout can write a sampled aspect), request the access CommitBindings
		// is about to request for it (attachment + shader read). Baseline asks for the
		// attachment access first and then for attachment + shader read, so every such draw
		// records two image barriers on the same layout, and the first one ends the open render
		// pass, although no sampled aspect is written between consecutive such draws.
		// Only for a view of the whole image: CommitBindings transitions the whole image, so the
		// two requests are then the same request and the union never adds a barrier call.
		auto       request    = access;
		const bool whole_view = view.base_level == 0 && view.level_count == image.info.resources.levels &&
		                        view.base_layer == 0 && view.layer_count == image.info.resources.layers;
		if (MeshBatch::DepthReadUnionEnabled() && sampled_aspects && !feedback_aspects &&
		    whole_view && layout == depth_attachment_layout(depth) &&
		    !(sampled_aspects & ~DepthReadableAspects(layout)) && !image.binding.shader_write) {
			request |= vk::AccessFlagBits2::eShaderRead;
		}
		image.Transit(layout, request,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.RawHandle());
		state.width               = std::min(state.width, depth.desc.info.extent.width);
		state.height              = std::min(state.height, depth.desc.info.extent.height);
		state.num_layers          = std::min(state.num_layers, view.layer_count);
		const auto aspects        = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		auto&      attachment     = state.depth_stencil_attachment;
		attachment.image_view     = image_view;
		attachment.image_layout   = layout;
		attachment.clear_value[0] = std::bit_cast<uint32_t>(depth.depth_clear_value);
		attachment.clear_value[1] = depth.stencil_clear_value;
		attachment.has_depth      = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.depth_clear    = depth.depth_load_clear_enable;
		attachment.has_stencil    = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eStencil);
		attachment.stencil_clear  = depth.stencil_clear_enable;
	}
	if (color_count == 0 && !depth.image_id) {
		const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
		state.width        = limits.maxFramebufferWidth;
		state.height       = limits.maxFramebufferHeight;
	}
	if (state.num_layers == std::numeric_limits<uint32_t>::max()) {
		state.num_layers = 1;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.width == std::numeric_limits<uint32_t>::max() ||
	        state.height == std::numeric_limits<uint32_t>::max());
	return state;
}

static uint32_t DrawColorOutputMask(const HW::Context& ctx) {
	const auto& sh_regs     = ctx.GetShaderRegisters();
	const auto  write_mask  = ctx.GetRenderTargetMask() & sh_regs.m_cbShaderMask;
	uint32_t    output_mask = 0;
	for (uint32_t index = 0; index < RENDER_COLOR_ATTACHMENTS_MAX; index++) {
		const auto slot = ShaderPixelExportTarget(sh_regs.m_cbShaderMask, index);
		if (slot >= RENDER_COLOR_ATTACHMENTS_MAX) {
			break;
		}
		if (sh_regs.target_output_mode[index] != 0 &&
		    render_target_mask_slot(write_mask, slot) != 0) {
			output_mask |= 1u << slot;
		}
	}
	return output_mask;
}

enum class CbColorMode : uint8_t {
	Disable            = 0,
	Normal             = 1,
	EliminateFastClear = 2,
	Resolve            = 3,
	FmaskDecompress    = 5,
	DccDecompress      = 6,
};

static bool ConsumeMetadataColorOperation(const CommandBuffer& buffer) {
	const auto& ctx  = buffer.GetRegisters();
	const auto  mode = ctx.GetColorControl().mode;
	// These special modes run color-buffer metadata or decompression operations. The shader is a
	// vehicle for that operation, and its exported color must not be applied as a normal draw.
	// Kyty stores expanded Vulkan images rather than compressed guest surfaces, so no equivalent
	// hardware pass is emitted. Tracked DCC clear state is materialized on attachment bind;
	// future CMask/FMask support can consume its state through the same TextureCache path.
	return mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear) ||
	       mode == static_cast<uint8_t>(CbColorMode::FmaskDecompress) ||
	       mode == static_cast<uint8_t>(CbColorMode::DccDecompress);
}

struct DrawEmitInfo {
	int32_t  vertex_offset = 0;
	uint32_t first_vertex  = 0;
	uint32_t first_instance = 0;
};

struct DrawIndexBufferSource {
	uint64_t      address   = 0;
	const void*   host_data = nullptr;
	uint64_t      size      = 0;
	vk::IndexType type      = vk::IndexType::eUint16;
	uint32_t      guest_element_size = 0;
};

struct PreparedIndexBuffer {
	vk::Buffer     buffer = nullptr;
	vk::DeviceSize offset = 0;
	vk::IndexType  type   = vk::IndexType::eUint16;
};

static uint64_t VertexBufferDescriptorSize(int binding, const ShaderVertexInputInfo& info) {
	const auto& buffer = info.buffers[binding];
	if (buffer.stride != 0 || buffer.num_records == 0) {
		return static_cast<uint64_t>(buffer.stride) * buffer.num_records;
	}

	uint64_t size = 0;
	for (int i = 0; i < info.resources_num; i++) {
		if (info.resources_dst[i].buffer_index != binding) {
			continue;
		}
		const auto& resource = info.resources[i];
		// RDNA2 OOB_SELECT=2 only checks NumRecords != 0. A constant attribute still
		// fetches its entire format; NumRecords is not a byte count in this mode.
		const uint64_t extent = resource.OutOfBounds() == 2
		                            ? resource.Base48() - buffer.addr +
		                                  ShaderRecompiler::Format::GetFormatInfo(resource.Format()).byte_size
		                            : buffer.num_records;
		size = std::max(size, extent);
	}
	return size;
}

struct VertexBufferRange {
	uint64_t                     base_address  = 0;
	uint64_t                     requested_end = 0;
	uint64_t                     acquired_end  = 0;
	std::pair<Buffer*, uint64_t> binding;

	[[nodiscard]] uint64_t RequestedSize() const { return requested_end - base_address; }
};

struct PreparedVertexBuffers {
	static constexpr uint32_t MaxBuffers = ShaderVertexInputInfo::RES_MAX;

	std::array<vk::Buffer, MaxBuffers>     buffers {};
	std::array<vk::DeviceSize, MaxBuffers> offsets {};
	std::array<vk::DeviceSize, MaxBuffers> sizes {};
	uint32_t                               count = 0;
};

static PreparedVertexBuffers AcquireVertexBuffers(CommandBuffer&               buffer,
                                                  const ShaderVertexInputInfo& vs_input_info) {
	EXIT_IF(vs_input_info.buffers_num < 0 ||
	        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX);

	// Collect the non-empty guest vertex ranges.
	std::array<uint64_t, ShaderVertexInputInfo::RES_MAX>          sizes {};
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> ranges {};
	uint32_t                                                      range_count = 0;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = VertexBufferDescriptorSize(i, vs_input_info);
		sizes[i]           = size;
		if (size == 0) {
			continue;
		}
		if (vertex.addr == 0 || size > UINT64_MAX - vertex.addr) {
			EXIT("invalid vertex buffer range: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vertex.addr, size);
		}
		ranges[range_count++] = {vertex.addr, vertex.addr + size};
	}

	std::sort(ranges.begin(), ranges.begin() + range_count,
	          [](const VertexBufferRange& left, const VertexBufferRange& right) {
		          return left.base_address < right.base_address;
	          });

	// Merge overlapping or touching ranges before acquiring host buffers.
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> merged_ranges {};
	uint32_t                                                      merged_count = 0;
	for (uint32_t i = 0; i < range_count; i++) {
		const auto& range = ranges[i];
		if (merged_count != 0 &&
		    merged_ranges[merged_count - 1].requested_end >= range.base_address) {
			merged_ranges[merged_count - 1].requested_end =
			    std::max(merged_ranges[merged_count - 1].requested_end, range.requested_end);
			continue;
		}
		merged_ranges[merged_count++] = {range.base_address, range.requested_end};
	}

	auto& cache = buffer.GetContext().GetBufferCache();
	for (uint32_t i = 0; i < merged_count; i++) {
		auto& range = merged_ranges[i];
		// PPSA20298
		const auto size =
		    Libs::LibKernel::Memory::ClampRangeSize(range.base_address, range.RequestedSize());
		range.acquired_end = range.base_address + size;
		range.binding      = cache.ObtainBuffer(range.base_address, size, false);
	}

	// Rebuild slot bindings, offsetting non-empty slots into their acquired merged range.
	PreparedVertexBuffers prepared;
	prepared.count         = static_cast<uint32_t>(vs_input_info.buffers_num);
	vk::Buffer null_buffer = nullptr;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = sizes[i];
		if (size == 0) {
			if (null_buffer == nullptr) {
				null_buffer = cache.GetBuffer(NULL_BUFFER_ID).Handle();
			}
			prepared.buffers[i] = null_buffer;
			prepared.offsets[i] = 0;
			continue;
		}

		const auto range = std::find_if(merged_ranges.begin(), merged_ranges.begin() + merged_count,
		                                [&](const VertexBufferRange& value) {
			                                return vertex.addr >= value.base_address &&
			                                       vertex.addr < value.acquired_end;
		                                });
		if (range == merged_ranges.begin() + merged_count) {
			EXIT("vertex buffer address is outside the acquired range: addr=0x%016" PRIx64 "\n",
			     vertex.addr);
		}

		prepared.buffers[i] = range->binding.first->Handle();
		prepared.offsets[i] = range->binding.second + vertex.addr - range->base_address;
		prepared.sizes[i]   = std::min(size, range->acquired_end - vertex.addr);
	}

	return prepared;
}

static void SetDrawDebugPhase(CommandBuffer& buffer, uint64_t submit_id, const DrawCallInfo& draw,
                              uint32_t phase) {
	buffer.SetDebugInfo(static_cast<uint32_t>(draw.debug_op), submit_id, phase, draw.index_count, 0,
	                    draw.instance_count, draw.first_instance);
}

static bool GetDrawTopology(const HW::UserConfig& ucfg, vk::PrimitiveTopology& topology) {

	topology = vk::PrimitiveTopology::ePointList;

	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kNone: return false;
		case Prospero::PrimitiveType::kPointList:
			topology = vk::PrimitiveTopology::ePointList;
			break;
		case Prospero::PrimitiveType::kLineList: topology = vk::PrimitiveTopology::eLineList; break;
		case Prospero::PrimitiveType::kLineStrip:
			topology = vk::PrimitiveTopology::eLineStrip;
			break;
		case Prospero::PrimitiveType::kTriList:
			topology = vk::PrimitiveTopology::eTriangleList;
			break;
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kPolygon:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		case Prospero::PrimitiveType::kTriStrip:
			topology = vk::PrimitiveTopology::eTriangleStrip;
			break;
		case Prospero::PrimitiveType::kPatch:
			if (!Config::TessellationEnabled()) return false;
			[[fallthrough]];
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy:
			topology = vk::PrimitiveTopology::ePatchList;
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		default: {
			static std::atomic_bool logged = false;
			if (!logged.exchange(true, std::memory_order_relaxed)) {
				std::printf("Skipping draw with unknown primitive type: %u\n",
				            static_cast<uint32_t>(ucfg.GetPrimType()));
			}
			return false;
		}
	}

	return true;
}

static bool ResolvePrimitiveRestart(const CommandBuffer& buffer,
                                    const DrawIndexBufferSource& source, bool allow_scan = true) {
	const auto control = buffer.GetUserConfig().GetPrimitiveResetControl();
	EXIT_NOT_IMPLEMENTED((control & ~0x3u) != 0);
	if ((control & 0x1u) == 0) {
		return false;
	}
	switch (buffer.GetUserConfig().GetPrimType()) {
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip: break;
		default: return false;
	}

	const auto element_size = source.guest_element_size;
	const auto index_mask   = UINT32_MAX >> ((4 - element_size) * 8);
	const auto reset_index  = buffer.GetRegisters().GetPrimitiveResetIndex();
	if ((control & 0x2u) != 0 && (reset_index & ~index_mask) != 0) {
		return false;
	}
	const auto restart_index = reset_index & index_mask;
	if (restart_index == index_mask) {
		// Use native restart; the 8-bit path widens its marker to 0xffff.
		return true;
	}

	// A game can set a custom reset value without using it in the index buffer.
	// Keep restart off in that case; fail if we actually find the value.
	// Scan before preparing draw resources: readback can restart the command buffer.
	if (!allow_scan) {
		// KYTY_GPU_INDIRECT: the count is GPU-resident; the scan only ever EXITs or returns false.
		return false;
	}
	EXIT_NOT_IMPLEMENTED(source.address == 0);
	const auto* indices = reinterpret_cast<const uint8_t*>(source.address);
	for (uint64_t offset = 0; offset < source.size; offset += element_size) {
		uint32_t index = 0;
		std::memcpy(&index, indices + offset, element_size);
		EXIT_NOT_IMPLEMENTED(index == restart_index);
	}
	return false;
}

static void RefreshShaders(CommandBuffer& buffer, const DrawCallInfo& draw,
                           uint32_t color_output_mask, DrawRenderState& state) {
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	const auto& vertex_shader_info = sh_ctx.GetVs();
	const auto& pixel_shader_info  = sh_ctx.GetPs();
	const auto& shader_regs        = ctx.GetShaderRegisters();

	state.programs      = {};
	state.ps_input_info = {};
	std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX>
	    target_export_mapping {};
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		const auto& rt = ctx.GetRenderTarget(slot);
		if ((color_output_mask & (1u << slot)) != 0 && rt.base.addr != 0) {
			target_export_mapping[slot] =
			    TextureGetRenderTargetFormat(rt.info.format, rt.info.channel_type,
			                                 rt.info.channel_order)
			        .export_mapping;
		}
	}
	auto& pipeline_cache = buffer.GetContext().GetPipelineCache();
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "GetGraphicsPrograms");
	}
	state.programs = pipeline_cache.GetGraphicsPrograms(
	    vertex_shader_info, pixel_shader_info, shader_regs, ctx, buffer.GetUserConfig(),
	    target_export_mapping, state.ps_active, state.vertex_info, state.ps_input_info);
}

bool RenderExecutor::PrepareDrawRenderState(CommandBuffer& buffer, const DrawCallInfo& draw,
                                            uint32_t            render_target_slice_offset,
	                                        DrawRenderState& state) {
	const auto& shader_regs       = buffer.GetRegisters().GetShaderRegisters();
	const auto  color_output_mask = DrawColorOutputMask(buffer.GetRegisters());
	state.ps_active = buffer.GetShaders().GetPs().ps_regs.data_addr != 0 &&
	                  (color_output_mask != 0 ||
	                   PixelShaderHasDepthOrCoverageSideEffects(shader_regs));
	RefreshShaders(buffer, draw, color_output_mask, state);
	uint32_t mrt_mask = 0;
	if (state.ps_active) {
		for (const auto& output: state.ps_input_info.stage.program->info.outputs) {
			if (output.kind == ShaderRecompiler::IR::StageOutputKind::Mrt) {
				mrt_mask |= 1u << output.location;
			}
		}
	}
	mrt_mask &= color_output_mask;
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "ResolveRenderColorTarget");
	}
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if ((mrt_mask & (1u << slot)) != 0) {
			ResolveRenderColorTarget(buffer, state.color_info[state.color_count],
			                         render_target_slice_offset, slot);
			if (state.color_info[state.color_count].image_id) {
				state.color_count++;
			}
		}
	}
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "ResolveRenderDepthTarget");
	}
	ResolveRenderDepthTarget(buffer, state.depth_info);

	if (state.color_count == 0 && !state.depth_info.image_id && !state.ps_active) {
		LogFramebufferSkip(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   draw.index_count, 0);
		return false;
	}

	return true;
}

static PreparedIndexBuffer PrepareIndexBuffer(CommandBuffer&               buffer,
                                              const DrawIndexBufferSource& source) {
	PreparedIndexBuffer prepared;
	if (source.size == 0) {
		return prepared;
	}
	prepared.type = source.type;
	if (source.host_data != nullptr) {
		auto& stream = buffer.GetContext().GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		prepared.offset = stream.Copy(source.host_data, source.size, 16);
		prepared.buffer = stream.Handle();
	} else {
		auto [buffer_ptr, offset] =
		    buffer.GetContext().GetBufferCache().ObtainBuffer(source.address, source.size, false);
		prepared.buffer = buffer_ptr->Handle();
		prepared.offset = offset;
	}
	return prepared;
}

static void CommitVertexBuffers(vk::CommandBuffer            vk_buffer,
                                const PreparedVertexBuffers& prepared) {
	for (uint32_t i = 0; i < prepared.count; i++) {
		EXIT_IF(prepared.buffers[i] == nullptr);
	}
	if (prepared.count != 0) {
		// Guest descriptor bounds must survive allocation merging in the cache.
		vk_buffer.bindVertexBuffers2(0, prepared.count, prepared.buffers.data(),
		                             prepared.offsets.data(), prepared.sizes.data(), nullptr);
	}
}

static void CommitIndexBuffer(vk::CommandBuffer vk_buffer, const PreparedIndexBuffer& prepared) {
	if (prepared.buffer == nullptr) {
		return;
	}
	vk_buffer.bindIndexBuffer(prepared.buffer, prepared.offset, prepared.type);
}

static void LogDrawStateIfNeeded(const CommandBuffer& buffer, const DrawCallInfo& draw,
	                             const DrawRenderState& state, uint32_t index_type_and_size,
                                 const void* index_addr) {
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	if (!draw.IsIndexed() && !Prospero::IsRectList(buffer.GetUserConfig().GetPrimType()) &&
	    !([] { static const bool kyty_env_set = std::getenv("KYTY_DRAW_INPUT_STATS") != nullptr; return kyty_env_set; }())) {
		return;
	}

	if (state.ps_active) {
		LogDrawTargetState(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   state.ps_input_info, draw.index_count, 0);
	}
	LogDrawInputState(buffer, state.color_info[0], state.vertex_info[0], index_type_and_size,
	                  draw.index_count, index_addr);
}

static void EmitDrawPrimitives(const HW::UserConfig& ucfg, vk::CommandBuffer vk_buffer,
                               const DrawCallInfo& draw, const DrawEmitInfo& emit) {
	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip:
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kPolygon:
		case Prospero::PrimitiveType::kRectListLegacy:
		case Prospero::PrimitiveType::kPatch:
			if (draw.IsIndexed()) {
				vk_buffer.drawIndexed(draw.index_count, draw.instance_count, 0, emit.vertex_offset,
				                      emit.first_instance);
			} else {
				vk_buffer.draw(draw.index_count, draw.instance_count, emit.first_vertex,
				               emit.first_instance);
			}
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			EXIT_NOT_IMPLEMENTED((draw.index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < draw.index_count; i += 4) {
				if (draw.IsIndexed()) {
					vk_buffer.drawIndexed(4, draw.instance_count, i, emit.vertex_offset,
					                      emit.first_instance);
				} else {
					vk_buffer.draw(4, draw.instance_count, i + emit.first_vertex,
					               emit.first_instance);
				}
			}
			break;
		default: EXIT("unknown primitive type: %u\n", static_cast<uint32_t>(ucfg.GetPrimType()));
	}
}

void RenderExecutor::ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer,
                                         const DrawCallInfo& draw, DrawRenderState& state,
                                         vk::PrimitiveTopology topology, const DrawEmitInfo& emit,
                                         const DrawIndexBufferSource& index_source,
	                                     bool primitive_restart_enable) {
	auto& ucfg = buffer.GetUserConfig();
	const auto vertex_stages =
	    std::span {state.vertex_info.data(), state.programs.VertexStageCount()};
	const bool mesh_active = state.vertex_info[0].stage.program->stage == ShaderType::Mesh;
	uint32_t   mesh_groups = 0;
	if (mesh_active) {
		const auto& mesh = state.vertex_info[0].mesh;
		EXIT_NOT_IMPLEMENTED(mesh.fast_launch && (draw.IsIndexed() || primitive_restart_enable));
		static std::atomic_bool restart_warned = false;
		if (primitive_restart_enable && !restart_warned.exchange(true, std::memory_order_relaxed)) {
			std::printf("Warning: primitive restart is not implemented for mesh shaders; "
			            "continuing draw (primitive=%u indexed=%u)\n",
			            static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed());
		}
		if (mesh.primitives_per_group == 0) {
			EXIT("unsupported mesh draw: primitive=%u indexed=%u restart=%u\n",
			     static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed(), primitive_restart_enable);
		}
		const auto primitives = mesh.InputPrimitiveCount(draw.index_count);
		if (primitives == 0 || draw.instance_count == 0) {
			return;
		}
		mesh_groups        = (primitives - 1u) / mesh.primitives_per_group + 1u;
		const auto& limits = m_context.GetGraphics().mesh_shader_properties;
		if (m_context.GetGraphics().mesh_emulation) {
			const auto& compute = m_context.GetGraphics().GetPhysicalDeviceProperties().limits;
			if (mesh_groups > compute.maxComputeWorkGroupCount[0] ||
			    draw.instance_count > compute.maxComputeWorkGroupCount[1]) {
				EXIT("emulated mesh draw exceeds compute workgroup limits: %ux%u\n", mesh_groups,
				     draw.instance_count);
			}
		} else if (mesh_groups > limits.maxMeshWorkGroupCount[0] ||
		    draw.instance_count > limits.maxMeshWorkGroupCount[1] ||
		    static_cast<uint64_t>(mesh_groups) * draw.instance_count >
		        limits.maxMeshWorkGroupTotalCount) {
			EXIT("mesh draw exceeds host workgroup limits: %ux%u\n", mesh_groups,
			     draw.instance_count);
		}
	}

	if (mesh_active && draw.IsIndexed()) {
		// Register the original guest indices for shader reads; PrepareGraphicsBindings
		// synchronizes registered BDA ranges before any draw commands are committed.
		(void)m_context.GetBufferCache().FindBuffer(
		    index_source.address, static_cast<uint64_t>(draw.index_count) *
		                              index_source.guest_element_size);
	}
	LogDrawPhase(draw.Name(), "PrepareBindings");
	auto&                            bindings = m_graphics_bindings;
	std::array<PreparedBindings*, 4> descriptor_stages {};
	uint32_t                         stage_count = 0;
	for (uint32_t i = 0; i < vertex_stages.size(); i++) {
		PrepareBindings(state.vertex_info[i].stage, bindings.vertex[i]);
		descriptor_stages[stage_count++] = &bindings.vertex[i];
	}
	if (state.ps_active) {
		if (!bindings.pixel) bindings.pixel.emplace();
		PrepareBindings(state.ps_input_info.stage, *bindings.pixel);
		descriptor_stages[stage_count++] = &*bindings.pixel;
	}
	const auto stages = std::span {descriptor_stages.data(), stage_count};
	PrepareGraphicsBindings(stages, std::span {state.color_info, state.color_count});
	PreparedVertexBuffers vertex_bindings;
	PreparedIndexBuffer   index_binding;
	if (!mesh_active) {
		LogDrawPhase(draw.Name(), "PrepareVertexBuffers");
		vertex_bindings = AcquireVertexBuffers(buffer, state.vertex_info[0]);
		index_binding   = PrepareIndexBuffer(buffer, index_source);
	}
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "CreatePipeline");
	}
	auto& pipeline = m_context.GetPipelineCache().GetGraphicsPipeline(
	    std::span {state.color_info, state.color_count}, state.depth_info, vertex_stages, buffer,
	    state.ps_active ? &state.ps_input_info : nullptr, topology, primitive_restart_enable,
	    state.programs);
	vk::ImageAspectFlags feedback_aspects;
	const auto rendering =
	    AcquireRenderTargets(buffer, state.color_info, state.color_count, state.depth_info,
	                         feedback_aspects, stages);

	// Resource preparation above may synchronously finish and restart the scheduler. From this
	// point onward, every operation targets the current command buffer and cannot touch guest
	// memory.
	//
	// KYTY_MESH_BATCH: a pending batch of captured mesh draws is replayed here unless this draw
	// joins it; everything recorded before this point went through EndRendering/Handle, which
	// replay it first.
	const bool mesh_emulated = mesh_active && pipeline.mesh_compute != nullptr;
	uint64_t   mesh_batch_bytes = 0;
	const bool mesh_batched =
	    mesh_emulated && MeshBatch::Enabled() &&
	    MeshDrawBatchable(draw, state, stages, mesh_groups, mesh_batch_bytes);
	if (!mesh_batched) {
		FlushMeshBatch(buffer, MeshBatch::FlushReason::Draw);
	}
	auto vk_buffer = buffer.RawHandle();
	SetDrawDebugPhase(buffer, submit_id, draw, draw.IsIndexed() ? 0x100u : 0x200u);
	if (!mesh_active) {
		CommitVertexBuffers(vk_buffer, vertex_bindings);
	}
	if (state.ps_active && !draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x300u);
	}
	if (mesh_emulated) {
		const uint32_t draw_data[] {
		    draw.index_count,
		    draw.IsIndexed() ? static_cast<uint32_t>(emit.vertex_offset) : emit.first_vertex,
		    emit.first_instance, index_source.guest_element_size,
		    static_cast<uint32_t>(index_source.address),
		    static_cast<uint32_t>(index_source.address >> 32u), 0u, 0u, 0u, 0u};
		static_assert(std::size(draw_data) == ShaderRecompiler::IR::PushData::MeshDrawDwordCount);
		if (mesh_batched) {
			ExecuteBatchedMeshDraw(buffer, draw, state, pipeline, stages, rendering,
			                       feedback_aspects, draw_data, mesh_groups, mesh_batch_bytes);
		} else {
			ExecuteEmulatedMeshDraw(buffer, draw, state, pipeline, stages, rendering,
			                        feedback_aspects, draw_data, mesh_groups);
		}
	} else {
	CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline, stages);
	if (mesh_active) {
		const uint32_t draw_data[] {
		    draw.index_count,
		    draw.IsIndexed() ? static_cast<uint32_t>(emit.vertex_offset) : emit.first_vertex,
		    emit.first_instance, index_source.guest_element_size,
		    static_cast<uint32_t>(index_source.address),
		    static_cast<uint32_t>(index_source.address >> 32u), 0u, 0u, 0u, 0u};
		static_assert(std::size(draw_data) == ShaderRecompiler::IR::PushData::MeshDrawDwordCount);
		vk_buffer.pushConstants(pipeline.pipeline_layout,
		                        vk::ShaderStageFlagBits::eMeshEXT |
		                            vk::ShaderStageFlagBits::eFragment,
		                        0, sizeof(draw_data), draw_data);
	} else {
		CommitIndexBuffer(vk_buffer, index_binding);
	}

	SetGraphicsDynamicParams(buffer, vk_buffer, vertex_stages.back(), state.depth_info, rendering);
	if (m_context.GetGraphics().attachment_feedback_loop_enabled) {
		vk_buffer.setAttachmentFeedbackLoopEnableEXT(feedback_aspects);
	}

	LogDrawPhase(draw.Name(), "BeginRendering");
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x400u);
	}
	m_context.GetCommandScheduler().BeginRendering(rendering);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x500u);
	}
	if (mesh_active) {
		vk_buffer.drawMeshTasksEXT(mesh_groups, draw.instance_count, 1);
	} else {
		EmitDrawPrimitives(ucfg, vk_buffer, draw, emit);
	}
	} // !mesh_emulated

	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x600u);
	}
	vk::PipelineStageFlags shader_write_stages = {};
	for (const auto& stage: vertex_stages) {
		if (HasShaderBufferWrites(stage.stage)) {
			shader_write_stages |= ShaderPipelineStages(NativeShaderStage(stage.logical_stage));
		}
	}
	if (state.ps_active && HasShaderBufferWrites(state.ps_input_info.stage)) {
		shader_write_stages |= vk::PipelineStageFlagBits::eFragmentShader;
	}
	if (shader_write_stages) {
		m_context.GetCommandScheduler().EndRendering();
		ShaderWriteBarrier(vk_buffer, shader_write_stages);
	}
	LogDrawPhase(draw.Name(), "DrawComplete");
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x700u);
	}
	// KYTY_INTERNAL_FRAME_VERTICES=1: log the topology and the vertices this draw really fetches.
	static const bool log_vertices = std::getenv("KYTY_INTERNAL_FRAME_VERTICES") != nullptr;
	if (log_vertices && state.ps_active && state.vertex_info[0].buffers_num > 0) {
		const auto& vb = state.vertex_info[0].buffers[0];
		LOGF("DrawVertexProbe: ps=0x%016" PRIx64 " prim=%u indexed=%d count=%u instances=%u "
		     "first_vertex=%u vertex_offset=%d first_instance=%u vb=0x%010" PRIx64
		     " stride=%u records=%u\n",
		     state.ps_input_info.stage.program->shader_hash,
		     static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed() ? 1 : 0, draw.index_count,
		     draw.instance_count, emit.first_vertex, emit.vertex_offset, emit.first_instance, vb.addr,
		     vb.stride, vb.num_records);
		const auto* bytes = reinterpret_cast<const uint8_t*>(vb.addr);
		if (bytes != nullptr && vb.stride >= 20) {
			const uint32_t shown = std::min<uint32_t>(std::max(draw.index_count, 1u), 6u);
			for (uint32_t v = 0; v < shown; ++v) {
				const uint64_t index = static_cast<uint64_t>(emit.first_vertex) + v;
				if (index >= vb.num_records) break;
				float f[4]; uint32_t color = 0;
				std::memcpy(f, bytes + index * vb.stride, sizeof(f));
				std::memcpy(&color, bytes + index * vb.stride + 16, sizeof(color));
				LOGF("DrawVertexProbe:   v%u (x=%.2f y=%.2f u=%.2f v=%.2f) color=0x%08x\n", v, f[0],
				     f[1], f[2], f[3], color);
			}
		}
	}
	// KYTY_VERTEX_GPU_COMPARE=1: read the vertex data the GPU really fetched (host buffer) back and
	// compare it with guest memory, to detect stale/missed CPU->GPU uploads.
	static const bool compare_gpu_vertices = std::getenv("KYTY_VERTEX_GPU_COMPARE") != nullptr;
	static uint32_t   compare_count        = 0;
	static const int compare_min_frame = [] {
		const char* value = std::getenv("KYTY_VERTEX_GPU_COMPARE_MIN_FRAME");
		return value != nullptr ? std::atoi(value) : 0;
	}();
	if (compare_gpu_vertices && !mesh_active && state.vertex_info[0].buffers_num > 0 &&
	    vertex_bindings.buffers[0] != nullptr && compare_count < 400 &&
	    buffer.GetContext().GetGpu().GetFrameNum() >= compare_min_frame) {
		const auto& vb = state.vertex_info[0].buffers[0];
		if (vb.addr != 0 && vb.stride >= 20 && vb.stride <= 256) {
			const uint64_t first = static_cast<uint64_t>(emit.first_vertex);
			const uint32_t shown = std::min<uint32_t>(std::max(draw.index_count, 1u), 4u);
			const uint64_t bytes = static_cast<uint64_t>(shown) * vb.stride;
			if (first + shown <= vb.num_records) {
				auto& scheduler = m_context.GetCommandScheduler();
				scheduler.EndRendering();
				auto& dl = m_context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
				auto [mapped, offset] = dl.Map(bytes, 4);
				if (mapped != nullptr) {
					dl.Commit();
					vk::BufferCopy copy {vertex_bindings.offsets[0] + first * vb.stride, offset, bytes};
					scheduler.Current().Handle().copyBuffer(vertex_bindings.buffers[0], dl.Handle(), copy);
					vk::BufferMemoryBarrier barrier {};
					barrier.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
					barrier.dstAccessMask       = vk::AccessFlagBits::eHostRead;
					barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					barrier.buffer              = dl.Handle();
					barrier.offset              = offset;
					barrier.size                = bytes;
					scheduler.Current().Handle().pipelineBarrier(
					    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, 0,
					    nullptr, 1, &barrier, 0, nullptr);
					scheduler.FlushAndWait();
					dl.Invalidate(offset, bytes);
					const auto* gpu   = static_cast<const uint8_t*>(mapped);
					const auto* guest = reinterpret_cast<const uint8_t*>(vb.addr) + first * vb.stride;
					const bool  match = std::memcmp(gpu, guest, bytes) == 0;
					float g[4] {}, h[4] {};
					std::memcpy(g, gpu, sizeof(g));
					std::memcpy(h, guest, sizeof(h));
					++compare_count;
					LOGF("VertexGpuCompare: ps=0x%016" PRIx64 " first=%u count=%u %s gpu_v0=(%.2f,%.2f,%.2f,%.2f)"
					     " guest_v0=(%.2f,%.2f,%.2f,%.2f)\n",
					     state.ps_active ? state.ps_input_info.stage.program->shader_hash : 0,
					     emit.first_vertex, draw.index_count, match ? "MATCH" : "MISMATCH", g[0], g[1],
					     g[2], g[3], h[0], h[1], h[2], h[3]);
				}
			}
		}
	}
	// KYTY_DEBUG_FORCE_EXPOSURE=1: also force a pixel-shader-written 1x1 RGBA32F render target
	// (Unreal eye adaptation, e.g. 0x8047290000) to (1,1,1,1) after the draw.
	static const bool force_exposure_rt = std::getenv("KYTY_DEBUG_FORCE_EXPOSURE") != nullptr;
	if (force_exposure_rt) {
		for (uint32_t i = 0; i < state.color_count; ++i) {
			const auto id = state.color_info[i].image_id;
			if (id) {
				m_context.GetCommandScheduler().EndRendering();
				m_context.GetTextureCache().DebugForceExposure(id);
			}
		}
	}
	for (uint32_t i = 0; i < state.color_count && InternalFrameStatsFrameReached(buffer); ++i) {
		const auto& color = state.color_info[i];
		m_context.GetTextureCache().LogInternalFrameStats(
		    color.image_id, color.desc.view_info.base_level, color.desc.view_info.base_layer,
		    state.vertex_info[0].stage.program->shader_hash,
		    state.ps_active ? state.ps_input_info.stage.program->shader_hash : 0);
	}
	// KYTY_INTERNAL_FRAME_SAMPLED=1: also read back every texture the pixel shader samples.
	static const bool log_sampled = std::getenv("KYTY_INTERNAL_FRAME_SAMPLED") != nullptr;
	if (log_sampled && state.ps_active && bindings.pixel && InternalFrameStatsFrameReached(buffer)) {
		for (const auto& texture: bindings.pixel->images) {
			m_context.GetTextureCache().LogInternalFrameStats(
			    texture.image_id, texture.desc.view_info.base_level,
			    texture.desc.view_info.base_layer, state.vertex_info[0].stage.program->shader_hash,
			    state.ps_input_info.stage.program->shader_hash, "sampled");
		}
	}
}

void RenderExecutor::DrawIndex(uint64_t submit_id, CommandBuffer& buffer,
                               const DrawIndexArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndex), submit_id,
	                    args.index_count, 0, 1, args.instance_count,
	                    reinterpret_cast<uint64_t>(args.index_addr));

	Common::LockGuard lock(m_context.GetMutex());
	if (args.index_count == 0 || args.instance_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(buffer) || DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer, args.render_target_slice_offset)) {
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndex():Shader:\n");
		uc_print("GraphicsRenderDrawIndex():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndex():Parameters:\n"
		     "\t index_type_and_size = 0x%08" PRIx32 "\n"
		     "\t index_count         = 0x%08" PRIx32 "\n"
		     "\t index_addr          = 0x%016" PRIx64 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t base_vertex         = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.index_type_and_size, args.index_count,
		     reinterpret_cast<uint64_t>(args.index_addr), args.instance_count,
		     static_cast<uint32_t>(args.base_vertex), args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
		return;
	}

	DrawIndexBufferSource index_source {};
	index_source.address = reinterpret_cast<uint64_t>(args.index_addr);
	switch (static_cast<Prospero::IndexType>(args.index_type_and_size)) {
		case Prospero::IndexType::kIndex16:
			index_source.type               = vk::IndexType::eUint16;
			index_source.guest_element_size = 2;
			break;
		case Prospero::IndexType::kIndex32:
			index_source.type               = vk::IndexType::eUint32;
			index_source.guest_element_size = 4;
			break;
		case Prospero::IndexType::kIndex8:
			index_source.guest_element_size = 1;
			break;
		default: EXIT("unknown index_type_and_size: %u\n", args.index_type_and_size);
	}
	index_source.size = static_cast<uint64_t>(args.index_count) * index_source.guest_element_size;
	const bool primitive_restart = ResolvePrimitiveRestart(buffer, index_source);

	std::vector<uint16_t> expanded_indices;
	if (index_source.guest_element_size == 1) {
		EXIT_NOT_IMPLEMENTED(args.index_addr == nullptr);
		const auto* src = static_cast<const uint8_t*>(args.index_addr);
		expanded_indices.resize(args.index_count);
		for (uint32_t i = 0; i < args.index_count; i++) {
			expanded_indices[i] = primitive_restart && src[i] == 0xffu ? 0xffffu : src[i];
		}
		index_source.host_data = expanded_indices.data();
		index_source.size      = expanded_indices.size() * sizeof(uint16_t);
	}

	const DrawCallInfo draw {CommandBufferDebugOp::DrawIndex, args.index_count,
	                        args.instance_count, args.first_instance};
	DrawRenderState state {};
	if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state)) {
		ResetBindings();
		return;
	}
	if (m_gpu_pending_mask != 0) {
		// KYTY_GPU_INDIRECT: this program reads a GS user SGPR that only a GPU-converted indirect
		// draw knows. Fetch it (may drain), then re-derive the state from the real value.
		if (const auto slots = GpuPendingSlotsReadBy(buffer, state); slots != 0) {
			ResolveGpuPendingRegisters(buffer, slots);
			state = {};
			if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state)) {
				ResetBindings();
				return;
			}
		}
	}

	LogDrawStateIfNeeded(buffer, draw, state, args.index_type_and_size,
	                     args.index_addr);

	const bool indirect = args.offset_source == DrawOffsetSource::IndirectArgs;
	const auto [vertex_offset, instance_offset] =
	    indirect ? std::pair<int32_t, uint32_t> {0, args.first_instance}
	             : ResolveDrawOffsets(ucfg.GetIndexOffset(), state.vertex_info[0]);

	DrawEmitInfo emit {};
	// Native fetches consume the patched SGPRs; rewritten fetches need Vulkan offsets.
	const bool native_indirect = indirect && !state.vertex_info[0].fetch_embedded;
	emit.vertex_offset  = native_indirect ? 0 : vertex_offset + args.base_vertex;
	emit.first_instance = native_indirect ? 0 : instance_offset;

	ExecutePreparedDraw(submit_id, buffer, draw, state, topology, emit, index_source,
	                    primitive_restart);
	ResetBindings();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RenderExecutor::DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndexAuto), submit_id,
	                    args.vertex_count, 0, args.first_vertex, args.instance_count,
	                    args.first_instance);

	Common::LockGuard lock(m_context.GetMutex());
	if (args.vertex_count == 0 || args.instance_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(buffer) || DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer, args.render_target_slice_offset)) {
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndexAuto():Shader:\n");
		uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndexAuto():Parameters:\n"
		     "\t vertex_count        = 0x%08" PRIx32 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t first_vertex        = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.vertex_count, args.instance_count, args.first_vertex, args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	const DrawCallInfo draw {CommandBufferDebugOp::DrawIndexAuto,
	                         args.vertex_count, args.instance_count, args.first_instance};

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
		ResetBindings();
		return;
	}
	DrawRenderState state {};
	if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state)) {
		ResetBindings();
		return;
	}
	if (m_gpu_pending_mask != 0) {
		// KYTY_GPU_INDIRECT: see DrawIndex.
		if (const auto slots = GpuPendingSlotsReadBy(buffer, state); slots != 0) {
			ResolveGpuPendingRegisters(buffer, slots);
			state = {};
			if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state)) {
				ResetBindings();
				return;
			}
		}
	}

	const bool rect_list = Prospero::IsRectList(ucfg.GetPrimType());
	if (rect_list && state.vertex_info[0].buffers_num == 0 &&
	    state.vertex_info[0].stage.program->param_export_mask == 0 &&
	    state.ps_input_info.input_num != 0) {
		if (graphics_debug_dump_enabled()) {
			LOGF("DrawIndexAuto: skipping rect-list draw with no VS param exports and PS inputs: "
			     "ps_inputs=%u ps=0x%016" PRIx64 " es=0x%016" PRIx64 " gs=0x%016" PRIx64 "\n",
			     state.ps_input_info.input_num, sh_ctx.GetPs().ps_regs.data_addr,
			     sh_ctx.GetVs().es_regs.data_addr, sh_ctx.GetVs().gs_regs.data_addr);
		}
		ResetBindings();
		return;
	}

	LogDrawStateIfNeeded(buffer, draw, state, 0, nullptr);

	const bool indirect = args.offset_source == DrawOffsetSource::IndirectArgs;
	const auto [vertex_offset, instance_offset] =
	    indirect ? std::pair<int32_t, uint32_t> {0, args.first_instance}
	             : ResolveDrawOffsets(ucfg.GetIndexOffset(), state.vertex_info[0]);
	DrawEmitInfo emit {};
	const bool native_indirect = indirect && !state.vertex_info[0].fetch_embedded;
	emit.first_vertex = native_indirect ? 0 :
	    static_cast<uint32_t>(vertex_offset + static_cast<int32_t>(args.first_vertex));
	emit.first_instance = native_indirect ? 0 : instance_offset;

	DrawIndexBufferSource index_source {};
	ExecutePreparedDraw(submit_id, buffer, draw, state, topology, emit, index_source, false);
	ResetBindings();
}

bool RenderExecutor::ResolveColorTargets(CommandBuffer& buffer, uint32_t render_target_slice_offset) {
	const auto& hw = buffer.GetRegisters();
	if (hw.GetColorControl().mode != 3) {
		return false;
	}

	const auto& src_rt = hw.GetRenderTarget(0);
	const auto& dst_rt = hw.GetRenderTarget(1);
	if (src_rt.base.addr == 0 || dst_rt.base.addr == 0) {
		return false;
	}

	RenderColorInfo src {};
	RenderColorInfo dst {};
	ResolveRenderColorTarget(buffer, src, render_target_slice_offset, 0, true, true);
	ResolveRenderColorTarget(buffer, dst, render_target_slice_offset, 1, true, true);
	if (!src.image_id || !dst.image_id) {
		return false;
	}
	if (src.desc.info.data.address == dst.desc.info.data.address &&
	    src.guest_mip_level == dst.guest_mip_level &&
	    src.guest_array_layer == dst.guest_array_layer) {
		return true;
	}

	auto& cache = m_context.GetTextureCache();
	cache.MarkGpuWritten(dst.image_id);
	auto& source      = cache.GetImage(src.image_id);
	auto& destination = cache.GetImage(dst.image_id);
	destination.Resolve(source, {src.guest_mip_level, 1, src.guest_array_layer, 1},
	                    {dst.guest_mip_level, 1, dst.guest_array_layer, 1});
	return true;
}


namespace {

// KYTY_GPU_INDIRECT constants.
constexpr uint64_t GpuIndirectTailBytes     = 256;  // param block at the end of the mesh scratch
constexpr uint32_t GpuIndirectSlots         = 4096; // command + history ring entries
constexpr uint32_t GpuIndirectSlotBytes     = 32;
constexpr uint32_t GpuIndirectHistoryHeader = 16;   // [0] status!=ok count, [1] oob count
constexpr uint32_t GpuIndirectPushDwords    = 17;

static_assert(MeshEmulation::ParamBlockDwords * 4u <= GpuIndirectTailBytes);

uint64_t MeshScratchItemBytes(const Buffer& scratch) {
	return MeshEmulation::GpuIndirectEnabled() ? scratch.Size() - GpuIndirectTailBytes
	                                          : scratch.Size();
}

enum class GpuIndirectReason : uint32_t {
	Gpu,
	NoMeshEmulation,
	SpecialColorMode,
	NoVertexShader,
	Topology,
	NoIndexBufferSize,
	NoTargets,
	NotMeshProgram,
	MeshShape,
	RectListSkip,
	PendingRead,
	UserDataLayout,
	Count,
};

constexpr const char* GpuIndirectReasonName(GpuIndirectReason reason) {
	switch (reason) {
		case GpuIndirectReason::Gpu: return "gpu";
		case GpuIndirectReason::NoMeshEmulation: return "no_mesh_emu";
		case GpuIndirectReason::SpecialColorMode: return "special_cb";
		case GpuIndirectReason::NoVertexShader: return "no_vs";
		case GpuIndirectReason::Topology: return "topology";
		case GpuIndirectReason::NoIndexBufferSize: return "no_ib_size";
		case GpuIndirectReason::NoTargets: return "no_targets";
		case GpuIndirectReason::NotMeshProgram: return "not_mesh";
		case GpuIndirectReason::MeshShape: return "mesh_shape";
		case GpuIndirectReason::RectListSkip: return "rect_skip";
		case GpuIndirectReason::PendingRead: return "pending_read";
		case GpuIndirectReason::UserDataLayout: return "ud_layout";
		default: return "?";
	}
}

struct GpuIndirectStats {
	std::array<uint64_t, static_cast<size_t>(GpuIndirectReason::Count)> draws {};
	uint64_t resolve_regs      = 0; // lazy CPU reads of GPU-resident registers (each may drain)
	uint64_t resolve_drains    = 0; // ... that had to submit/wait
	uint64_t resolve_wait_us   = 0;
	uint64_t resolve_seq_miss  = 0; // history slot recycled: fell back to the guest memory read
	uint64_t cpu_multi         = 0; // multi-draw / count-indirect packets (CPU path)
	uint64_t cpu_dest          = 0; // non-GS patch destination / unaligned record (CPU path)
	std::chrono::steady_clock::time_point last_log = std::chrono::steady_clock::now();
};

GpuIndirectStats g_gpu_indirect_stats; // GPU thread only

uint64_t& GpuIndirectCount(GpuIndirectReason reason) {
	return g_gpu_indirect_stats.draws[static_cast<size_t>(reason)];
}

} // namespace

void GpuIndirectNoteCpuMulti() {
	g_gpu_indirect_stats.cpu_multi++;
}

void GpuIndirectNoteCpuDestination() {
	g_gpu_indirect_stats.cpu_dest++;
}

Buffer& RenderExecutor::MeshEmulationScratch() {
	if (m_mesh_scratch == nullptr) {
		uint64_t megabytes = 256;
		if (const char* value = std::getenv("KYTY_MESH_SCRATCH_MB"); value != nullptr) {
			megabytes = std::clamp<uint64_t>(std::strtoull(value, nullptr, 10), 16u, 4096u);
		}
		const auto usage = std::getenv("KYTY_MESH_SCRATCH_HOST") != nullptr
		                       ? MemoryUsage::Upload
		                       : MemoryUsage::DeviceLocal;
		const auto flags = vk::BufferUsageFlagBits::eStorageBuffer |
		                   (!Config::MeshScratchDescriptorEnabled()
		                        ? vk::BufferUsageFlagBits::eShaderDeviceAddress
		                        : vk::BufferUsageFlags {});
		m_mesh_scratch = std::make_unique<Buffer>(
		    m_context.GetGraphics(), m_context.GetCommandScheduler(), usage, 0, flags,
		    megabytes << 20u);
		LOGF("MeshEmulation: scratch buffer %" PRIu64 " MB usage=%s\n", megabytes,
		     usage == MemoryUsage::Upload ? "host" : "device");
	}
	return *m_mesh_scratch;
}

void RenderExecutor::ExecuteEmulatedMeshDraw(CommandBuffer& buffer, const DrawCallInfo& draw,
                                             DrawRenderState& state,
                                             PipelineCache::Pipeline& pipeline,
                                             std::span<PreparedBindings* const> stages,
                                             const RenderState&                 rendering,
                                             vk::ImageAspectFlags feedback_aspects,
                                             std::span<const uint32_t> draw_data_in,
                                             uint32_t                  mesh_groups) {
	auto        vk_buffer = buffer.RawHandle(); // ExecutePreparedDraw flushed KYTY_MESH_BATCH
	auto&       scheduler = m_context.GetCommandScheduler();
	const auto& mesh      = state.vertex_info[0].mesh;
	const auto& program   = *state.vertex_info[0].stage.program;
	const auto  layout =
	    MeshEmulation::ComputeLayout(program.info.outputs, mesh.max_vertices, mesh.max_primitives);
	auto&      scratch      = MeshEmulationScratch();
	const auto item_bytes   = layout.ItemBytes();
	const auto instances    = draw.instance_count;
	// KYTY_GPU_INDIRECT reserves the scratch tail for the GPU-written param block.
	const auto capacity     = MeshScratchItemBytes(scratch) / item_bytes;
	if (capacity < instances) {
		EXIT("mesh emulation scratch too small: item=%" PRIu64 " instances=%u (raise "
		     "KYTY_MESH_SCRATCH_MB)\n",
		     item_bytes, instances);
	}
	const auto chunk_limit =
	    static_cast<uint32_t>(std::min<uint64_t>(capacity / instances, mesh_groups));

	std::array<uint32_t, ShaderRecompiler::IR::PushData::MeshDrawDwordCount> draw_data {};
	std::copy(draw_data_in.begin(), draw_data_in.end(), draw_data.begin());
	const bool descriptor_scratch = Config::MeshScratchDescriptorEnabled();
	if (descriptor_scratch) {
		// The descriptor-backed emulated shaders do not read the legacy address.
		draw_data[MeshEmulation::ScratchAddressLoDword] = 0;
		draw_data[MeshEmulation::ScratchAddressHiDword] = 0;
	} else {
		const auto address = scratch.BufferDeviceAddress();
		draw_data[MeshEmulation::ScratchAddressLoDword] = static_cast<uint32_t>(address);
		draw_data[MeshEmulation::ScratchAddressHiDword] = static_cast<uint32_t>(address >> 32u);
	}
	constexpr auto push_stages = vk::ShaderStageFlagBits::eVertex |
	                             vk::ShaderStageFlagBits::eFragment |
	                             vk::ShaderStageFlagBits::eCompute;

	// Stage 0 is the guest mesh program (compute); the rest are graphics stages.
	std::array<PreparedBindings*, 1> compute_stages {stages.front()};
	const auto graphics_stages = stages.subspan(1);

	// Diagnostics: KYTY_MESH_EMU_SKIP=compute|draw|all bisects which half of the emulated mesh
	// draw is responsible for a GPU fault.
	static const char* skip_env = std::getenv("KYTY_MESH_EMU_SKIP");
	const bool  skip_compute = skip_env != nullptr && (std::strcmp(skip_env, "compute") == 0 ||
	                                                    std::strcmp(skip_env, "all") == 0);
	const bool  skip_draw    = skip_env != nullptr && (std::strcmp(skip_env, "draw") == 0 ||
	                                                    std::strcmp(skip_env, "all") == 0);
	for (uint32_t base = 0; base < mesh_groups; base += chunk_limit) {
		const auto groups = std::min(chunk_limit, mesh_groups - base);
		draw_data[MeshEmulation::GroupBaseDword]   = base;
		draw_data[MeshEmulation::ChunkGroupsDword] = groups;

		scheduler.EndRendering();
		// Earlier draws may still read the scratch, and earlier passes wrote it.
		vk::MemoryBarrier reuse {};
		reuse.srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
		reuse.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
		vk_buffer.pipelineBarrier(
		    vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eComputeShader,
		    vk::PipelineStageFlagBits::eComputeShader, {}, 1, &reuse, 0, nullptr, 0, nullptr);

		if (!skip_compute) {
			vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.mesh_compute);
			CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline, compute_stages);
			vk_buffer.pushConstants(pipeline.pipeline_layout, push_stages, 0,
			                        static_cast<uint32_t>(draw_data.size() * sizeof(uint32_t)),
			                        draw_data.data());
			if (base == 0) {
				vk_buffer.dispatch(groups, instances, 1);
			} else {
				vk_buffer.dispatchBase(base, 0, 0, groups, instances, 1);
			}
		}

		vk::MemoryBarrier produced {};
		produced.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
		produced.dstAccessMask = vk::AccessFlagBits::eShaderRead;
		vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
		                          vk::PipelineStageFlagBits::eVertexShader, {}, 1, &produced, 0,
		                          nullptr, 0, nullptr);

		// The descriptor-backed generated vertex shader always reads the scratch binding, so the
		// graphics set must be bound even for depth-only draws that have no pixel-shader stage.
		// Skipping it left MoltenVK with a null descriptor set at encode time.
		if (!graphics_stages.empty() || descriptor_scratch) {
			CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline, graphics_stages);
		}
		vk_buffer.pushConstants(pipeline.pipeline_layout, push_stages, 0,
		                        static_cast<uint32_t>(draw_data.size() * sizeof(uint32_t)),
		                        draw_data.data());
		SetGraphicsDynamicParams(buffer, vk_buffer, state.vertex_info[0], state.depth_info,
		                         rendering);
		if (m_context.GetGraphics().attachment_feedback_loop_enabled) {
			vk_buffer.setAttachmentFeedbackLoopEnableEXT(feedback_aspects);
		}
		scheduler.BeginRendering(rendering);
		vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
		const uint64_t vertex_count =
		    uint64_t {groups} * instances * mesh.max_primitives * 3u;
		EXIT_IF(vertex_count > UINT32_MAX);
		if (!skip_draw) {
			vk_buffer.draw(static_cast<uint32_t>(vertex_count), 1, 0, 0);
		}
	}
	static std::atomic_bool logged = false;
	if (!logged.exchange(true, std::memory_order_relaxed)) {
		LOGF("MeshEmulation: first draw groups=%u instances=%u item_bytes=%" PRIu64
		     " vertex_slots=%u primitives/group=%u\n",
		     mesh_groups, instances, item_bytes, layout.VertexSlots(), mesh.max_primitives);
	}
}

// ------------------------------------------------------------------------------------------------
// KYTY_GPU_INDIRECT=1: native GPU-driven execution of single guest indirect draws.
//
// The CPU path (CommandProcessor::DrawIndirectMulti) dereferences the guest argument record. When
// a GPU pass in the still-open command buffer wrote it, that load faults, and the fault handler
// submits the command buffer and waits for all of it (DownloadBufferMemory<false>, a full drain).
// Here the record stays on the GPU: a one-thread compute pass converts it into the emulated mesh
// draw's param block plus dispatch/draw indirect commands; the mesh compute program reads its draw
// parameters (and the patched user SGPRs) from that block. See meshEmulation.h and
// host_gpu/shaders/gpu_indirect_mesh_args.comp.
// ------------------------------------------------------------------------------------------------

RenderExecutor::~RenderExecutor() {
	if (m_gpu_indirect_pipeline || m_gpu_indirect_pipeline_layout || m_gpu_indirect_set_layout) {
		const auto device = m_context.GetGraphics().device;
		device.destroyPipeline(m_gpu_indirect_pipeline, nullptr);
		device.destroyPipelineLayout(m_gpu_indirect_pipeline_layout, nullptr);
		device.destroyDescriptorSetLayout(m_gpu_indirect_set_layout, nullptr);
	}
}

void RenderExecutor::EnsureGpuIndirect() {
	if (m_gpu_indirect_pipeline) {
		return;
	}
	auto& graphics  = m_context.GetGraphics();
	auto& scheduler = m_context.GetCommandScheduler();
	m_gpu_indirect_commands = std::make_unique<Buffer>(
	    graphics, scheduler, MemoryUsage::DeviceLocal, 0,
	    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer,
	    uint64_t {GpuIndirectSlots} * GpuIndirectSlotBytes);
	m_gpu_indirect_history = std::make_unique<Buffer>(
	    graphics, scheduler, MemoryUsage::Download, 0, vk::BufferUsageFlagBits::eStorageBuffer,
	    GpuIndirectHistoryHeader + uint64_t {GpuIndirectSlots} * GpuIndirectSlotBytes);
	EXIT_IF(m_gpu_indirect_history->Mapped().size() < m_gpu_indirect_history->Size());
	std::memset(m_gpu_indirect_history->Mapped().data(), 0, m_gpu_indirect_history->Size());
	m_gpu_indirect_history->Flush(0, m_gpu_indirect_history->Size());
	SetVulkanObjectNameF(graphics.device, m_gpu_indirect_commands->Handle(), "GPU indirect commands");
	SetVulkanObjectNameF(graphics.device, m_gpu_indirect_history->Handle(), "GPU indirect history");

	std::array<vk::DescriptorSetLayoutBinding, 4> bindings {};
	for (uint32_t i = 0; i < bindings.size(); i++) {
		bindings[i] = {i, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute,
		               nullptr};
	}
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
	layout_info.pBindings    = bindings.data();
	RequireVulkanSuccess(graphics.device.createDescriptorSetLayout(&layout_info, nullptr,
	                                                               &m_gpu_indirect_set_layout),
	                     "create GPU indirect descriptor layout");
	vk::PushConstantRange push_range {vk::ShaderStageFlagBits::eCompute, 0,
	                                  GpuIndirectPushDwords * sizeof(uint32_t)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_gpu_indirect_set_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                          &m_gpu_indirect_pipeline_layout),
	                     "create GPU indirect pipeline layout");
	const auto module = CompileSPV(GPU_INDIRECT_MESH_ARGS_SPV, graphics.device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_gpu_indirect_pipeline_layout;
	const auto result = graphics.device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr,
	                                                           &m_gpu_indirect_pipeline);
	graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create GPU indirect pipeline");
	SetVulkanObjectNameF(graphics.device, m_gpu_indirect_pipeline, "GPU indirect mesh args");
	LOGF("GpuIndirect: enabled (mesh-emulated single indirect draws convert on the GPU)\n");
}

bool RenderExecutor::ReadGpuIndirectHistory(const GpuIndirectHistoryRef& ref,
                                            GpuIndirectHistoryValues& values) {
	Common::LockGuard lock(m_context.GetMutex());
	EXIT_IF(m_gpu_indirect_history == nullptr);
	auto& scheduler = m_context.GetCommandScheduler();
	g_gpu_indirect_stats.resolve_regs++;
	if (!scheduler.IsFree(ref.tick)) {
		const auto start = std::chrono::steady_clock::now();
		scheduler.Wait(ref.tick);
		g_gpu_indirect_stats.resolve_drains++;
		g_gpu_indirect_stats.resolve_wait_us += static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
		                                                          start)
		        .count());
	}
	const auto offset = GpuIndirectHistoryHeader + uint64_t {ref.index} * GpuIndirectSlotBytes;
	m_gpu_indirect_history->Invalidate(offset, GpuIndirectSlotBytes);
	std::array<uint32_t, GpuIndirectSlotBytes / 4u> words {};
	std::memcpy(words.data(), m_gpu_indirect_history->Mapped().data() + offset, sizeof(words));
	if (words[0] != ref.sequence) {
		g_gpu_indirect_stats.resolve_seq_miss++;
		return false;
	}
	values.count          = words[1];
	values.instances      = words[2];
	values.vertex_offset  = words[3];
	values.start_instance = words[4];
	values.start_index    = words[5];
	return true;
}

uint32_t RenderExecutor::GpuPendingSlots(CommandBuffer& buffer) {
	// A register the guest rewrote since the GPU draw holds the guest's value again.
	m_gpu_pending_mask &= ~buffer.GetShaders().GsUserWritten();
	return m_gpu_pending_mask;
}

uint32_t RenderExecutor::GpuPendingSlotsReadBy(CommandBuffer& buffer, const DrawRenderState& state) {
	const auto pending = GpuPendingSlots(buffer);
	if (pending == 0) {
		return 0;
	}
	const auto* program = state.vertex_info[0].stage.program;
	if (program == nullptr || program->stage != ShaderType::Mesh ||
	    state.programs.VertexStageCount() != 1) {
		return pending; // conservative: only mesh user-data layouts are decoded below
	}
	const auto user_sgprs = buffer.GetShaders().GetVs().gs_regs.rsrc2.user_sgpr;
	if (program->user_data_count < user_sgprs + program->user_data_base) {
		return pending;
	}
	// GS user-data slot i lands in SGPR user_data_base + offset + i (offset 8 for NGG, see
	// PrepareProgram/GetShaderParams in shader.cpp).
	const auto first = program->user_data_base + (program->user_data_count -
	                                              program->user_data_base - user_sgprs);
	uint32_t   reads = 0;
	const auto add   = [&](int64_t sgpr) {
		if (sgpr >= first && sgpr - first < 32) {
			reads |= 1u << static_cast<uint32_t>(sgpr - first);
		}
	};
	for (const auto sgpr: program->info.user_data_registers) {
		add(sgpr);
	}
	if (state.vertex_info[0].fetch_embedded) {
		add(program->info.vertex_offset_sgpr);
		add(program->info.instance_offset_sgpr);
	}
	return pending & reads;
}

void RenderExecutor::ResolveGpuPendingRegisters(CommandBuffer& buffer, uint32_t slots) {
	auto& shaders = buffer.GetShaders();
	for (uint32_t slot = 0; slot < 32; slot++) {
		if ((slots & m_gpu_pending_mask & (1u << slot)) == 0) {
			continue;
		}
		const auto&              pending = m_gpu_pending[slot];
		GpuIndirectHistoryValues values;
		uint32_t                 value = 0;
		if (ReadGpuIndirectHistory(pending.ref, values)) {
			value = pending.role == 0   ? values.vertex_offset
			        : pending.role == 1 ? values.start_instance
			                            : values.start_index;
		} else {
			// History slot recycled (>4096 GPU draws since): read the guest record, as the CPU
			// path would have, accepting that it may be newer. Counted as resolve_seq_miss.
			const auto* words = reinterpret_cast<const volatile uint32_t*>(pending.ref_args_addr);
			const auto  indexed = pending.ref_indexed;
			value = pending.role == 0   ? words[indexed ? 3 : 2]
			        : pending.role == 1 ? words[indexed ? 4 : 3]
			                            : (indexed ? words[2] : 0u);
		}
		shaders.SetGsUserSgpr(slot, value, HW::UserSgprType::Unknown);
		m_gpu_pending_mask &= ~(1u << slot);
	}
}

void RenderExecutor::LogGpuIndirectStats() {
	auto&      stats = g_gpu_indirect_stats;
	const auto now   = std::chrono::steady_clock::now();
	const auto dt =
	    std::chrono::duration_cast<std::chrono::milliseconds>(now - stats.last_log).count();
	if (dt < 2000) {
		return;
	}
	uint32_t overflow = 0;
	uint32_t oob      = 0;
	if (m_gpu_indirect_history != nullptr) {
		m_gpu_indirect_history->Invalidate(0, GpuIndirectHistoryHeader);
		std::memcpy(&overflow, m_gpu_indirect_history->Mapped().data() + 0, 4);
		std::memcpy(&oob, m_gpu_indirect_history->Mapped().data() + 4, 4);
	}
	std::string reasons;
	for (size_t i = 1; i < stats.draws.size(); i++) {
		if (stats.draws[i] != 0) {
			reasons += fmt::format(" {}={}", GpuIndirectReasonName(static_cast<GpuIndirectReason>(i)),
			                       stats.draws[i]);
		}
	}
	LOGF("GpuIndirect: dt_ms=%lld gpu=%" PRIu64 " cpu_multi=%" PRIu64 " cpu_dest=%" PRIu64
	     " resolve=%" PRIu64 "/%" PRIu64 "drains/%" PRIu64 "us seq_miss=%" PRIu64
	     " overflow_total=%u oob_total=%u fallback:%s\n",
	     static_cast<long long>(dt), stats.draws[0], stats.cpu_multi, stats.cpu_dest,
	     stats.resolve_regs,
	     stats.resolve_drains, stats.resolve_wait_us, stats.resolve_seq_miss, overflow, oob,
	     reasons.empty() ? " none" : reasons.c_str());
	const auto last = now;
	stats           = {};
	stats.last_log  = last;
}

bool RenderExecutor::DrawIndirectGpu(uint64_t submit_id, CommandBuffer& buffer,
                                     const GpuIndirectDraw& args) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	LogGpuIndirectStats();
	const auto reject = [](GpuIndirectReason reason) {
		GpuIndirectCount(reason)++;
		return false;
	};
	auto& graphics = m_context.GetGraphics();
	if (!graphics.mesh_emulation) {
		return reject(GpuIndirectReason::NoMeshEmulation);
	}
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();
	const auto debug_op = args.indexed ? CommandBufferDebugOp::DrawIndex
	                                   : CommandBufferDebugOp::DrawIndexAuto;
	buffer.SetDebugInfo(static_cast<uint32_t>(debug_op), submit_id, 0, 0, 0x1d,
	                    static_cast<uint32_t>(args.args_addr), args.args_addr);

	Common::LockGuard lock(m_context.GetMutex());

	// Register-only gates, evaluated in the CPU path's order before it touches any resource.
	// Any draw these would divert keeps the CPU path (which reads the arguments).
	const auto cb_mode = buffer.GetRegisters().GetColorControl().mode;
	if (ConsumeMetadataColorOperation(buffer) || cb_mode == 3u /* ResolveColorTargets */ ||
	    DepthStencilCopyRequested(buffer.GetRegisters())) {
		return reject(GpuIndirectReason::SpecialColorMode);
	}
	if (!DrawHasValidVertexShader(sh_ctx)) {
		return reject(GpuIndirectReason::NoVertexShader);
	}
	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
		return reject(GpuIndirectReason::Topology);
	}
	DrawIndexBufferSource index_source {};
	if (args.indexed) {
		if (args.index_buffer_size == 0) {
			return reject(GpuIndirectReason::NoIndexBufferSize);
		}
		switch (static_cast<Prospero::IndexType>(args.index_type_and_size)) {
			case Prospero::IndexType::kIndex16:
				index_source.type               = vk::IndexType::eUint16;
				index_source.guest_element_size = 2;
				break;
			case Prospero::IndexType::kIndex32:
				index_source.type               = vk::IndexType::eUint32;
				index_source.guest_element_size = 4;
				break;
			case Prospero::IndexType::kIndex8: index_source.guest_element_size = 1; break;
			default: EXIT("unknown index_type_and_size: %u\n", args.index_type_and_size);
		}
		index_source.address = args.index_base_addr;
		index_source.size =
		    uint64_t {args.index_buffer_size} * index_source.guest_element_size;
	}
	const bool primitive_restart =
	    args.indexed && ResolvePrimitiveRestart(buffer, index_source, /*allow_scan=*/false);

	// Count placeholders: only names and logging read them before the mesh path below.
	const DrawCallInfo draw {debug_op, 1, 1, 0};
	DrawRenderState    state {};
	if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state)) {
		ResetBindings();
		return reject(GpuIndirectReason::NoTargets);
	}
	const auto& vertex_info = state.vertex_info[0];
	const auto& program     = *vertex_info.stage.program;
	const auto& mesh        = vertex_info.mesh;
	if (program.stage != ShaderType::Mesh || state.programs.VertexStageCount() != 1) {
		ResetBindings();
		return reject(GpuIndirectReason::NotMeshProgram);
	}
	if (mesh.primitives_per_group == 0 ||
	    (mesh.fast_launch && (args.indexed || primitive_restart))) {
		ResetBindings();
		return reject(GpuIndirectReason::MeshShape);
	}
	if (!args.indexed && Prospero::IsRectList(ucfg.GetPrimType()) && vertex_info.buffers_num == 0 &&
	    program.param_export_mask == 0 && state.ps_input_info.input_num != 0) {
		ResetBindings();
		return reject(GpuIndirectReason::RectListSkip);
	}
	// Map patched GS user-data slots to the program's SGPRs (and check pending registers).
	const auto user_sgprs = sh_ctx.GetVs().gs_regs.rsrc2.user_sgpr;
	if (program.user_data_count < program.user_data_base + user_sgprs) {
		ResetBindings();
		return reject(GpuIndirectReason::UserDataLayout);
	}
	const auto first_sgpr =
	    program.user_data_base + (program.user_data_count - program.user_data_base - user_sgprs);
	uint32_t patch_mask = 0;
	uint32_t mode_word  = MeshEmulation::ParamModeBit;
	for (uint32_t role = 0; role < GpuIndirectDraw::RoleCount; role++) {
		const auto slot = args.patch_slots[role];
		if (slot < 0) {
			continue;
		}
		const auto sgpr = static_cast<int32_t>(first_sgpr) + slot;
		if (sgpr > static_cast<int32_t>(MeshEmulation::ParamPatchSgprMask)) {
			ResetBindings();
			return reject(GpuIndirectReason::UserDataLayout);
		}
		patch_mask |= 1u << static_cast<uint32_t>(slot);
		mode_word |= MeshEmulation::EncodeParamPatch(role, sgpr);
	}
	if ((GpuPendingSlotsReadBy(buffer, state) & ~patch_mask) != 0) {
		// An older GPU draw's register value would be read: the CPU path resolves it.
		ResetBindings();
		return reject(GpuIndirectReason::PendingRead);
	}
	if (primitive_restart) {
		static std::atomic_bool restart_warned = false;
		if (!restart_warned.exchange(true, std::memory_order_relaxed)) {
			std::printf("Warning: primitive restart is not implemented for mesh shaders; "
			            "continuing draw (primitive=%u indexed=%u)\n",
			            static_cast<uint32_t>(ucfg.GetPrimType()), args.indexed);
		}
	}

	// ---- From here on the draw is committed to the GPU path. ----
	EnsureGpuIndirect();
	auto& cache = m_context.GetBufferCache();
	if (args.indexed) {
		// Register the guest index range for the mesh program's BDA reads. INDEX_BUFFER_SIZE bounds
		// every index the CPU path could fetch (its count is clamped to it).
		(void)cache.FindBuffer(index_source.address, index_source.size);
	}
	auto&                            bindings = m_graphics_bindings;
	std::array<PreparedBindings*, 4> descriptor_stages {};
	uint32_t                         stage_count = 0;
	PrepareBindings(vertex_info.stage, bindings.vertex[0]);
	descriptor_stages[stage_count++] = &bindings.vertex[0];
	if (state.ps_active) {
		if (!bindings.pixel) bindings.pixel.emplace();
		PrepareBindings(state.ps_input_info.stage, *bindings.pixel);
		descriptor_stages[stage_count++] = &*bindings.pixel;
	}
	const auto stages = std::span {descriptor_stages.data(), stage_count};
	PrepareGraphicsBindings(stages, std::span {state.color_info, state.color_count});
	const uint64_t args_size   = args.indexed ? 20u : 16u;
	const auto [args_buffer, args_offset] = cache.ObtainBuffer(args.args_addr, args_size, false);
	EXIT_IF(args_buffer == nullptr || (args_offset & 3u) != 0);
	const auto vertex_stages = std::span {state.vertex_info.data(), 1u};
	auto& pipeline = m_context.GetPipelineCache().GetGraphicsPipeline(
	    std::span {state.color_info, state.color_count}, state.depth_info, vertex_stages, buffer,
	    state.ps_active ? &state.ps_input_info : nullptr, topology, primitive_restart,
	    state.programs);
	EXIT_IF(pipeline.mesh_compute == nullptr);
	vk::ImageAspectFlags feedback_aspects;
	const auto rendering = AcquireRenderTargets(buffer, state.color_info, state.color_count,
	                                            state.depth_info, feedback_aspects, stages);

	// Resource preparation above may have restarted the scheduler: use the current handle.
	auto& scheduler = m_context.GetCommandScheduler();
	scheduler.EndRendering();
	auto vk_buffer = buffer.Handle();
	SetDrawDebugPhase(buffer, submit_id, draw, args.indexed ? 0x1100u : 0x1200u);

	const auto layout =
	    MeshEmulation::ComputeLayout(program.info.outputs, mesh.max_vertices, mesh.max_primitives);
	auto&      scratch    = MeshEmulationScratch();
	const auto item_bytes = layout.ItemBytes();
	const auto capacity   = MeshScratchItemBytes(scratch) / item_bytes;
	const auto tail       = scratch.Size() - GpuIndirectTailBytes;
	const auto slot       = m_gpu_indirect_cursor;
	m_gpu_indirect_cursor = (m_gpu_indirect_cursor + 1u) % GpuIndirectSlots;
	const auto sequence   = ++m_gpu_indirect_sequence;
	const auto& limits    = graphics.GetPhysicalDeviceProperties().limits;

	// 1. Conversion pass. Producer writes (shader/transfer, any earlier stage) -> argument read;
	//    earlier indirect reads / mesh reads of the reused param block and ring -> overwrite.
	{
		vk::MemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
		vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics |
		                              vk::PipelineStageFlagBits::eComputeShader |
		                              vk::PipelineStageFlagBits::eTransfer,
		                          vk::PipelineStageFlagBits::eComputeShader, {}, 1, &before, 0,
		                          nullptr, 0, nullptr);
		const auto align = std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4u);
		const auto args_bind = args_offset / align * align;
		const std::array<vk::DescriptorBufferInfo, 4> infos {{
		    {args_buffer->Handle(), args_bind, args_offset - args_bind + args_size},
		    {scratch.Handle(), tail, GpuIndirectTailBytes},
		    {m_gpu_indirect_commands->Handle(), 0, m_gpu_indirect_commands->Size()},
		    {m_gpu_indirect_history->Handle(), 0, m_gpu_indirect_history->Size()},
		}};
		std::array<vk::WriteDescriptorSet, 4> writes {};
		for (uint32_t i = 0; i < writes.size(); i++) {
			writes[i].dstBinding      = i;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType  = vk::DescriptorType::eStorageBuffer;
			writes[i].pBufferInfo     = &infos[i];
		}
		const std::array<uint32_t, GpuIndirectPushDwords> push {
		    static_cast<uint32_t>((args_offset - args_bind) / 4u),
		    0u,
		    slot * (GpuIndirectSlotBytes / 4u),
		    GpuIndirectHistoryHeader / 4u + slot * (GpuIndirectSlotBytes / 4u),
		    (args.indexed ? 1u : 0u) | (vertex_info.fetch_embedded ? 2u : 0u),
		    index_source.guest_element_size,
		    static_cast<uint32_t>(args.index_base_addr),
		    static_cast<uint32_t>(args.index_base_addr >> 32u),
		    args.index_buffer_size,
		    mesh.InputPrimitiveSize(),
		    mesh.InputPrimitiveStep(),
		    mesh.primitives_per_group,
		    layout.max_primitives,
		    static_cast<uint32_t>(std::min<uint64_t>(capacity, UINT32_MAX)),
		    limits.maxComputeWorkGroupCount[0],
		    limits.maxComputeWorkGroupCount[1],
		    sequence,
		};
		vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_gpu_indirect_pipeline);
		vk_buffer.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
		                               m_gpu_indirect_pipeline_layout, 0, writes);
		vk_buffer.pushConstants(m_gpu_indirect_pipeline_layout, vk::ShaderStageFlagBits::eCompute,
		                        0, static_cast<uint32_t>(sizeof(push)), push.data());
		vk_buffer.dispatch(1, 1, 1);
		vk::MemoryBarrier after {};
		after.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
		after.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead |
		                      vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eHostRead;
		vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
		                          vk::PipelineStageFlagBits::eDrawIndirect |
		                              vk::PipelineStageFlagBits::eComputeShader |
		                              vk::PipelineStageFlagBits::eVertexShader |
		                              vk::PipelineStageFlagBits::eHost,
		                          {}, 1, &after, 0, nullptr, 0, nullptr);
	}

	// 2. Mesh pass, mirroring ExecuteEmulatedMeshDraw for one chunk with GPU-sourced counts.
	std::array<uint32_t, ShaderRecompiler::IR::PushData::MeshDrawDwordCount> draw_data {};
	draw_data[MeshEmulation::ParamBlockDword] = static_cast<uint32_t>(tail / 4u);
	draw_data[MeshEmulation::ParamModeDword]  = mode_word;
	const bool descriptor_scratch             = Config::MeshScratchDescriptorEnabled();
	if (!descriptor_scratch) {
		const auto address = scratch.BufferDeviceAddress();
		draw_data[MeshEmulation::ScratchAddressLoDword] = static_cast<uint32_t>(address);
		draw_data[MeshEmulation::ScratchAddressHiDword] = static_cast<uint32_t>(address >> 32u);
	}
	draw_data[MeshEmulation::GroupBaseDword]   = 0;
	draw_data[MeshEmulation::ChunkGroupsDword] = 0; // param block supplies it
	constexpr auto push_stages = vk::ShaderStageFlagBits::eVertex |
	                             vk::ShaderStageFlagBits::eFragment |
	                             vk::ShaderStageFlagBits::eCompute;
	const auto commands_offset = uint64_t {slot} * GpuIndirectSlotBytes;
	{
		vk::MemoryBarrier reuse {};
		reuse.srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
		reuse.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
		vk_buffer.pipelineBarrier(
		    vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eComputeShader,
		    vk::PipelineStageFlagBits::eComputeShader, {}, 1, &reuse, 0, nullptr, 0, nullptr);
	}
	std::array<PreparedBindings*, 1> compute_stages {stages.front()};
	const auto graphics_stages = stages.subspan(1);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.mesh_compute);
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline, compute_stages);
	vk_buffer.pushConstants(pipeline.pipeline_layout, push_stages, 0,
	                        static_cast<uint32_t>(draw_data.size() * sizeof(uint32_t)),
	                        draw_data.data());
	vk_buffer.dispatchIndirect(m_gpu_indirect_commands->Handle(), commands_offset);
	{
		vk::MemoryBarrier produced {};
		produced.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
		produced.dstAccessMask = vk::AccessFlagBits::eShaderRead;
		vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
		                          vk::PipelineStageFlagBits::eVertexShader, {}, 1, &produced, 0,
		                          nullptr, 0, nullptr);
	}
	if (!graphics_stages.empty() || descriptor_scratch) {
		CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline, graphics_stages);
	}
	vk_buffer.pushConstants(pipeline.pipeline_layout, push_stages, 0,
	                        static_cast<uint32_t>(draw_data.size() * sizeof(uint32_t)),
	                        draw_data.data());
	SetGraphicsDynamicParams(buffer, vk_buffer, vertex_info, state.depth_info, rendering);
	if (graphics.attachment_feedback_loop_enabled) {
		vk_buffer.setAttachmentFeedbackLoopEnableEXT(feedback_aspects);
	}
	scheduler.BeginRendering(rendering);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
	vk_buffer.drawIndirect(m_gpu_indirect_commands->Handle(), commands_offset + 16u, 1, 16u);

	vk::PipelineStageFlags shader_write_stages = {};
	if (HasShaderBufferWrites(vertex_info.stage)) {
		shader_write_stages |= ShaderPipelineStages(NativeShaderStage(vertex_info.logical_stage));
	}
	if (state.ps_active && HasShaderBufferWrites(state.ps_input_info.stage)) {
		shader_write_stages |= vk::PipelineStageFlagBits::eFragmentShader;
	}
	if (shader_write_stages) {
		scheduler.EndRendering();
		ShaderWriteBarrier(vk_buffer, shader_write_stages);
	}
	ResetBindings();

	// The patched registers and NUM_INSTANCES now live in history slot `slot`.
	const GpuIndirectHistoryRef ref {scheduler.CurrentTick(), slot, sequence};
	for (uint32_t role = 0; role < GpuIndirectDraw::RoleCount; role++) {
		const auto patch_slot = args.patch_slots[role];
		if (patch_slot >= 0) {
			m_gpu_pending[patch_slot] = {ref, role, args.args_addr, args.indexed};
		}
	}
	sh_ctx.ClearGsUserWritten(patch_mask);
	m_gpu_pending_mask |= patch_mask;
	m_gpu_last_ref = {ref, 0, args.args_addr, args.indexed};
	GpuIndirectCount(GpuIndirectReason::Gpu)++;
	return true;
}


// ---------------------------------------------------------------------------------------------
// KYTY_MESH_BATCH (see meshBatchPolicy.h)

void MeshDynamicState::setViewportWithCount(uint32_t count, const vk::Viewport* v) {
	EXIT_IF(count > MaxViewports);
	viewport_count = count;
	std::copy(v, v + count, viewports.begin());
}

void MeshDynamicState::setScissorWithCount(uint32_t count, const vk::Rect2D* r) {
	EXIT_IF(count != viewport_count);
	std::copy(r, r + count, scissors.begin());
}

static size_t MeshStencilFaceIndex(vk::StencilFaceFlagBits face) {
	EXIT_IF(face != vk::StencilFaceFlagBits::eFront && face != vk::StencilFaceFlagBits::eBack);
	return face == vk::StencilFaceFlagBits::eFront ? 0 : 1;
}

void MeshDynamicState::setStencilOp(vk::StencilFaceFlagBits face, vk::StencilOp f,
                                    vk::StencilOp p, vk::StencilOp d, vk::CompareOp c) {
	auto& s  = faces[MeshStencilFaceIndex(face)];
	s.op_set = true;
	s.fail = f, s.pass = p, s.depth_fail = d, s.compare = c;
}
void MeshDynamicState::setStencilCompareMask(vk::StencilFaceFlagBits face, uint32_t m) {
	auto& s       = faces[MeshStencilFaceIndex(face)];
	s.compare_set = true;
	s.compare_mask = m;
}
void MeshDynamicState::setStencilWriteMask(vk::StencilFaceFlagBits face, uint32_t m) {
	auto& s     = faces[MeshStencilFaceIndex(face)];
	s.write_set = true;
	s.write_mask = m;
}
void MeshDynamicState::setStencilReference(vk::StencilFaceFlagBits face, uint32_t r) {
	auto& s   = faces[MeshStencilFaceIndex(face)];
	s.ref_set = true;
	s.reference = r;
}
void MeshDynamicState::setColorWriteEnableEXT(uint32_t count, const vk::Bool32* enable) {
	EXIT_IF(count > color_write.size());
	color_write_count = count;
	std::copy(enable, enable + count, color_write.begin());
}

void MeshDynamicState::Record(vk::CommandBuffer cmd) const {
	// The order of SetGraphicsDynamicParams.
	cmd.setViewportWithCount(viewport_count, viewports.data());
	cmd.setScissorWithCount(viewport_count, scissors.data());
	cmd.setLineWidth(line_width);
	cmd.setBlendConstants(blend.data());
	cmd.setDepthTestEnable(depth_test);
	cmd.setDepthWriteEnable(depth_write);
	cmd.setDepthCompareOp(depth_compare);
	if (has_bounds_test) {
		cmd.setDepthBoundsTestEnable(bounds_test);
	}
	if (has_bounds) {
		cmd.setDepthBounds(bounds_min, bounds_max);
	}
	cmd.setDepthBiasEnable(bias_enable);
	if (has_bias) {
		cmd.setDepthBias(bias_constant, bias_clamp, bias_slope);
	}
	cmd.setStencilTestEnable(stencil_test);
	for (size_t i = 0; i < faces.size(); i++) {
		const auto  face = i == 0 ? vk::StencilFaceFlagBits::eFront : vk::StencilFaceFlagBits::eBack;
		const auto& s    = faces[i];
		if (s.op_set) cmd.setStencilOp(face, s.fail, s.pass, s.depth_fail, s.compare);
		if (s.compare_set) cmd.setStencilCompareMask(face, s.compare_mask);
		if (s.write_set) cmd.setStencilWriteMask(face, s.write_mask);
		if (s.ref_set) cmd.setStencilReference(face, s.reference);
	}
	if (color_write_count != 0) {
		cmd.setColorWriteEnableEXT(
		    vk::ArrayProxy<const vk::Bool32>(color_write_count, color_write.data()));
	}
}

static uint64_t MeshBatchImageKey(ImageId id) {
	return (uint64_t {id.index} << 32u) | id.generation;
}

bool RenderExecutor::MeshDrawBatchable(const DrawCallInfo& draw, DrawRenderState& state,
                                       std::span<PreparedBindings* const> stages,
                                       uint32_t mesh_groups, uint64_t& scratch_bytes) {
	m_mesh_stats.MaybePrint([](const char* line) { LOGF("%s\n", line); });
	static const bool diagnostics = std::getenv("KYTY_MESH_EMU_SKIP") != nullptr;
	if (diagnostics) {
		m_mesh_stats.NoteReject(MeshBatch::RejectReason::Diagnostics);
		return false;
	}
	const auto& mesh    = state.vertex_info[0].mesh;
	const auto& program = *state.vertex_info[0].stage.program;
	const auto  layout =
	    MeshEmulation::ComputeLayout(program.info.outputs, mesh.max_vertices, mesh.max_primitives);
	auto& scratch = MeshEmulationScratch();
	if (m_mesh_ring.capacity == 0) {
		m_mesh_ring.capacity = scratch.Size();
		m_mesh_ring.align    = std::max<uint64_t>(
            256u, m_context.GetGraphics().GetPhysicalDeviceProperties().limits.minStorageBufferOffsetAlignment);
	}
	// Exactly one chunk (the baseline loop runs once) and a legal vertex count.
	const uint64_t items = uint64_t {mesh_groups} * draw.instance_count;
	scratch_bytes        = items * layout.ItemBytes();
	if (scratch_bytes == 0 || scratch_bytes > scratch.Size() ||
	    items * mesh.max_primitives * 3u > UINT32_MAX) {
		m_mesh_stats.NoteReject(MeshBatch::RejectReason::Chunked);
		return false;
	}
	// Draws that record barriers of their own (GDS, ShaderWriteBarrier) or write images through
	// storage (no barrier follows those) keep the baseline path.
	for (const auto* stage: stages) {
		if (stage->gds.buffer != nullptr) {
			m_mesh_stats.NoteReject(MeshBatch::RejectReason::Gds);
			return false;
		}
		if (stage->runtime->program->has_address_writes || HasShaderBufferWrites(*stage->runtime)) {
			m_mesh_stats.NoteReject(MeshBatch::RejectReason::Writes);
			return false;
		}
		for (const auto& binding: stage->images) {
			const auto* image = m_context.GetTextureCache().m_slot_images.try_get(binding.image_id);
			if (binding.desc.type == TextureCache::BindingType::Storage ||
			    (image != nullptr && image->binding.shader_write)) {
				m_mesh_stats.NoteReject(MeshBatch::RejectReason::Storage);
				return false;
			}
		}
	}
	// The compute half runs before the graphics halves of the pending draws: it must not read
	// anything they write, i.e. any of their attachments.
	if (!m_mesh_pending_targets.Empty()) {
		for (const auto& binding: stages.front()->images) {
			const auto* image = m_context.GetTextureCache().m_slot_images.try_get(binding.image_id);
			bool conflict = m_mesh_pending_targets.Conflicts(MeshBatchImageKey(binding.image_id), 0, 0);
			if (image != nullptr) {
				for (const auto range: {image->info.data, image->info.stencil, image->info.metadata.range}) {
					conflict |= m_mesh_pending_targets.Conflicts(0, range.address, range.size);
				}
			}
			if (conflict) {
				m_mesh_stats.NoteReject(MeshBatch::RejectReason::Hazard);
				return false;
			}
		}
	}
	return true;
}

void RenderExecutor::MeshBatchFlushHook(void* context, const CommandBuffer& buffer,
                                        MeshBatch::FlushReason reason) {
	static_cast<RenderExecutor*>(context)->FlushMeshBatch(buffer, reason);
}

void RenderExecutor::ExecuteBatchedMeshDraw(CommandBuffer& buffer, const DrawCallInfo& draw,
                                            DrawRenderState& state,
                                            PipelineCache::Pipeline& pipeline,
                                            std::span<PreparedBindings* const> stages,
                                            const RenderState&                 rendering,
                                            vk::ImageAspectFlags feedback_aspects,
                                            std::span<const uint32_t> draw_data_in,
                                            uint32_t mesh_groups, uint64_t scratch_bytes) {
	auto&       scheduler = m_context.GetCommandScheduler();
	auto&       scratch   = MeshEmulationScratch();
	const auto  vk_buffer = buffer.RawHandle();
	const auto& mesh      = state.vertex_info[0].mesh;
	const auto  instances = draw.instance_count;

	if (m_mesh_pending.size() >= MeshBatch::MaxDraws) {
		FlushMeshBatch(buffer, MeshBatch::FlushReason::Full);
	}
	uint64_t offset = 0;
	auto     result = m_mesh_ring.Try(scratch_bytes, offset);
	if (result == MeshBatch::ScratchRing::Result::NeedsNewEpoch) {
		if (!m_mesh_pending.empty()) {
			FlushMeshBatch(buffer, MeshBatch::FlushReason::Overflow);
		}
		// Baseline's per-draw prologue, once per epoch: earlier draws may still read the scratch,
		// and earlier passes wrote it.
		scheduler.EndRendering();
		vk::MemoryBarrier reuse {};
		reuse.srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
		reuse.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
		vk_buffer.pipelineBarrier(
		    vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eComputeShader,
		    vk::PipelineStageFlagBits::eComputeShader, {}, 1, &reuse, 0, nullptr, 0, nullptr);
		m_mesh_ring.NewEpoch();
		result = m_mesh_ring.Try(scratch_bytes, offset);
	}
	EXIT_IF(result != MeshBatch::ScratchRing::Result::Fits);

	std::array<uint32_t, ShaderRecompiler::IR::PushData::MeshDrawDwordCount> draw_data {};
	std::copy(draw_data_in.begin(), draw_data_in.end(), draw_data.begin());
	const bool descriptor_scratch = Config::MeshScratchDescriptorEnabled();
	if (descriptor_scratch) {
		draw_data[MeshEmulation::ScratchAddressLoDword] = 0;
		draw_data[MeshEmulation::ScratchAddressHiDword] = 0;
	} else {
		const auto address = scratch.BufferDeviceAddress() + offset;
		draw_data[MeshEmulation::ScratchAddressLoDword] = static_cast<uint32_t>(address);
		draw_data[MeshEmulation::ScratchAddressHiDword] = static_cast<uint32_t>(address >> 32u);
	}
	draw_data[MeshEmulation::GroupBaseDword]   = 0;
	draw_data[MeshEmulation::ChunkGroupsDword] = mesh_groups;
	constexpr auto push_stages = vk::ShaderStageFlagBits::eVertex |
	                             vk::ShaderStageFlagBits::eFragment |
	                             vk::ShaderStageFlagBits::eCompute;
	std::array<PreparedBindings*, 1> compute_stages {stages.front()};
	const auto graphics_stages = stages.subspan(1);

	// Compute half, recorded now into the open compute encoder. A barrier recorded by the
	// bindings (an image transition) replays the pending draws first via EndRendering.
	m_mesh_scratch_offset = offset;
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.mesh_compute);
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline, compute_stages);
	vk_buffer.pushConstants(pipeline.pipeline_layout, push_stages, 0,
	                        static_cast<uint32_t>(draw_data.size() * sizeof(uint32_t)),
	                        draw_data.data());
	vk_buffer.dispatch(mesh_groups, instances, 1);

	// Graphics half, captured.
	PendingMeshDraw pending;
	if (!graphics_stages.empty() || descriptor_scratch) {
		CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline, graphics_stages,
		               &pending.bindings);
		pending.commit_bindings = true;
	}
	m_mesh_scratch_offset = 0;
	pending.pipeline  = pipeline.pipeline;
	pending.layout    = pipeline.pipeline_layout;
	pending.draw_data = draw_data;
	SetGraphicsDynamicParams(buffer, pending.dynamic, state.vertex_info[0], state.depth_info,
	                         rendering);
	pending.set_feedback     = m_context.GetGraphics().attachment_feedback_loop_enabled;
	pending.feedback_aspects = feedback_aspects;
	pending.rendering        = rendering;
	pending.vertex_count     = static_cast<uint32_t>(uint64_t {mesh_groups} * instances *
                                                     mesh.max_primitives * 3u);
	m_mesh_pending.push_back(std::move(pending));

	auto& cache = m_context.GetTextureCache();
	const auto note_target = [&](ImageId id) {
		if (!id) return;
		const auto* image = cache.m_slot_images.try_get(id);
		m_mesh_pending_targets.Add(MeshBatchImageKey(id), 0, 0);
		if (image != nullptr) {
			for (const auto range: {image->info.data, image->info.stencil, image->info.metadata.range}) {
				m_mesh_pending_targets.Add(0, range.address, range.size);
			}
		}
	};
	for (uint32_t i = 0; i < state.color_count; i++) {
		note_target(state.color_info[i].image_id);
	}
	note_target(state.depth_info.image_id);
	// A flush during this draw closed the epoch; this draw's region is live in it again.
	m_mesh_ring.epoch = true;
	m_mesh_stats.batched++;
	buffer.SetFlushHook(&RenderExecutor::MeshBatchFlushHook, this);
}

void RenderExecutor::FlushMeshBatch(const CommandBuffer& buffer, MeshBatch::FlushReason reason) {
	if (m_mesh_pending.empty()) {
		return;
	}
	EXIT_IF(m_mesh_flushing);
	m_mesh_flushing = true;
	buffer.SetFlushHook(nullptr, nullptr);
	auto&      scheduler = m_context.GetCommandScheduler();
	const auto vk_buffer = buffer.RawHandle();

	// Baseline's per-draw "produced" barrier, once for every dispatch recorded since the last one.
	vk::MemoryBarrier produced {};
	produced.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	produced.dstAccessMask = vk::AccessFlagBits::eShaderRead;
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                          vk::PipelineStageFlagBits::eVertexShader, {}, 1, &produced, 0,
	                          nullptr, 0, nullptr);
	constexpr auto push_stages = vk::ShaderStageFlagBits::eVertex |
	                             vk::ShaderStageFlagBits::eFragment |
	                             vk::ShaderStageFlagBits::eCompute;
	// Baseline's per-draw epilogue, in draw order.
	for (const auto& pending: m_mesh_pending) {
		if (pending.commit_bindings) {
			pending.bindings.Record(vk_buffer);
		}
		vk_buffer.pushConstants(pending.layout, push_stages, 0,
		                        static_cast<uint32_t>(pending.draw_data.size() * sizeof(uint32_t)),
		                        pending.draw_data.data());
		pending.dynamic.Record(vk_buffer);
		if (pending.set_feedback) {
			vk_buffer.setAttachmentFeedbackLoopEnableEXT(pending.feedback_aspects);
		}
		scheduler.BeginRendering(pending.rendering);
		vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pending.pipeline);
		vk_buffer.draw(pending.vertex_count, 1, 0, 0);
	}
	m_mesh_stats.NoteFlush(reason, m_mesh_pending.size());
	m_mesh_pending.clear();
	m_mesh_pending_targets.Clear();
	m_mesh_ring.Close();
	m_mesh_flushing = false;
	m_mesh_stats.MaybePrint([](const char* line) { LOGF("%s\n", line); });
}

} // namespace Libs::Graphics
