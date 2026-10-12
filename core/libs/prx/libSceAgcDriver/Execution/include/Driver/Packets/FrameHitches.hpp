#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_PACKETS_FRAMEHITCHES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_PACKETS_FRAMEHITCHES_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace AgcDriver::DriverDetail {

struct HitchSubmission {
    std::chrono::steady_clock::time_point entered;
    std::chrono::steady_clock::time_point left;
    double flipRoomMs = 0;
    double workerHoldMs = 0;
};

struct HitchWork {
    std::uint64_t serial = 0;
    double busyMs = 0;
    double waitMs = 0;
    std::string slowest;
    double slowestMs = 0;
    std::string longestWait;
    double longestWaitMs = 0;
};

class FrameHitchTracker {
public:
    explicit FrameHitchTracker(double thresholdMs);

    using TitleDescriber = std::function<std::string(std::chrono::steady_clock::time_point, std::chrono::steady_clock::time_point)>;

    std::optional<std::string> FlipSubmitted(const HitchSubmission& submission, const TitleDescriber& describeTitle = {});
    void OtherSubmitted();
    std::optional<std::string> WorkFinished(const HitchWork& work);

private:
    std::mutex mutex;
    double thresholdMs;
    std::uint64_t frames = 0;
    std::optional<HitchSubmission> previous;
    std::uint64_t otherSubmissions = 0;
    std::uint64_t workSubmissions = 0;
    double workBusyMs = 0;
    double workWaitMs = 0;
    HitchWork longestWork;
};

double ParseHitchThresholdMs(const char* text);

FrameHitchTracker* FrameHitches();

}

#endif
