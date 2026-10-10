#ifndef EMULATOR_SRC_GRAPHICS_SHADER_MESHEMULATION_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_MESHEMULATION_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <cstdint>
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
