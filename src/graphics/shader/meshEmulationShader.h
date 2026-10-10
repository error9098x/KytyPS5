#ifndef EMULATOR_SRC_GRAPHICS_SHADER_MESHEMULATIONSHADER_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_MESHEMULATIONSHADER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

// Vertex shader that draws the triangles a mesh-emulation compute pass wrote to scratch memory.
// Draw it non-indexed with `items * max_primitives * 3` vertices; see meshEmulation.h.
std::vector<uint32_t> BuildMeshEmulationVertexShader(
    std::span<const ShaderRecompiler::IR::StageOutput> outputs, uint32_t max_vertices,
    uint32_t max_primitives);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_SHADER_MESHEMULATIONSHADER_H_
