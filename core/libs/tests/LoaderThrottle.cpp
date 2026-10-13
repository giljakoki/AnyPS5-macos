#include "prx/libkernel/Aio/include/ReadThrottle.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
}

}

int main() {
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

        setenv("APS5_LOADER_THROTTLE_US", "20000", 1);
        setenv("APS5_LOADER_THROTTLE_SECONDS", "30", 1);
        const auto current = ReadThrottle::Current();
        check(current.microsPerMiB == 20000 && current.windowSeconds == 30.0, "the environment sets the rate and the window");
        const auto start = std::chrono::steady_clock::now();
        ReadThrottle::After(4u * 1024u * 1024u);
        const auto waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("4 MiB at 20000 us/MiB waited %.1f ms\n", waited);
        check(waited >= 79.0 && waited < 1000.0, "a read waits for its share of the rate");
    } catch (const std::exception& error) {
        std::printf("FAIL %s\n", error.what());
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
