#include "prx/libkernel/System/include/RaiseTrace.hpp"
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
}

constexpr std::uint64_t Now = 50000000000ULL;
constexpr std::uint64_t Grace = RaiseTrace::UnhandledGraceNanos;

}

int main() {
    check(RaiseTrace::Unhandled(9, 8, 8, Now - Grace, Now), "a raise sent a grace period ago without a handler is unhandled");
    check(!RaiseTrace::Unhandled(9, 8, 8, Now - Grace + 1, Now), "a raise inside the grace period is not reported");
    check(!RaiseTrace::Unhandled(9, 8, 8, Now + 1000, Now), "a raise sent after the watch read the clock is not reported");
    check(!RaiseTrace::Unhandled(9, 9, 8, Now - 2 * Grace, Now), "a handled raise is not reported");
    check(!RaiseTrace::Unhandled(8, 9, 8, Now - 2 * Grace, Now), "a handler that entered before its raise was recorded is not reported");
    check(!RaiseTrace::Unhandled(9, 8, 9, Now - 2 * Grace, Now), "a raise is reported once");
    check(!RaiseTrace::Unhandled(0, 0, 0, 0, Now), "a thread that was never sent a raise is not reported");
    return failures == 0 ? 0 : 1;
}
