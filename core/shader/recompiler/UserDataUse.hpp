#ifndef CORE_SHADER_RECOMPILER_USERDATAUSE_HPP
#define CORE_SHADER_RECOMPILER_USERDATAUSE_HPP

#include <cstdint>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

// How a resource walk used its user-data words (the first TrackedWords): `asBase` words were only
// the base of a raw read's handle (a pointer the walk dereferenced), `asValue` words were evaluated
// as values (anything else, so their numeric value may be in the result); `reads` lists every raw
// read's address with the user word its handle's base came from (-1 when not one).
struct SrtUserDataUse {
    static constexpr std::uint32_t TrackedWords = 64;
    std::uint64_t asBase = 0;
    std::uint64_t asValue = 0;
    std::vector<std::pair<std::uint64_t, std::int32_t>> reads;
};

}

#endif
