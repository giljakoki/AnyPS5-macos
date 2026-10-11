#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <signal.h>
#include <cstdint>
#include <cstdio>
#include <unistd.h>

namespace {

const void* Program() {
    for (std::uint32_t index = 0; index < _dyld_image_count(); ++index) {
        const mach_header* header = _dyld_get_image_header(index);
        if (header != nullptr && header->filetype == MH_EXECUTE) return header;
    }
    return nullptr;
}

void Report(int signal) {
    void* frames[128];
    const int count = backtrace(frames, 128);
    dprintf(STDERR_FILENO, "abort_backtrace: signal %d, load address %p\n", signal, Program());
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
