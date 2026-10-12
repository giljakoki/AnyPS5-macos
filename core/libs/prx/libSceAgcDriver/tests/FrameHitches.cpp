#include "prx/libSceAgcDriver/Execution/include/Driver/Packets/FrameHitches.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace AgcDriver::DriverDetail;
using Clock = std::chrono::steady_clock;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

HitchSubmission At(Clock::time_point base, double enteredMs, double leftMs, double roomMs = 0, double holdMs = 0) {
    const auto offset = [&](double ms) { return base + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms)); };
    return {offset(enteredMs), offset(leftMs), roomMs, holdMs};
}

void PeriodTests() {
    FrameHitchTracker tracker(25);
    const auto base = Clock::now();
    Require(!tracker.FlipSubmitted(At(base, 0, 14, 13, 0)).has_value(), "the first flip has no period to report");
    Require(!tracker.FlipSubmitted(At(base, 16.7, 30, 12, 0)).has_value(), "a 16.7 ms frame is below a 25 ms threshold");
    tracker.OtherSubmitted();
    tracker.WorkFinished({7, 4, 12, "DRAW_INDEX_2", 0.5, "WAIT_FLIP_DONE", 11});
    const auto report = tracker.FlipSubmitted(At(base, 76.7, 80, 3, 0));
    Require(report.has_value(), "a 60 ms frame must be reported");
    Require(report->find("60.0 ms between flip submissions") != std::string::npos, "the report must give the period: " + *report);
    Require(report->find("46.7 ms in the title") != std::string::npos, "the report must give the time outside the driver: " + *report);
    Require(report->find("blocked 13.3 ms (flip room 12.0 ms") != std::string::npos, "the report must give the previous submission's block: " + *report);
    Require(report->find("1 other submissions") != std::string::npos, "the report must count other submissions: " + *report);
    Require(report->find("longest submission 7 with 4.0 ms of packets (slowest DRAW_INDEX_2 0.5 ms)") != std::string::npos, "the report must name the worker's longest submission: " + *report);
    Require(!tracker.FlipSubmitted(At(base, 93.4, 95)).has_value(), "counters reset after a report");
}

void WorkTests() {
    FrameHitchTracker tracker(25);
    Require(!tracker.WorkFinished({1, 24.9, 20, "flip", 1, "WAIT_FLIP_DONE", 20}).has_value(), "work and waits below the threshold are quiet");
    const auto report = tracker.WorkFinished({2, 30, 5, "DRAW_INDEX_AUTO", 28, "WAIT_REG_MEM", 5});
    Require(report.has_value() && report->find("submission 2: 30.0 ms of packets, 5.0 ms waiting; slowest DRAW_INDEX_AUTO 28.0 ms, longest wait WAIT_REG_MEM 5.0 ms") != std::string::npos, "long work must be reported");
    const auto waited = tracker.WorkFinished({3, 1, 40, "flip", 0.4, "WAIT_FLIP_DONE", 38});
    Require(waited.has_value() && waited->find("longest wait WAIT_FLIP_DONE 38.0 ms") != std::string::npos, "a long wait must be reported");
}

void ParseTests() {
    Require(ParseHitchThresholdMs(nullptr) == 0, "an unset variable disables the tracer");
    Require(ParseHitchThresholdMs("25") == 25 && ParseHitchThresholdMs("20.5") == 20.5, "thresholds are milliseconds");
    for (const char* text : {"", "0", "-3", "abc", "25ms"}) {
        bool threw = false;
        try {
            static_cast<void>(ParseHitchThresholdMs(text));
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        Require(threw, std::string("APS5_TRACE_HITCHES=") + text + " must be rejected");
    }
}

}

int main() {
    try {
        PeriodTests();
        WorkTests();
        ParseTests();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "frame hitch tests passed\n";
    return 0;
}
