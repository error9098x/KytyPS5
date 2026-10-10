#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"
#include "graphics/shader/meshEmulation.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t MeshArray(EmitterState& state, spv::StorageClass storage, uint32_t type, uint32_t count) {
	const auto array = state.builder.Type(spv::OpTypeArray, type, ConstantU32(state, count));
	return state.builder.DefineGlobalVariable(TypePointer(state, storage, array), storage);
}

uint32_t MeshElement(EmitterState& state, uint32_t variable, spv::StorageClass storage,
                     uint32_t type, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePointer(state, storage, type), pointer,
	                          variable, index);
	return pointer;
}

uint32_t MeshLoad(EmitterState& state, uint32_t variable, spv::StorageClass storage, uint32_t type,
                  uint32_t index) {
	const auto pointer = MeshElement(state, variable, storage, type, index);
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, type, value, pointer);
	return value;
}

uint32_t MeshOutputType(EmitterState& state, IR::StageOutputKind kind) {
	switch (kind) {
		case IR::StageOutputKind::Layer: return TypeU32(state);
		case IR::StageOutputKind::ClipDistance:
		case IR::StageOutputKind::CullDistance: return TypeF32(state);
		case IR::StageOutputKind::Position:
		case IR::StageOutputKind::Parameter: return TypeF32Vector(state, 4);
		default: EXIT("unsupported mesh output kind=%u\n", static_cast<uint32_t>(kind));
	}
}

} // namespace

void DefineMeshOutputs(EmitterState& state, uint32_t clip_distance_count,
                       uint32_t cull_distance_count) {
	const auto& mesh = state.input_info.vertex->mesh;
	for (auto& output: state.outputs) {
		const auto type = MeshOutputType(state, output.kind);
		const bool clip = output.kind == IR::StageOutputKind::ClipDistance;
		const bool cull = output.kind == IR::StageOutputKind::CullDistance;
		auto& variable = clip ? state.clip_distance_variable
		                 : cull ? state.cull_distance_variable : output.variable_id;
		if (variable == 0 && !state.mesh_compute) {
			const auto element_type = clip || cull
			                              ? state.builder.Type(spv::OpTypeArray, type,
			                                                   ConstantU32(state, clip ? clip_distance_count
			                                                                           : cull_distance_count))
			                              : type;
			variable = MeshArray(state, spv::StorageClassOutput, element_type,
			                     output.kind == IR::StageOutputKind::Layer ? mesh.max_primitives
			                                                             : mesh.max_vertices);
			state.interface_variables.push_back(variable);
			state.builder.AddName(variable, output.debug_name.c_str());
			if (output.kind == IR::StageOutputKind::Parameter) {
				state.builder.AddAnnotation(spv::OpDecorate, variable,
				                            spv::DecorationLocation, output.location);
			} else {
				const auto builtin = clip ? spv::BuiltInClipDistance
				                     : cull ? spv::BuiltInCullDistance
				                     : output.kind == IR::StageOutputKind::Layer ? spv::BuiltInLayer
				                                                                 : spv::BuiltInPosition;
				state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn, builtin);
			}
			if (output.kind == IR::StageOutputKind::Layer) {
				state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationPerPrimitiveEXT);
			}
		}
		output.variable_id = variable;
		// Only Layer is read by another invocation, through the primitive's provoking vertex.
		const bool shared = output.kind == IR::StageOutputKind::Layer;
		output.mesh_data_variable =
		    MeshArray(state, shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, type,
		              shared ? mesh.max_vertices : state.lane_count);
	}
	state.mesh_allocation = MeshArray(state, spv::StorageClassWorkgroup, TypeU32(state), 2);
	state.mesh_primitive_data =
	    MeshArray(state, spv::StorageClassPrivate, TypeU32(state), state.lane_count);
	if (state.mesh_compute) {
		return;
	}
	state.mesh_primitives =
	    MeshArray(state, spv::StorageClassOutput, TypeU32Vector(state, 3), mesh.max_primitives);
	state.mesh_cull =
	    MeshArray(state, spv::StorageClassOutput, TypeBool(state), mesh.max_primitives);
	state.interface_variables.push_back(state.mesh_primitives);
	state.interface_variables.push_back(state.mesh_cull);
	state.builder.AddAnnotation(
	    spv::OpDecorate, state.mesh_primitives, spv::DecorationBuiltIn,
	    spv::BuiltInPrimitiveTriangleIndicesEXT); // PrimitiveTriangleIndicesEXT
	state.builder.AddAnnotation(spv::OpDecorate, state.mesh_cull, spv::DecorationBuiltIn,
	                            spv::BuiltInCullPrimitiveEXT); // CullPrimitiveEXT
	state.builder.AddAnnotation(spv::OpDecorate, state.mesh_cull, spv::DecorationPerPrimitiveEXT);
}

uint32_t MeshOutputPointer(EmitterState& state, IR::StageOutputKind kind, uint32_t index) {
	const auto output = std::ranges::find_if(state.outputs, [=](const OutputBinding& binding) {
		return binding.kind == kind && binding.index == index;
	});
	if (output == state.outputs.end()) {
		EXIT("mesh export has no output binding: kind=%u index=%u\n", static_cast<uint32_t>(kind),
		     index);
	}
	const bool shared = kind == IR::StageOutputKind::Layer;
	return MeshElement(
	    state, output->mesh_data_variable,
	    shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, MeshOutputType(state, kind),
	    shared ? EmitLocalInvocationIndex(state) : ConstantU32(state, state.lane_half));
}

uint32_t MeshPrimitivePointer(EmitterState& state) {
	return MeshElement(state, state.mesh_primitive_data, spv::StorageClassPrivate, TypeU32(state),
	                   ConstantU32(state, state.lane_half));
}

void EmitMeshAllocate(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state = ctx.state;
	const auto first = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpIEqual, TypeBool(state), first,
	                          EmitLocalInvocationIndex(state), ConstantU32(state, 0));
	EmitIfCondition(state, first, [&] {
		const auto allocation = ctx.Arg(inst, 0);
		for (uint32_t field = 0; field < 2; field++) {
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpBitFieldUExtract, TypeU32(state), value, allocation,
			                          ConstantU32(state, field * 12u),
			                          ConstantU32(state, field == 0 ? 10u : 11u));
			const auto pointer =
			    MeshElement(state, state.mesh_allocation, spv::StorageClassWorkgroup,
			                TypeU32(state), ConstantU32(state, field));
			state.builder.AddFunction(spv::OpStore, pointer, value);
		}
	});
}

namespace {

// Pointer to dword `dword` of the scratch item, reached through a buffer device address.
struct ScratchWriter {
	EmitterState& state;
	uint32_t      item_address = 0; // u64
	uint32_t      element_pointer_type = 0;
	uint32_t      array_pointer_type = 0;

	explicit ScratchWriter(EmitterState& state_): state(state_) {
		const auto u32           = TypeU32(state);
		const auto runtime_array = state.builder.DecoratedType(
		    spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}}, u32);
		const auto block = state.builder.DecoratedType(
		    spv::OpTypeStruct,
		    {{spv::OpMemberDecorate, {0u, spv::DecorationOffset, 0u}},
		     {spv::OpDecorate, {spv::DecorationBlock}}},
		    runtime_array);
		array_pointer_type   = TypePointer(state, spv::StorageClassPhysicalStorageBuffer, block);
		element_pointer_type = TypePointer(state, spv::StorageClassPhysicalStorageBuffer, u32);
	}

	uint32_t PushDword(uint32_t index) {
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain, TypePushConstantElementPointer(state),
		                          pointer, state.push_constant_variable, ConstantU32(state, 0),
		                          ConstantU32(state, index));
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
		return value;
	}

	void ComputeItemAddress(const MeshEmulation::Layout& layout) {
		const auto u64  = TypeU64(state);
		const auto lo   = Unary(state, spv::OpUConvert, u64,
		                        PushDword(MeshEmulation::ScratchAddressLoDword));
		const auto hi   = Unary(state, spv::OpUConvert, u64,
		                        PushDword(MeshEmulation::ScratchAddressHiDword));
		const auto base = Binary(state, spv::OpBitwiseOr, u64, lo,
		                         Binary(state, spv::OpShiftLeftLogical, u64, hi, ConstantU32(state, 32)));
		// slot = WorkgroupId.y * chunk_groups + (WorkgroupId.x - group_base)
		const auto group_x = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 0);
		const auto group_y = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 1);
		const auto local_x = EmitBinaryU32(state, spv::OpISub, group_x,
		                                   PushDword(MeshEmulation::GroupBaseDword));
		const auto slot    = EmitBinaryU32(
		    state, spv::OpIAdd, local_x,
		    EmitBinaryU32(state, spv::OpIMul, group_y, PushDword(MeshEmulation::ChunkGroupsDword)));
		const auto offset  = Binary(state, spv::OpIMul, u64, Unary(state, spv::OpUConvert, u64, slot),
		                            ConstantU64(state, layout.ItemBytes()));
		item_address       = Binary(state, spv::OpIAdd, u64, base, offset);
	}

	void Store(uint32_t dword, uint32_t value_u32) {
		const auto array = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpConvertUToPtr, array_pointer_type, array, item_address);
		const auto element = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain, element_pointer_type, element, array,
		                          ConstantU32(state, 0), dword);
		state.builder.AddFunction(spv::OpStore, element, value_u32, spv::MemoryAccessAlignedMask, 4u);
	}
};

uint32_t DwordIndex(EmitterState& state, uint32_t base, uint32_t index, uint32_t stride) {
	return EmitBinaryU32(state, spv::OpIAdd, ConstantU32(state, base),
	                     EmitBinaryU32(state, spv::OpIMul, index, ConstantU32(state, stride)));
}

void EmitMeshComputeEntryPoint(EmitterState& state) {
	const auto& mesh    = state.input_info.vertex->mesh;
	std::vector<IR::StageOutput> outputs(state.outputs.begin(), state.outputs.end());
	const auto layout   = MeshEmulation::ComputeLayout(outputs, mesh.max_vertices, mesh.max_primitives);

	state.builder.AddFunction(spv::OpFunction, TypeVoid(state), state.main_func,
	                          spv::FunctionControlMaskNone, TypeFunction(state));
	EmitLabel(state, state.builder.AllocateId());
	state.builder.AddFunction(spv::OpFunctionCall, TypeVoid(state), state.builder.AllocateId(),
	                          state.mesh_guest_func);
	EmitBarrier(state);
	const auto vertices   = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 0));
	const auto primitives = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 1));
	ScratchWriter writer(state);
	writer.ComputeItemAddress(layout);
	const auto vertex_stride = layout.VertexStrideDwords();
	for (uint32_t half = 0; half < state.lane_count; half++) {
		state.lane_half      = half;
		const auto index     = EmitLocalInvocationIndex(state);
		const auto is_vertex = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_vertex, index, vertices);
		EmitIfCondition(state, is_vertex, [&] {
			for (size_t i = 0; i < state.outputs.size(); i++) {
				const auto& output = state.outputs[i];
				const auto  slot   = MeshEmulation::SlotFor(layout, outputs, i);
				if (slot.kind == MeshEmulation::SlotKind::None) {
					continue;
				}
				const auto type  = MeshOutputType(state, output.kind);
				const auto value = MeshLoad(state, output.mesh_data_variable,
				                            spv::StorageClassPrivate, type, ConstantU32(state, half));
				if (slot.kind == MeshEmulation::SlotKind::Vector) {
					for (uint32_t c = 0; c < 4; c++) {
						const auto component = state.builder.AllocateId();
						state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), component,
						                          value, c);
						writer.Store(DwordIndex(state, layout.VertexBaseDword(slot.slot, c), index,
						                        vertex_stride),
						             Unary(state, spv::OpBitcast, TypeU32(state), component));
					}
				} else {
					writer.Store(DwordIndex(state, layout.VertexBaseDword(slot.slot, slot.component),
					                        index, vertex_stride),
					             Unary(state, spv::OpBitcast, TypeU32(state), value));
				}
			}
		});
		// Every primitive slot is written (live or dead) because the scratch item is reused.
		const auto is_slot = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_slot, index,
		                          ConstantU32(state, mesh.max_primitives));
		EmitIfCondition(state, is_slot, [&] {
			const auto is_primitive = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_primitive, index,
			                          primitives);
			const auto packed = MeshLoad(state, state.mesh_primitive_data, spv::StorageClassPrivate,
			                             TypeU32(state), ConstantU32(state, half));
			uint32_t   vertex[3] {};
			for (uint32_t component = 0; component < 3; component++) {
				vertex[component] = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpBitFieldUExtract, TypeU32(state), vertex[component],
				                          packed, ConstantU32(state, component * 10u),
				                          ConstantU32(state, 10));
			}
			const auto null_bit =
			    EmitBinaryU32(state, spv::OpBitwiseAnd, packed, ConstantU32(state, 0x80000000u));
			const auto not_culled = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpIEqual, TypeBool(state), not_culled, null_bit,
			                          ConstantU32(state, 0));
			const auto live = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpLogicalAnd, TypeBool(state), live, is_primitive,
			                          not_culled);
			auto flags = Select(state, TypeU32(state), live,
			                    ConstantU32(state, MeshEmulation::PrimitiveLiveBit),
			                    ConstantU32(state, 0));
			for (const auto& output: state.outputs) {
				if (output.kind != IR::StageOutputKind::Layer) {
					continue;
				}
				// The provoking vertex index is only meaningful for live primitives.
				const auto provoking = Select(state, TypeU32(state), live,
				                              vertex[mesh.provoking_vertex], ConstantU32(state, 0));
				const auto layer = MeshLoad(state, output.mesh_data_variable,
				                            spv::StorageClassWorkgroup, TypeU32(state), provoking);
				flags = EmitBinaryU32(
				    state, spv::OpBitwiseOr, flags,
				    EmitBinaryU32(state, spv::OpBitwiseAnd, layer,
				                  ConstantU32(state, MeshEmulation::PrimitiveLayerMask)));
			}
			for (uint32_t component = 0; component < 3; component++) {
				writer.Store(DwordIndex(state, MeshEmulation::Layout::PrimitiveBaseDword(component),
				                        index, 4u),
				             vertex[component]);
			}
			writer.Store(DwordIndex(state, MeshEmulation::Layout::PrimitiveBaseDword(3u), index, 4u),
			             flags);
		});
		const auto is_first = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpIEqual, TypeBool(state), is_first, index,
		                          ConstantU32(state, 0));
		EmitIfCondition(state, is_first,
		                [&] { writer.Store(ConstantU32(state, 0), primitives); });
	}
	state.lane_half = 0;
	state.builder.AddFunction(spv::OpReturn);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

} // namespace

void EmitMeshEntryPoint(EmitterState& state) {
	if (state.mesh_compute) {
		EmitMeshComputeEntryPoint(state);
		return;
	}
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state), state.main_func,
	                          spv::FunctionControlMaskNone, TypeFunction(state));
	EmitLabel(state, state.builder.AllocateId());
	state.builder.AddFunction(spv::OpFunctionCall, TypeVoid(state), state.builder.AllocateId(),
	                          state.mesh_guest_func);
	// All guest waves finish before the uniform Vulkan allocation and output stores.
	EmitBarrier(state);
	const auto vertices   = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 0));
	const auto primitives = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 1));
	state.builder.AddFunction(spv::OpSetMeshOutputsEXT, vertices,
	                          primitives); // OpSetMeshOutputsEXT
	for (uint32_t half = 0; half < state.lane_count; half++) {
		state.lane_half      = half;
		const auto index     = EmitLocalInvocationIndex(state);
		const auto is_vertex = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_vertex, index, vertices);
		EmitIfCondition(state, is_vertex, [&] {
			for (const auto& output: state.outputs) {
				if (output.kind == IR::StageOutputKind::Layer) {
					continue;
				}
				const auto type  = MeshOutputType(state, output.kind);
				const auto value =
				    MeshLoad(state, output.mesh_data_variable, spv::StorageClassPrivate, type,
				             ConstantU32(state, half));
				uint32_t pointer;
				if (output.kind == IR::StageOutputKind::ClipDistance ||
				    output.kind == IR::StageOutputKind::CullDistance) {
					pointer = state.builder.AllocateId();
					state.builder.AddFunction(
					    spv::OpAccessChain, TypePointer(state, spv::StorageClassOutput, type), pointer,
					    output.variable_id, index, ConstantU32(state, output.index));
				} else {
					pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput, type, index);
				}
				state.builder.AddFunction(spv::OpStore, pointer, value);
			}
		});
		const auto is_primitive = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_primitive, index,
		                          primitives);
		EmitIfCondition(state, is_primitive, [&] {
			const auto packed = MeshLoad(state, state.mesh_primitive_data, spv::StorageClassPrivate,
			                             TypeU32(state), ConstantU32(state, half));
			uint32_t   vertex[3] {};
			for (uint32_t component = 0; component < 3; component++) {
				vertex[component] = state.builder.AllocateId();
				state.builder.AddFunction(
				    spv::OpBitFieldUExtract, TypeU32(state), vertex[component], packed,
				    ConstantU32(state, component * 10u), ConstantU32(state, 10));
			}
			const auto triangle = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 3), triangle,
			                          vertex[0], vertex[1], vertex[2]);
			const auto triangle_pointer =
			    MeshElement(state, state.mesh_primitives, spv::StorageClassOutput,
			                TypeU32Vector(state, 3), index);
			state.builder.AddFunction(spv::OpStore, triangle_pointer, triangle);
			const auto null_bit =
			    EmitBinaryU32(state, spv::OpBitwiseAnd, packed, ConstantU32(state, 0x80000000u));
			const auto culled = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), culled, null_bit,
			                          ConstantU32(state, 0));
			state.builder.AddFunction(spv::OpStore,
			                          MeshElement(state, state.mesh_cull, spv::StorageClassOutput,
			                                      TypeBool(state), index),
			                          culled);
			for (const auto& output: state.outputs) {
				if (output.kind != IR::StageOutputKind::Layer) {
					continue;
				}
				const auto layer = MeshLoad(state, output.mesh_data_variable,
				                            spv::StorageClassWorkgroup, TypeU32(state),
				                            vertex[state.input_info.vertex->mesh.provoking_vertex]);
				const auto pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput,
				                                 TypeU32(state), index);
				state.builder.AddFunction(spv::OpStore, pointer, layer);
			}
		});
	}
	state.lane_half = 0;
	state.builder.AddFunction(spv::OpReturn);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
