#include "prx/libSceAgcDriver/Execution/include/Driver/Packets/FrameHitches.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace AgcDriver::DriverDetail {

namespace {

double Ms(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

}

FrameHitchTracker::FrameHitchTracker(double thresholdMs) : thresholdMs(thresholdMs) {}

std::optional<std::string> FrameHitchTracker::FlipSubmitted(const HitchSubmission& submission) {
    std::lock_guard lock(mutex);
    ++frames;
    std::optional<std::string> report;
    if (previous.has_value()) {
        const auto periodMs = Ms(submission.entered - previous->entered);
        if (periodMs >= thresholdMs) {
            char text[640];
            std::snprintf(text, sizeof(text), "[hitch] frame %llu: %.1f ms between flip submissions: %.1f ms in the title since the previous one returned, which blocked %.1f ms (flip room %.1f ms, behind the worker %.1f ms); %llu other submissions; queue 0 worker in between: %llu submissions, %.1f ms of packets, %.1f ms waiting, longest submission %llu with %.1f ms of packets (slowest %s %.1f ms)\n", static_cast<unsigned long long>(frames), periodMs, Ms(submission.entered - previous->left), Ms(previous->left - previous->entered), previous->flipRoomMs, previous->workerHoldMs, static_cast<unsigned long long>(otherSubmissions), static_cast<unsigned long long>(workSubmissions), workBusyMs, workWaitMs, static_cast<unsigned long long>(longestWork.serial), longestWork.busyMs, longestWork.slowest.empty() ? "none" : longestWork.slowest.c_str(), longestWork.slowestMs);
            report = text;
        }
    }
    previous = submission;
    otherSubmissions = 0;
    workSubmissions = 0;
    workBusyMs = 0;
    workWaitMs = 0;
    longestWork = {};
    return report;
}

void FrameHitchTracker::OtherSubmitted() {
    std::lock_guard lock(mutex);
    ++otherSubmissions;
}

std::optional<std::string> FrameHitchTracker::WorkFinished(const HitchWork& work) {
    std::lock_guard lock(mutex);
    ++workSubmissions;
    workBusyMs += work.busyMs;
    workWaitMs += work.waitMs;
    if (work.busyMs >= longestWork.busyMs) longestWork = work;
    if (work.busyMs < thresholdMs) return std::nullopt;
    char text[320];
    std::snprintf(text, sizeof(text), "[hitch] queue 0 submission %llu: %.1f ms of packets, %.1f ms waiting; slowest %s %.1f ms\n", static_cast<unsigned long long>(work.serial), work.busyMs, work.waitMs, work.slowest.empty() ? "none" : work.slowest.c_str(), work.slowestMs);
    return std::string(text);
}

double ParseHitchThresholdMs(const char* text) {
    if (text == nullptr) return 0;
    errno = 0;
    char* end = nullptr;
    const auto value = std::strtod(text, &end);
    if (end == text || *end != '\0' || errno != 0 || !(value > 0)) throw std::invalid_argument(std::string("APS5_TRACE_HITCHES must be a frame period in milliseconds above zero, not '") + text + "'");
    return value;
}

FrameHitchTracker* FrameHitches() {
    static const double threshold = ParseHitchThresholdMs(std::getenv("APS5_TRACE_HITCHES"));
    if (threshold == 0) return nullptr;
    static FrameHitchTracker tracker(threshold);
    return &tracker;
}

}
