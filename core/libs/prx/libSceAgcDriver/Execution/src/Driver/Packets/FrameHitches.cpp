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

std::optional<std::string> FrameHitchTracker::FlipSubmitted(const HitchSubmission& submission, const TitleDescriber& describeTitle) {
    std::lock_guard lock(mutex);
    ++frames;
    std::optional<std::string> report;
    if (previous.has_value()) {
        const auto periodMs = Ms(submission.entered - previous->entered);
        if (periodMs >= thresholdMs) {
            char text[640];
            std::snprintf(text, sizeof(text), "[hitch] frame %llu: %.1f ms between flip submissions: %.1f ms in the title since the previous one returned, which blocked %.1f ms (flip room %.1f ms, behind the worker %.1f ms); %llu other submissions; queue 0 worker in between: %llu submissions, %.1f ms of packets, %.1f ms waiting, longest submission %llu with %.1f ms of packets (slowest %s %.1f ms), %llu idle stores in %.1f ms\n", static_cast<unsigned long long>(frames), periodMs, Ms(submission.entered - previous->left), Ms(previous->left - previous->entered), previous->flipRoomMs, previous->workerHoldMs, static_cast<unsigned long long>(otherSubmissions), static_cast<unsigned long long>(workSubmissions), workBusyMs, workWaitMs, static_cast<unsigned long long>(longestWork.serial), longestWork.busyMs, longestWork.slowest.empty() ? "none" : longestWork.slowest.c_str(), longestWork.slowestMs, static_cast<unsigned long long>(idleStores), idleStoreMs);
            report = text;
            const auto titleMs = Ms(submission.entered - previous->left);
            if (describeTitle && titleMs >= thresholdMs / 2) {
                report->pop_back();
                *report += "; title thread: " + describeTitle(previous->left, submission.entered) + "\n";
            }
        }
    }
    previous = submission;
    otherSubmissions = 0;
    workSubmissions = 0;
    workBusyMs = 0;
    workWaitMs = 0;
    longestWork = {};
    idleStores = 0;
    idleStoreMs = 0;
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
    if (work.busyMs < thresholdMs && work.longestWaitMs < thresholdMs) return std::nullopt;
    char text[384];
    std::snprintf(text, sizeof(text), "[hitch] queue 0 submission %llu: %.1f ms of packets, %.1f ms waiting; slowest %s %.1f ms, longest wait %s %.1f ms\n", static_cast<unsigned long long>(work.serial), work.busyMs, work.waitMs, work.slowest.empty() ? "none" : work.slowest.c_str(), work.slowestMs, work.longestWait.empty() ? "none" : work.longestWait.c_str(), work.longestWaitMs);
    return std::string(text);
}

std::optional<std::string> FrameHitchTracker::IdleStoreFinished(double ms) {
    std::lock_guard lock(mutex);
    ++idleStores;
    idleStoreMs += ms;
    if (ms < thresholdMs) return std::nullopt;
    char text[160];
    std::snprintf(text, sizeof(text), "[hitch] queue 0 idle store of pending storage images: %.1f ms\n", ms);
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
    static auto* const tracker = new FrameHitchTracker(threshold);
    return tracker;
}

}
