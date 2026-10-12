#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_WAVEUNIFORMITY_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_WAVEUNIFORMITY_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include <unordered_set>

namespace ShaderRecompiler {

[[nodiscard]] std::unordered_set<const IrValue*> WaveUniformValues(const IrProgram& program);
[[nodiscard]] bool IsWaveMaskBranch(BranchCondition condition);
[[nodiscard]] bool IsWaveMaskBranch(const BlockInfo& info);

}

#endif
