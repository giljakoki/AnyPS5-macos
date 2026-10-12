#include "prx/libSceAgcDriver/Execution/include/Driver/Packets/FrameHitches.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Packets/HitchSampler.hpp"
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
    Require(!tracker.IdleStoreFinished(3).has_value(), "a short idle store is quiet");
    const auto store = tracker.IdleStoreFinished(30);
    Require(store.has_value() && store->find("idle store of pending storage images: 30.0 ms") != std::string::npos, "a long idle store is reported");
    const auto report = tracker.FlipSubmitted(At(base, 76.7, 80, 3, 0));
    Require(report.has_value(), "a 60 ms frame must be reported");
    Require(report->find("60.0 ms between flip submissions") != std::string::npos, "the report must give the period: " + *report);
    Require(report->find("46.7 ms in the title") != std::string::npos, "the report must give the time outside the driver: " + *report);
    Require(report->find("blocked 13.3 ms (flip room 12.0 ms") != std::string::npos, "the report must give the previous submission's block: " + *report);
    Require(report->find("1 other submissions") != std::string::npos, "the report must count other submissions: " + *report);
    Require(report->find("longest submission 7 with 4.0 ms of packets (slowest DRAW_INDEX_2 0.5 ms), 2 idle stores in 33.0 ms") != std::string::npos, "the report must name the worker's longest submission and idle stores: " + *report);
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

void DescriberTests() {
    FrameHitchTracker tracker(25);
    const auto base = Clock::now();
    int calls = 0;
    const auto describe = [&](Clock::time_point from, Clock::time_point to) {
        ++calls;
        Require(to - from > std::chrono::milliseconds(39), "the describer receives the title interval");
        return std::string("seen");
    };
    tracker.FlipSubmitted(At(base, 0, 35), describe);
    const auto blocked = tracker.FlipSubmitted(At(base, 40, 42), describe);
    Require(blocked.has_value() && calls == 0, "a frame that blocked in the driver is not described");
    const auto slow = tracker.FlipSubmitted(At(base, 90, 92), describe);
    Require(slow.has_value() && calls == 1 && slow->find("; title thread: seen\n") != std::string::npos, "a frame slow in the title is described: " + slow.value_or(""));
}

void SummaryTests() {
    const auto names = [](std::uint64_t address) -> std::string {
        if (address == 1) return "libc.prx!sceLibcMspaceMalloc";
        if (address == 2) return "libc.prx!helper";
        if (address == 3) return "eboot!+0x100";
        return "eboot!+0x200";
    };
    std::vector<ThreadSample> samples(4);
    samples[0].frames = {1, 2, 3, 0, 0, 0};
    samples[1].frames = {1, 3, 0, 0, 0, 0};
    samples[2].frames = {4, 3, 0, 0, 0, 0};
    samples[3].frames = {2, 0, 0, 0, 0, 0};
    const auto text = SummarizeSamples(samples, names, 6);
    Require(text.find("4 samples; leaves: 50% libc.prx!sceLibcMspaceMalloc, 25% eboot!+0x200, 25% libc.prx!helper") == 0, "leaves are ranked by share: " + text);
    Require(text.find("entered from: 50% libc.prx <- eboot!+0x100, 25% eboot <- none, 25% libc.prx <- none") != std::string::npos, "callers are the first frame outside the leaf's module: " + text);
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
        DescriberTests();
        SummaryTests();
        ParseTests();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "frame hitch tests passed\n";
    return 0;
}
