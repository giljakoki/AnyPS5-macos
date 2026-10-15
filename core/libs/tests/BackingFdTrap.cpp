// APS5_TRAP_BACKING_FD=1: a guarded direct memory backing descriptor stays mappable, closes through
// CloseBackingFd, and any other close() of it kills the process (EXC_GUARD) instead of succeeding.
#include "prx/libkernel/DirectMemory/BackingFdTrap.hpp"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static void Check(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "Backing fd trap check failed at line %d\n", line);
        std::abort();
    }
}
#define Require(value) Check((value), __LINE__)

int main(int argc, char** argv) {
    ::setenv("APS5_TRAP_BACKING_FD", "1", 1);
    Require(DirectMemoryTrap::Enabled());
    if (argc > 1 && std::string(argv[1]) == "self-close") {
        // Manual check of the in-process report: prints the [fdtrap] backtrace naming main, then aborts.
        const int own = DirectMemoryTrap::CreateBackingFile(0x4000);
        Require(own >= 0);
        ::close(own);
        ::sleep(5);
        return 1;
    }
    const int fd = DirectMemoryTrap::CreateBackingFile(0x4000);
    Require(fd >= 0);
    void* view = ::mmap(nullptr, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    Require(view != MAP_FAILED);
    static_cast<volatile char*>(view)[0] = 1;
    ::munmap(view, 0x4000);
    DirectMemoryTrap::CheckBackingFd(fd, nullptr, 0x4000);

    const pid_t child = ::fork();
    Require(child >= 0);
    if (child == 0) {
        const int own = DirectMemoryTrap::CreateBackingFile(0x4000);
        if (own < 0) ::_exit(2);
        ::close(own);  // the stray close the trap exists to catch
        ::_exit(0);
    }
    int status = 0;
    Require(::waitpid(child, &status, 0) == child);
    Require(WIFSIGNALED(status));
    Require(::fcntl(fd, F_GETFD) >= 0);

    DirectMemoryTrap::CloseBackingFd(fd);
    Require(::fcntl(fd, F_GETFD) < 0);
    return 0;
}
