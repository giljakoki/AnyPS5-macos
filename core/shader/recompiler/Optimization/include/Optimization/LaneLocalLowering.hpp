#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_LANELOCALLOWERING_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_LANELOCALLOWERING_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace ShaderRecompiler {

struct SubgroupUse {
    std::string description;
    std::uint32_t startPc = 0;
    std::uint32_t endPc = 0;
};

[[nodiscard]] std::vector<SubgroupUse> DescribeSubgroupUses(const IrProgram& program);

class LaneLocalLowering {
public:
    [[nodiscard]] std::vector<SubgroupUse> Lower(IrProgram& program, std::uint32_t lanes) const;
};

}

#endif
