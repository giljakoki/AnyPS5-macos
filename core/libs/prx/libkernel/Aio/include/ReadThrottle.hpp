#ifndef CORE_LIBS_PRX_LIBKERNEL_AIO_INCLUDE_READTHROTTLE_HPP
#define CORE_LIBS_PRX_LIBKERNEL_AIO_INCLUDE_READTHROTTLE_HPP

#include <cstdint>

namespace ReadThrottle {

struct Settings {
    std::uint64_t microsPerMiB = 0;
    double windowSeconds = 0.0;
};

inline std::uint64_t DelayMicros(const Settings& settings, std::uint64_t bytes, double elapsedSeconds, double& carryMicros) {
    if (settings.microsPerMiB == 0 || elapsedSeconds >= settings.windowSeconds || bytes == 0) return 0;
    carryMicros += static_cast<double>(bytes) * static_cast<double>(settings.microsPerMiB) / (1024.0 * 1024.0);
    const auto whole = static_cast<std::uint64_t>(carryMicros);
    carryMicros -= static_cast<double>(whole);
    return whole;
}

Settings Current();
void After(std::uint64_t bytes);

}

#endif
