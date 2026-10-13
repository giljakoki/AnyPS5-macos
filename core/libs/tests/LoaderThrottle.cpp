#include "prx/libkernel/Aio/include/ReadThrottle.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
}

}

void readDuringExit() {
    try {
        ReadThrottle::After(1024u * 1024u);
        std::printf("PASS a read during process exit is throttled without touching destroyed state\n");
    } catch (const std::exception& error) {
        std::printf("FAIL a read during process exit threw: %s\n", error.what());
        std::fflush(stdout);
        _exit(1);
    }
    std::fflush(stdout);
}

int main() {
    std::atexit(readDuringExit);
    try {
        const ReadThrottle::Settings settings{1000, 5.0};
        double carry = 0.0;
        check(ReadThrottle::DelayMicros(settings, 1024 * 1024, 0.0, carry) == 1000 && carry == 0.0, "one MiB costs the configured microseconds");
        carry = 0.0;
        check(ReadThrottle::DelayMicros(settings, 512 * 1024, 1.0, carry) == 500, "half a MiB costs half");
        carry = 0.0;
        std::uint64_t total = 0;
        for (int i = 0; i < 1024; ++i) total += ReadThrottle::DelayMicros(settings, 1024, 1.0, carry);
        check(total == 1000, "small reads accumulate their fractions");
        carry = 0.0;
        check(ReadThrottle::DelayMicros(settings, 1024 * 1024, 5.0, carry) == 0, "reads after the window are not delayed");
        check(ReadThrottle::DelayMicros(ReadThrottle::Settings{0, 5.0}, 1024 * 1024, 0.0, carry) == 0, "a zero rate is off");

        check(ReadThrottle::FlipAllows(0, 1u << 30, 0), "a zero flip budget is off");
        check(ReadThrottle::FlipAllows(1024, 2047, 0) && !ReadThrottle::FlipAllows(1024, 2048, 0), "two flips of headroom before the first counted flip");
        check(ReadThrottle::FlipAllows(1024, 2048, 1) && !ReadThrottle::FlipAllows(1024, 3072, 1), "each flip adds one budget");

        setenv("APS5_LOADER_THROTTLE_US", "20000", 1);
        setenv("APS5_LOADER_THROTTLE_SECONDS", "1", 1);
        setenv("APS5_LOADER_MIB_PER_FLIP", "1", 1);
        const auto current = ReadThrottle::Current();
        check(current.microsPerMiB == 20000 && current.windowSeconds == 1.0, "the environment sets the rate and the window");
        check(current.bytesPerFlip == 1024u * 1024u, "the environment sets the flip budget");
        const auto start = std::chrono::steady_clock::now();
        ReadThrottle::After(4u * 1024u * 1024u);
        const auto waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("4 MiB at 20000 us/MiB waited %.1f ms\n", waited);
        check(waited >= 79.0 && waited < 10000.0, "a read waits for its share of the rate");

        std::this_thread::sleep_until(start + std::chrono::milliseconds(1100));
        ReadThrottle::SetFlipStallMillis(60000);
        LoaderFlipCompleted_nid_no_patch();
        check(ReadThrottle::Flips() == 1, "a completed flip is counted");
        ReadThrottle::After(2u * 1024u * 1024u);
        const auto beforeFlip = ReadThrottle::Totals();
        check(beforeFlip.heldReads == 0, "a read inside the flip allowance is not held");
        std::atomic<bool> released{false};
        std::thread reader([&] {
            ReadThrottle::After(2u * 1024u * 1024u);
            released.store(true);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (ReadThrottle::Totals().waiting == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(ReadThrottle::Totals().waiting == 1, "a read over the flip allowance waits");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(!released.load(), "a waiting read stays held without a flip");
        LoaderFlipCompleted_nid_no_patch();
        LoaderFlipCompleted_nid_no_patch();
        reader.join();
        const auto afterFlip = ReadThrottle::Totals();
        check(afterFlip.heldReads == beforeFlip.heldReads + 1 && afterFlip.releasedByFlip == beforeFlip.releasedByFlip + 1 && afterFlip.stalls == beforeFlip.stalls, "the flips release the held read");

        ReadThrottle::SetFlipStallMillis(100);
        const auto stallStart = std::chrono::steady_clock::now();
        ReadThrottle::After(8u * 1024u * 1024u);
        const auto stalled = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stallStart).count();
        const auto afterStall = ReadThrottle::Totals();
        std::printf("a read without flips waited %.1f ms\n", stalled);
        check(afterStall.stalls == afterFlip.stalls + 1 && afterStall.releasedByFlip == afterFlip.releasedByFlip, "a read held without any flip is released by the stall limit");
        check(stalled >= 90.0 && stalled < 30000.0, "the stall limit holds the read at least its length");
        ReadThrottle::After(1024u * 1024u);
        check(ReadThrottle::Totals().heldReads == afterStall.heldReads, "after a stall the allowance starts again");
    } catch (const std::exception& error) {
        std::printf("FAIL %s\n", error.what());
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
