#include "graphics/shader/meshEmulationShader.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "graphics/shader/meshEmulation.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"

#include <array>
#include <bit>
#include <cstdlib>

namespace Libs::Graphics {

namespace {

using ShaderRecompiler::Spirv::Builder;
using OutputKind = ShaderRecompiler::IR::StageOutputKind;

class MeshEmulationVertexEmitter {
public:
	MeshEmulationVertexEmitter(std::span<const ShaderRecompiler::IR::StageOutput> outputs_,
	                           const MeshEmulation::Layout&                       layout_)
	    : outputs(outputs_), layout(layout_),
	      descriptor_scratch(Config::MeshScratchDescriptorEnabled()) {}

	std::vector<uint32_t> Emit() {
		builder.AddMemoryModel(descriptor_scratch ? spv::AddressingModelLogical
		                                         : spv::AddressingModelPhysicalStorageBuffer64,
		                       spv::MemoryModelGLSL450);
		builder.RequireCapability(spv::CapabilityShader);
		if (!descriptor_scratch) {
			builder.RequireCapability(spv::CapabilityInt64);
			builder.RequireCapability(spv::CapabilityPhysicalStorageBufferAddresses);
		}
		DefineTypes();
		DefineInterface();

		main = Result(spv::OpFunction, void_type, spv::FunctionControlMaskNone, function_type);
		builder.AddEntryPoint(spv::ExecutionModelVertex, main, "main", interfaces);
		ResultWithoutType(spv::OpLabel);

		const auto vertex_index =
		    Result(spv::OpBitcast, uint_type, Load(int_type, vertex_index_input));
		const auto triangle  = Result(spv::OpUDiv, uint_type, vertex_index, Uint(3));
		const auto corner    = Result(spv::OpUMod, uint_type, vertex_index, Uint(3));
		const auto slot      = Result(spv::OpUDiv, uint_type, triangle, Uint(layout.max_primitives));
		const auto primitive = Result(spv::OpUMod, uint_type, triangle, Uint(layout.max_primitives));

		if (descriptor_scratch) {
			// Keep the base dword index; each read then indexes the runtime array
			// directly. A runtime-array element is a scalar, so treating it as a
			// nested block would generate invalid Metal member accesses.
			item = Result(spv::OpIMul, uint_type, slot, Uint(layout.ItemDwords()));
		} else {
			const auto lo = Result(spv::OpUConvert, u64_type,
			                       PushDword(MeshEmulation::ScratchAddressLoDword));
			const auto hi = Result(spv::OpUConvert, u64_type,
			                       PushDword(MeshEmulation::ScratchAddressHiDword));
			const auto base = Result(
			    spv::OpBitwiseOr, u64_type, lo,
			    Result(spv::OpShiftLeftLogical, u64_type, hi, Uint(32)));
			const auto offset = Result(
			    spv::OpIMul, u64_type, Result(spv::OpUConvert, u64_type, slot),
			    builder.Constant(spv::OpConstant, u64_type,
			                     static_cast<uint32_t>(layout.ItemBytes()),
			                     static_cast<uint32_t>(layout.ItemBytes() >> 32u)));
			item = Result(spv::OpConvertUToPtr, ptr_psb_block,
			              Result(spv::OpIAdd, u64_type, base, offset));
		}

		const auto prim_base = Result(spv::OpIMul, uint_type, primitive, Uint(4));
		const auto PrimitiveDword = [&](uint32_t component) {
			return ReadDword(Result(spv::OpIAdd, uint_type, prim_base,
			                        Uint(MeshEmulation::Layout::PrimitiveBaseDword(component))));
		};
		// Only slots below the written primitive count were stored this pass.
		const auto counted = Result(spv::OpULessThan, bool_type, primitive, ReadDword(Uint(0)));
		std::array<uint32_t, 3> corners {};
		for (uint32_t i = 0; i < 3; i++) {
			corners[i] = PrimitiveDword(i);
		}
		const auto flags = PrimitiveDword(3);
		const auto live  = Result(spv::OpLogicalAnd, bool_type, counted,
		                         Result(spv::OpINotEqual, bool_type,
		                                Result(spv::OpBitwiseAnd, uint_type, flags,
		                                       Uint(MeshEmulation::PrimitiveLiveBit)),
		                                Uint(0)));
		auto vertex = Result(spv::OpSelect, uint_type,
		                     Result(spv::OpIEqual, bool_type, corner, Uint(0)), corners[0],
		                     Result(spv::OpSelect, uint_type,
		                            Result(spv::OpIEqual, bool_type, corner, Uint(1)), corners[1],
		                            corners[2]));
		// Dead primitives may carry garbage indices: read vertex 0 and clip the triangle away.
		vertex = Result(spv::OpSelect, uint_type, live, vertex, Uint(0));
		vertex = Result(spv::OpExtInst, uint_type, glsl, 38u /* UMin */, vertex,
		                Uint(layout.max_vertices - 1u));
		const auto vertex_base = Result(spv::OpIMul, uint_type, vertex, Uint(layout.VertexStrideDwords()));

		const auto ReadVertexDword = [&](uint32_t slot_index, uint32_t component) {
			return ReadDword(Result(spv::OpIAdd, uint_type, vertex_base,
			                        Uint(layout.VertexBaseDword(slot_index, component))));
		};
		const auto ReadVec4 = [&](uint32_t slot_index) {
			std::array<uint32_t, 4> c {};
			for (uint32_t i = 0; i < 4; i++) {
				c[i] = Result(spv::OpBitcast, float_type, ReadVertexDword(slot_index, i));
			}
			return Result(spv::OpCompositeConstruct, vec4_type, c[0], c[1], c[2], c[3]);
		};

		bool wrote_position = false;
		for (size_t i = 0; i < outputs.size(); i++) {
			const auto& output = outputs[i];
			const auto  slot_info = MeshEmulation::SlotFor(layout, outputs, i);
			switch (output.kind) {
				case OutputKind::Position: {
					const auto outside = builder.Constant(
					    spv::OpConstantComposite, vec4_type, Float(2.0f), Float(2.0f), Float(2.0f),
					    Float(1.0f));
					const auto position =
					    Result(spv::OpSelect, vec4_type, live, ReadVec4(slot_info.slot), outside);
					Store(Result(spv::OpAccessChain, ptr_output_vec4, per_vertex, Uint(0)), position);
					wrote_position = true;
					break;
				}
				case OutputKind::Parameter:
					Store(output_variables[i], ReadVec4(slot_info.slot));
					break;
				case OutputKind::ClipDistance:
				case OutputKind::CullDistance: {
					const auto variable = output.kind == OutputKind::ClipDistance ? clip_distance
					                                                               : cull_distance;
					Store(Result(spv::OpAccessChain, ptr_output_float, variable, Uint(output.index)),
					      Result(spv::OpBitcast, float_type,
					             ReadVertexDword(slot_info.slot, slot_info.component)));
					break;
				}
				case OutputKind::Layer:
					Store(layer, Result(spv::OpBitwiseAnd, uint_type, flags,
					                    Uint(MeshEmulation::PrimitiveLayerMask)));
					break;
				default: break;
			}
		}
		if (!wrote_position) {
			// Without a position the guest never rasterizes; keep the triangle off-screen.
			Store(Result(spv::OpAccessChain, ptr_output_vec4, per_vertex, Uint(0)),
			      builder.Constant(spv::OpConstantComposite, vec4_type, Float(2.0f), Float(2.0f),
			                       Float(2.0f), Float(1.0f)));
		}

		builder.AddFunction(spv::OpReturn);
		builder.AddFunction(spv::OpFunctionEnd);
		return builder.Build();
	}

private:
	void DefineTypes() {
		glsl          = builder.Import("GLSL.std.450");
		void_type     = builder.Type(spv::OpTypeVoid);
		bool_type     = builder.Type(spv::OpTypeBool);
		uint_type     = builder.Type(spv::OpTypeInt, 32u, 0u);
		int_type      = builder.Type(spv::OpTypeInt, 32u, 1u);
		u64_type      = builder.Type(spv::OpTypeInt, 64u, 0u);
		float_type    = builder.Type(spv::OpTypeFloat, 32u);
		vec4_type     = builder.Type(spv::OpTypeVector, float_type, 4u);
		function_type = builder.Type(spv::OpTypeFunction, void_type);

		const auto runtime_array = builder.DecoratedType(
		    spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}}, uint_type);
		const auto block = builder.DecoratedType(
		    spv::OpTypeStruct,
		    {{spv::OpMemberDecorate, {0u, spv::DecorationOffset, 0u}},
		     {spv::OpDecorate, {spv::DecorationBlock}}},
		    runtime_array);
		ptr_psb_block = Pointer(spv::StorageClassPhysicalStorageBuffer, block);
		ptr_psb_uint  = Pointer(spv::StorageClassPhysicalStorageBuffer, uint_type);
		if (descriptor_scratch) {
			ptr_scratch_block = Pointer(spv::StorageClassStorageBuffer, block);
			ptr_scratch_uint  = Pointer(spv::StorageClassStorageBuffer, uint_type);
			scratch_variable  = builder.DefineGlobalVariable(ptr_scratch_block,
			                                                spv::StorageClassStorageBuffer);
			builder.AddAnnotation(spv::OpDecorate, scratch_variable,
			                      spv::DecorationDescriptorSet, 0u);
			builder.AddAnnotation(spv::OpDecorate, scratch_variable,
			                      spv::DecorationBinding,
			                      ShaderRecompiler::IR::NativeBinding(
			                          ShaderType::Vertex,
			                          ShaderRecompiler::IR::DescriptorBindingKind::MeshScratch));
		}

		const auto push_array = builder.DecoratedType(
		    spv::OpTypeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}}, uint_type,
		    Uint(ShaderRecompiler::IR::PushData::DwordCount));
		const auto push_block = builder.DecoratedType(
		    spv::OpTypeStruct,
		    {{spv::OpMemberDecorate, {0u, spv::DecorationOffset, 0u}},
		     {spv::OpDecorate, {spv::DecorationBlock}}},
		    push_array);
		push_constants = builder.DefineGlobalVariable(
		    Pointer(spv::StorageClassPushConstant, push_block), spv::StorageClassPushConstant);
		ptr_push_uint = Pointer(spv::StorageClassPushConstant, uint_type);

		ptr_output_vec4  = Pointer(spv::StorageClassOutput, vec4_type);
		ptr_output_float = Pointer(spv::StorageClassOutput, float_type);
	}

	void DefineInterface() {
		vertex_index_input = AddInterface(spv::StorageClassInput, int_type);
		Decorate(vertex_index_input, spv::DecorationBuiltIn, spv::BuiltInVertexIndex);

		const auto per_vertex_type = builder.DecoratedType(
		    spv::OpTypeStruct,
		    {{spv::OpMemberDecorate, {0u, spv::DecorationBuiltIn, spv::BuiltInPosition}},
		     {spv::OpDecorate, {spv::DecorationBlock}}},
		    vec4_type);
		per_vertex = AddInterface(spv::StorageClassOutput, per_vertex_type);

		output_variables.assign(outputs.size(), 0);
		for (size_t i = 0; i < outputs.size(); i++) {
			const auto& output = outputs[i];
			switch (output.kind) {
				case OutputKind::Parameter:
					output_variables[i] = AddInterface(spv::StorageClassOutput, vec4_type);
					Decorate(output_variables[i], spv::DecorationLocation, output.location);
					break;
				case OutputKind::ClipDistance:
					if (clip_distance == 0) {
						builder.RequireCapability(spv::CapabilityClipDistance);
						clip_distance = AddInterface(
						    spv::StorageClassOutput,
						    builder.Type(spv::OpTypeArray, float_type, Uint(layout.clip_count)));
						Decorate(clip_distance, spv::DecorationBuiltIn, spv::BuiltInClipDistance);
					}
					break;
				case OutputKind::CullDistance:
					if (cull_distance == 0) {
						builder.RequireCapability(spv::CapabilityCullDistance);
						cull_distance = AddInterface(
						    spv::StorageClassOutput,
						    builder.Type(spv::OpTypeArray, float_type, Uint(layout.cull_count)));
						Decorate(cull_distance, spv::DecorationBuiltIn, spv::BuiltInCullDistance);
					}
					break;
				case OutputKind::Layer:
					if (layer == 0) {
						builder.RequireCapability(spv::CapabilityShaderLayer);
						layer = AddInterface(spv::StorageClassOutput, uint_type);
						Decorate(layer, spv::DecorationBuiltIn, spv::BuiltInLayer);
					}
					break;
				default: break;
			}
		}
	}

	uint32_t PushDword(uint32_t index) {
		return Load(uint_type, Result(spv::OpAccessChain, ptr_push_uint, push_constants, Uint(0),
		                              Uint(index)));
	}

	uint32_t ReadDword(uint32_t dword) {
		uint32_t pointer = 0;
		if (descriptor_scratch) {
			const auto index = Result(spv::OpIAdd, uint_type, item, dword);
			pointer = Result(spv::OpAccessChain, ptr_scratch_uint, scratch_variable,
			                 Uint(0), index);
		} else {
			pointer = Result(spv::OpAccessChain, ptr_psb_uint, item, Uint(0), dword);
		}
		const auto value   = builder.AllocateId();
		builder.AddFunction(spv::OpLoad, uint_type, value, pointer, spv::MemoryAccessAlignedMask, 4u);
		return value;
	}

	uint32_t Pointer(spv::StorageClass storage, uint32_t type) {
		return builder.Type(spv::OpTypePointer, storage, type);
	}

	template <typename... Args>
	uint32_t Result(spv::Op opcode, uint32_t type, Args... operands) {
		const auto id = builder.AllocateId();
		builder.AddFunction(opcode, type, id, operands...);
		return id;
	}

	template <typename... Args>
	uint32_t ResultWithoutType(spv::Op opcode, Args... operands) {
		const auto id = builder.AllocateId();
		builder.AddFunction(opcode, id, operands...);
		return id;
	}

	uint32_t Load(uint32_t type, uint32_t pointer) { return Result(spv::OpLoad, type, pointer); }
	void     Store(uint32_t pointer, uint32_t value) { builder.AddFunction(spv::OpStore, pointer, value); }
	uint32_t Uint(uint32_t value) { return builder.Constant(spv::OpConstant, uint_type, value); }
	uint32_t Float(float value) {
		return builder.Constant(spv::OpConstant, float_type, std::bit_cast<uint32_t>(value));
	}

	uint32_t AddInterface(spv::StorageClass storage, uint32_t type) {
		const auto variable = builder.DefineGlobalVariable(Pointer(storage, type), storage);
		interfaces.push_back(variable);
		return variable;
	}

	void Decorate(uint32_t target, spv::Decoration decoration, uint32_t value) {
		builder.AddAnnotation(spv::OpDecorate, target, decoration, value);
	}

	std::span<const ShaderRecompiler::IR::StageOutput> outputs;
	const MeshEmulation::Layout&                       layout;
	bool                                               descriptor_scratch = false;
	Builder                                            builder {0x00010500u};
	std::vector<uint32_t>                              interfaces;
	std::vector<uint32_t>                              output_variables;
	uint32_t main = 0, glsl = 0, void_type = 0, bool_type = 0, uint_type = 0, int_type = 0;
	uint32_t u64_type = 0, float_type = 0, vec4_type = 0, function_type = 0;
	uint32_t ptr_psb_block = 0, ptr_psb_uint = 0;
	uint32_t ptr_scratch_block = 0, ptr_scratch_uint = 0, ptr_push_uint = 0;
	uint32_t ptr_output_vec4 = 0, ptr_output_float = 0;
	uint32_t push_constants = 0, vertex_index_input = 0, per_vertex = 0;
	uint32_t clip_distance = 0, cull_distance = 0, layer = 0, item = 0, scratch_variable = 0;
};

} // namespace

std::vector<uint32_t> BuildMeshEmulationVertexShader(
    std::span<const ShaderRecompiler::IR::StageOutput> outputs, uint32_t max_vertices,
    uint32_t max_primitives) {
	EXIT_IF(max_vertices == 0 || max_primitives == 0);
	const auto layout = MeshEmulation::ComputeLayout(outputs, max_vertices, max_primitives);
	MeshEmulationVertexEmitter emitter(outputs, layout);
	return emitter.Emit();
}

} // namespace Libs::Graphics
