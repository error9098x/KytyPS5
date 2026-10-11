#ifndef EMULATOR_SRC_GRAPHICS_SHADER_MESHEMULATION_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_MESHEMULATION_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

// Mesh-shader emulation for hosts without VK_EXT_mesh_shader (MoltenVK).
//
// The guest NGG/GS program, which Kyty normally runs as a Vulkan mesh shader, is compiled as a
// compute shader instead. Each workgroup writes one "item" to a scratch buffer reached through a
// buffer device address:
//
//   item = [ uvec4(primitive_count, 0, 0, 0) ]
//          [ max_primitives x uvec4(v0, v1, v2, flags) ]
//          [ max_vertices x vertex_slots x vec4 ]
//
// flags bit 31 = primitive is live, bits 0..15 = gl_Layer. A primitive is drawn only when its
// index is below primitive_count and its live bit is set. A generated vertex shader then draws
// `items * max_primitives * 3` vertices, reading the triangle and its vertices back. Dead
// primitives are emitted as triangles outside the clip volume.
namespace Libs::Graphics::MeshEmulation {

// Push-constant dwords (after the regular mesh draw parameters).
inline constexpr uint32_t ScratchAddressLoDword = 6;
inline constexpr uint32_t ScratchAddressHiDword = 7;
inline constexpr uint32_t GroupBaseDword        = 8;
inline constexpr uint32_t ChunkGroupsDword      = 9;
static_assert(ChunkGroupsDword < ShaderRecompiler::IR::PushData::MeshDrawDwordCount);

// KYTY_GPU_INDIRECT=1: GPU-sourced mesh draw parameters ("param mode").
//
// A single-draw guest DRAW_INDIRECT / DRAW_INDEX_INDIRECT whose arguments live in GPU memory is
// converted by a one-thread compute pass (host_gpu/shaders/gpu_indirect_mesh_args.comp) into a
// param block in the tail of the mesh scratch buffer plus VkDispatchIndirectCommand /
// VkDrawIndirectCommand records, so the CPU never dereferences the arguments. The mesh compute
// program then takes draw parameters 0..5, the chunk group count and up to three patched user
// SGPRs from the param block instead of push constants. Param mode is selected per draw by
// ParamModeBit in push dword ParamModeDword; with the bit clear every value comes from push
// constants exactly as before, so direct draws behave identically with the flag on.
inline bool GpuIndirectEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GPU_INDIRECT");
		return value != nullptr && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0);
	}();
	return enabled;
}

inline constexpr uint32_t ParamModeDword       = 3;  // carries the mode bit + patch map
inline constexpr uint32_t ParamBlockDword      = 0;  // param mode: dword index of the param block
inline constexpr uint32_t ParamModeBit         = 0x80000000u;
inline constexpr uint32_t ParamPatchShift      = 8;  // 3 x 7-bit fields: valid(0x40) | sgpr(0..63)
inline constexpr uint32_t ParamPatchBits       = 7;
inline constexpr uint32_t ParamPatchValid      = 0x40u;
inline constexpr uint32_t ParamPatchSgprMask   = 0x3fu;
inline constexpr uint32_t ParamPatchCount      = 3;  // vertex offset, instance offset, index offset
// Param block layout (dwords), written by the conversion pass.
inline constexpr uint32_t ParamDrawDwords      = 6;  // == mesh draw parameters 0..5
inline constexpr uint32_t ParamPatchValueDword = 6;  // 6..8: patched SGPR values by role
inline constexpr uint32_t ParamChunkGroupsDword = 9; // replaces push ChunkGroupsDword
inline constexpr uint32_t ParamBlockDwords     = 16;
static_assert(ParamPatchShift + ParamPatchCount * ParamPatchBits <= 31u);

[[nodiscard]] constexpr uint32_t EncodeParamPatch(uint32_t role, int32_t sgpr) {
	return sgpr < 0 ? 0u
	                : (ParamPatchValid | (static_cast<uint32_t>(sgpr) & ParamPatchSgprMask))
	                      << (ParamPatchShift + role * ParamPatchBits);
}

inline constexpr uint32_t PrimitiveLiveBit = 0x80000000u;
inline constexpr uint32_t PrimitiveLayerMask = 0xffffu;

enum class SlotKind : uint32_t { Vector, Clip, Cull, None };

struct OutputSlot {
	SlotKind kind      = SlotKind::None;
	uint32_t slot      = 0; // vec4 slot inside the vertex record
	uint32_t component = 0; // component for clip/cull scalars
};

struct Layout {
	uint32_t max_vertices   = 0;
	uint32_t max_primitives = 0;
	uint32_t vector_slots   = 0; // Position + Parameter vec4s
	uint32_t clip_count     = 0;
	uint32_t cull_count     = 0;

	[[nodiscard]] uint32_t ClipSlots() const { return (clip_count + 3u) / 4u; }
	[[nodiscard]] uint32_t CullSlots() const { return (cull_count + 3u) / 4u; }
	[[nodiscard]] uint32_t VertexSlots() const { return vector_slots + ClipSlots() + CullSlots(); }
	static constexpr uint32_t HeaderDwords = 4u;
	[[nodiscard]] uint32_t PrimitiveDwords() const { return HeaderDwords + max_primitives * 4u; }
	// Dword offset of component `component` of primitive 0.
	[[nodiscard]] static constexpr uint32_t PrimitiveBaseDword(uint32_t component) {
		return HeaderDwords + component;
	}
	[[nodiscard]] uint32_t ItemDwords() const {
		return PrimitiveDwords() + max_vertices * VertexSlots() * 4u;
	}
	[[nodiscard]] uint64_t ItemBytes() const { return uint64_t {ItemDwords()} * 4u; }
	// Dword offset (inside an item) of component `component` of vec4 `slot` of vertex 0.
	[[nodiscard]] uint32_t VertexBaseDword(uint32_t slot, uint32_t component) const {
		return PrimitiveDwords() + slot * 4u + component;
	}
	[[nodiscard]] uint32_t VertexStrideDwords() const { return VertexSlots() * 4u; }
};

inline Layout ComputeLayout(std::span<const ShaderRecompiler::IR::StageOutput> outputs,
                            uint32_t max_vertices, uint32_t max_primitives) {
	using Kind = ShaderRecompiler::IR::StageOutputKind;
	Layout layout;
	layout.max_vertices   = max_vertices;
	layout.max_primitives = max_primitives;
	for (const auto& output: outputs) {
		switch (output.kind) {
			case Kind::Position:
			case Kind::Parameter: layout.vector_slots++; break;
			case Kind::ClipDistance: layout.clip_count = std::max(layout.clip_count, output.index + 1u); break;
			case Kind::CullDistance: layout.cull_count = std::max(layout.cull_count, output.index + 1u); break;
			default: break;
		}
	}
	return layout;
}

inline OutputSlot SlotFor(const Layout&                                         layout,
                          std::span<const ShaderRecompiler::IR::StageOutput> outputs,
                          size_t                                                output_index) {
	using Kind        = ShaderRecompiler::IR::StageOutputKind;
	const auto& entry = outputs[output_index];
	switch (entry.kind) {
		case Kind::Position:
		case Kind::Parameter: {
			uint32_t slot = 0;
			for (size_t i = 0; i < output_index; i++) {
				if (outputs[i].kind == Kind::Position || outputs[i].kind == Kind::Parameter) {
					slot++;
				}
			}
			return {SlotKind::Vector, slot, 0};
		}
		case Kind::ClipDistance:
			return {SlotKind::Clip, layout.vector_slots + entry.index / 4u, entry.index % 4u};
		case Kind::CullDistance:
			return {SlotKind::Cull, layout.vector_slots + layout.ClipSlots() + entry.index / 4u,
			        entry.index % 4u};
		default: return {};
	}
}

} // namespace Libs::Graphics::MeshEmulation

#endif // EMULATOR_SRC_GRAPHICS_SHADER_MESHEMULATION_H_
