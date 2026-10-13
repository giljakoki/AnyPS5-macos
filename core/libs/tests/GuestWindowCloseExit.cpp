#include "prx/libc/include/Shutdown.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern "C" int APS5_VABI __cxa_atexit_nid_postfix(void (APS5_VABI *func)(void*), void* arg, void* dsoHandle);
extern "C" int APS5_VABI atexit_nid_postfix(void (APS5_VABI *func)());

// A window close (LibcRequestExit) runs the emulator's shutdown callbacks, flushes stdio and ends
// the process with the requested code without the title's exit handlers or static destructors
// (here both spin forever, like a sound library's destructor at Hades II's exit), and a shutdown
// callback that never returns does not keep the process alive past the shutdown deadline.

namespace {

int events[2];

void Record(char value) {
    static_cast<void>(!write(events[1], &value, 1));
}

void APS5_VABI SpinningDestructor(void*) {
    Record('D');
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

void APS5_VABI SpinningAtexit() {
    Record('A');
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

void HostHandler() { Record('H'); }

void Shutdown() { Record('S'); }

void HangingShutdown() {
    Record('S');
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

struct Outcome {
    std::string events;
    std::string output;
    int status = -1;
    double seconds = 0.0;
};

Outcome CloseWindow(void (*shutdown)()) {
    Outcome outcome;
    int output[2];
    if (pipe(events) != 0 || pipe(output) != 0) return outcome;
    const auto start = std::chrono::steady_clock::now();
    const pid_t child = fork();
    if (child < 0) return outcome;
    if (child == 0) {
        close(events[0]);
        close(output[0]);
        dup2(output[1], STDOUT_FILENO);
        __cxa_atexit_nid_postfix(SpinningDestructor, nullptr, nullptr);
        atexit_nid_postfix(SpinningAtexit);
        std::atexit(HostHandler);
        LibcRegisterShutdown_nid_postfix(shutdown);
        std::fputs("buffered before close", stdout);
        LibcRequestExit_nid_postfix(7);
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    close(events[1]);
    close(output[1]);
    char buffer[256];
    for (ssize_t count; (count = read(events[0], buffer, sizeof(buffer))) > 0;) outcome.events.append(buffer, static_cast<std::size_t>(count));
    for (ssize_t count; (count = read(output[0], buffer, sizeof(buffer))) > 0;) outcome.output.append(buffer, static_cast<std::size_t>(count));
    close(events[0]);
    close(output[0]);
    int status = 0;
    if (waitpid(child, &status, 0) == child && WIFEXITED(status)) outcome.status = WEXITSTATUS(status);
    outcome.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return outcome;
}

}

int main() {
    const auto prompt = CloseWindow(Shutdown);
    if (prompt.events != "S" || prompt.status != 7 || prompt.output != "buffered before close" || prompt.seconds > 3.0) {
        std::fprintf(stderr, "window close: events '%s', status %d, output '%s', %.1f s\n", prompt.events.c_str(), prompt.status, prompt.output.c_str(), prompt.seconds);
        return 1;
    }
    const auto hung = CloseWindow(HangingShutdown);
    if (hung.events != "S" || hung.status != 7 || hung.output != "buffered before close" || hung.seconds < 4.0 || hung.seconds > 9.0) {
        std::fprintf(stderr, "hung shutdown: events '%s', status %d, output '%s', %.1f s\n", hung.events.c_str(), hung.status, hung.output.c_str(), hung.seconds);
        return 1;
    }
    return 0;
}
