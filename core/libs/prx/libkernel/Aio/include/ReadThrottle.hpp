#ifndef CORE_LIBS_PRX_LIBKERNEL_AIO_INCLUDE_READTHROTTLE_HPP
#define CORE_LIBS_PRX_LIBKERNEL_AIO_INCLUDE_READTHROTTLE_HPP

#include <cstdint>

namespace ReadThrottle {

struct Settings {
    std::uint64_t microsPerMiB = 0;
    double windowSeconds = 0.0;
    std::uint64_t bytesPerFlip = 0;
};

constexpr std::uint64_t FlipHeadroom = 2;
constexpr unsigned FlipStallMillis = 100;

inline bool FlipAllows(std::uint64_t bytesPerFlip, std::uint64_t consumed, std::uint64_t flipsSinceBase) {
    return bytesPerFlip == 0 || consumed < bytesPerFlip * (flipsSinceBase + FlipHeadroom);
}

inline std::uint64_t DelayMicros(const Settings& settings, std::uint64_t bytes, double elapsedSeconds, double& carryMicros) {
    if (settings.microsPerMiB == 0 || elapsedSeconds >= settings.windowSeconds || bytes == 0) return 0;
    carryMicros += static_cast<double>(bytes) * static_cast<double>(settings.microsPerMiB) / (1024.0 * 1024.0);
    const auto whole = static_cast<std::uint64_t>(carryMicros);
    carryMicros -= static_cast<double>(whole);
    return whole;
}

struct PacingTotals {
    std::uint64_t waiting = 0;
    std::uint64_t heldReads = 0;
    std::uint64_t releasedByFlip = 0;
    std::uint64_t stalls = 0;
};

Settings Current();
void After(std::uint64_t bytes);
std::uint64_t Flips();
PacingTotals Totals();
void SetFlipStallMillis(unsigned millis);

}

extern "C" void LoaderFlipCompleted_nid_no_patch();

namespace ReadThrottle {

}

#endif
