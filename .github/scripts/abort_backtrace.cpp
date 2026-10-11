#include <execinfo.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <cstdio>
#include <unistd.h>

namespace {

void Report(int signal) {
    void* frames[128];
    const int count = backtrace(frames, 128);
    dprintf(STDERR_FILENO, "abort_backtrace: signal %d, load address %p\n", signal, static_cast<const void*>(_dyld_get_image_header(0)));
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    ::signal(signal, SIG_DFL);
    raise(signal);
}

const bool installed = [] {
    struct sigaction action{};
    action.sa_handler = Report;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGABRT, &action, nullptr) == 0;
}();

}
