#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_MASKEDSELECTELIMINATOR_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_MASKEDSELECTELIMINATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include <cstddef>
#include <cstdint>

namespace ShaderRecompiler {

struct MaskedSelectEliminationStats {
    std::uint32_t removedSelects = 0;
};

class MaskedSelectEliminator {
public:
    [[nodiscard]] MaskedSelectEliminationStats Eliminate(IrProgram& program) const;
};

[[nodiscard]] bool PhiIncomingUnobserved(const IrProgram& program, const IrValue& phi, std::size_t index);

}

#endif
