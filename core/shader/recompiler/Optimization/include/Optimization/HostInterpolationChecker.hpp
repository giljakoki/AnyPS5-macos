#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_HOSTINTERPOLATIONCHECKER_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_HOSTINTERPOLATIONCHECKER_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include <string>

namespace ShaderRecompiler {

class HostInterpolationChecker {
public:
    [[nodiscard]] bool Lower(IrProgram& program, const ShaderPixelInputInfo& pixel, std::string& reason) const;
};

}

#endif
