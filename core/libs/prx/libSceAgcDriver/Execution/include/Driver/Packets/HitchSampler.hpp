#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_PACKETS_HITCHSAMPLER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_PACKETS_HITCHSAMPLER_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace AgcDriver::DriverDetail {

struct ThreadSample {
    std::chrono::steady_clock::time_point at;
    std::array<std::uint64_t, 6> frames{};
};

std::string SummarizeSamples(const std::vector<ThreadSample>& samples, const std::function<std::string(std::uint64_t)>& symbolize, std::size_t shown);

class HitchSampler {
public:
    static HitchSampler* Get();

    virtual ~HitchSampler() = default;
    virtual void Follow() = 0;
    virtual std::string Describe(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) = 0;
    virtual std::string Period() = 0;
};

}

#endif
