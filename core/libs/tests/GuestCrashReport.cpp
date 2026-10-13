#include "SceTypes.hpp"
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char** environ;

extern "C" {
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
int APS5_VABI sigaction_nid_postfix(int guest, const void* action, void* previous);
}

namespace {

void Require(bool condition, const std::string& message) {
    if (condition) return;
    std::fprintf(stderr, "guest crash report: %s\n", message.c_str());
    std::exit(1);
}

volatile std::uintptr_t invalidAddress = 0x18;

void* APS5_VABI Fault(void*) {
    *reinterpret_cast<volatile int*>(invalidAddress) = 1;
    return nullptr;
}

volatile int depthSink = 0;

int Recurse(int depth) {
    volatile char frame[1024];
    frame[0] = static_cast<char>(depth);
    depthSink = frame[0];
    return Recurse(depth + 1) + frame[0];
}

void* APS5_VABI Overflow(void*) {
    depthSink = Recurse(0);
    return nullptr;
}

void* APS5_VABI Raise(void*) {
    std::raise(SIGSEGV);
    return nullptr;
}

void ThrowThrough() noexcept {
    throw std::runtime_error("crash-test-terminate");
}

void* APS5_VABI Terminate(void*) {
    ThrowThrough();
    return nullptr;
}

int Child(const char* mode) {
    struct GuestSigaction {
        std::uintptr_t handler;
        int flags;
        std::uint32_t mask[4];
    } reset{};
    if (std::strcmp(mode, "guest-reset") == 0) Require(sigaction_nid_postfix(11, &reset, nullptr) == 0, "the guest could not reset SIGSEGV");
    PthreadEntry entry = std::strcmp(mode, "overflow") == 0 ? Overflow : std::strcmp(mode, "raise") == 0 ? Raise : std::strcmp(mode, "terminate") == 0 ? Terminate : Fault;
    Pthread thread = nullptr;
    Require(scePthreadCreate(&thread, nullptr, entry, nullptr, "crash-test-thread") == 0, "creating the thread failed");
    scePthreadJoin(thread, nullptr);
    return 0;
}

std::string SelfPath() {
#ifdef __APPLE__
    char path[4096];
    std::uint32_t size = sizeof(path);
    Require(_NSGetExecutablePath(path, &size) == 0, "cannot find the test executable");
    return path;
#else
    char path[4096];
    const auto length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    Require(length > 0, "cannot find the test executable");
    path[length] = '\0';
    return path;
#endif
}

void Expect(const char* mode, const char* const* needles, int expectedSignal = 0) {
    int pipeEnds[2];
    Require(pipe(pipeEnds) == 0, "pipe failed");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipeEnds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipeEnds[0]);
    const std::string self = SelfPath();
    char* const arguments[] = {const_cast<char*>(self.c_str()), const_cast<char*>(mode), nullptr};
    pid_t child = 0;
    Require(posix_spawn(&child, self.c_str(), &actions, nullptr, arguments, environ) == 0, "spawning the child failed");
    posix_spawn_file_actions_destroy(&actions);
    close(pipeEnds[1]);
    std::string output;
    char buffer[4096];
    for (;;) {
        const auto bytes = read(pipeEnds[0], buffer, sizeof(buffer));
        if (bytes <= 0) break;
        output.append(buffer, static_cast<std::size_t>(bytes));
    }
    close(pipeEnds[0]);
    int status = 0;
    Require(waitpid(child, &status, 0) == child, "waiting for the child failed");
    const std::string label = std::string(mode) + ": ";
    const bool signaled = WIFSIGNALED(status) && (expectedSignal != 0 ? WTERMSIG(status) == expectedSignal : WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS);
    Require(signaled, label + "the child did not die of the expected signal, output:\n" + output);
    for (const char* const* needle = needles; *needle != nullptr; ++needle)
        Require(output.find(*needle) != std::string::npos, label + "the report lacks '" + *needle + "', output:\n" + output);
}

}

int main(int argc, char** argv) {
    if (argc > 1) return Child(argv[1]);
    const char* const fault[] = {"[crash] SIGSEGV (signal 11)", "at address 0x18", "guest thread", "'crash-test-thread'", "[crash] rip 0x", "[crash] frames by rbp:", "Fault", "end of report", nullptr};
    Expect("fault", fault);
    Expect("guest-reset", fault);
    const char* const overflow[] = {"[crash] SIG", "'crash-test-thread'", "Recurse", "end of report", nullptr};
    Expect("overflow", overflow);
    const char* const raised[] = {"[crash] SIGSEGV (signal 11)", "'crash-test-thread'", "end of report", nullptr};
    Expect("raise", raised);
    const char* const terminated[] = {"[crash] std::terminate", "'crash-test-thread'", "crash-test-terminate", "[crash] frames:", "TerminateEPv", "end of report", nullptr};
    Expect("terminate", terminated, SIGABRT);
    std::puts("guest crash report: ok");
    return 0;
}
