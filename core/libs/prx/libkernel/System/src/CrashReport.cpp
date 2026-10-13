#include "prx/libkernel/System/include/CrashReport.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"

#if defined(__x86_64__) && (defined(__APPLE__) || defined(__linux__))

#include "prx/libc/include/specifics/linux/ElfTypes.hpp"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#endif

namespace CrashReport {

namespace {

constexpr std::size_t AlternateStackBytes = 128 * 1024;
constexpr int FrameLimit = 48;
constexpr int StackWords = 1024;

std::atomic<bool> installed{false};
std::atomic<int> reportingThread{0};

struct Registers {
    std::uint64_t rip, rsp, rbp, rax, rbx, rcx, rdx, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15, rflags, error, trap;
};

Registers Read(const void* context) {
    Registers r{};
#ifdef __APPLE__
    const auto* machine = static_cast<const ucontext_t*>(context)->uc_mcontext;
    const auto& s = machine->__ss;
    r = {s.__rip, s.__rsp, s.__rbp, s.__rax, s.__rbx, s.__rcx, s.__rdx, s.__rsi, s.__rdi, s.__r8, s.__r9, s.__r10, s.__r11, s.__r12, s.__r13, s.__r14, s.__r15, s.__rflags,
         machine->__es.__err, machine->__es.__trapno};
#else
    const auto* g = static_cast<const ucontext_t*>(context)->uc_mcontext.gregs;
    const auto u = [&](int index) { return static_cast<std::uint64_t>(g[index]); };
    r = {u(REG_RIP), u(REG_RSP), u(REG_RBP), u(REG_RAX), u(REG_RBX), u(REG_RCX), u(REG_RDX), u(REG_RSI), u(REG_RDI), u(REG_R8), u(REG_R9), u(REG_R10), u(REG_R11), u(REG_R12), u(REG_R13), u(REG_R14),
         u(REG_R15), u(REG_EFL), u(REG_ERR), u(REG_TRAPNO)};
#endif
    return r;
}

bool ReadMemory(std::uintptr_t address, void* out, std::size_t bytes) {
#ifdef __APPLE__
    mach_vm_size_t read = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, bytes, reinterpret_cast<mach_vm_address_t>(out), &read) == KERN_SUCCESS && read == bytes;
#else
    iovec local{out, bytes};
    iovec remote{reinterpret_cast<void*>(address), bytes};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(bytes);
#endif
}

class Writer {
public:
    void Line(const char* format, ...) {
        va_list arguments;
        va_start(arguments, format);
        const int length = std::vsnprintf(_line, sizeof(_line), format, arguments);
        va_end(arguments);
        if (length <= 0) return;
        Raw(_line, std::min<std::size_t>(static_cast<std::size_t>(length), sizeof(_line) - 1));
    }

    static void Raw(const char* text, std::size_t bytes) {
        while (bytes > 0) {
            const auto written = ::write(STDERR_FILENO, text, bytes);
            if (written <= 0) return;
            text += written;
            bytes -= static_cast<std::size_t>(written);
        }
    }

private:
    char _line[640];
};

unsigned long long U(std::uint64_t value) {
    return static_cast<unsigned long long>(value);
}

std::uint64_t HostThreadId() {
#ifdef __APPLE__
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
#else
    return static_cast<std::uint64_t>(syscall(SYS_gettid));
#endif
}

const char* SignalName(int signal) {
    switch (signal) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        default: return "signal";
    }
}

struct GuestLookup {
    std::uintptr_t address;
    const char* name;
    std::uintptr_t bias;
    bool found;
};

int FindGuestImage(dl_phdr_info* info, std::size_t, void* data) {
    auto* lookup = static_cast<GuestLookup*>(data);
    for (std::uint16_t i = 0; i < info->dlpi_phnum; ++i) {
        const auto& header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) == 0) continue;
        const auto begin = info->dlpi_addr + header.p_vaddr;
        if (lookup->address >= begin && lookup->address < begin + header.p_memsz) {
            lookup->name = info->dlpi_name != nullptr && info->dlpi_name[0] != '\0' ? info->dlpi_name : "guest executable";
            lookup->bias = info->dlpi_addr;
            lookup->found = true;
            return 1;
        }
    }
    return 0;
}

bool InSharedCache(const void* base) {
#ifdef __APPLE__
    const auto* header = static_cast<const mach_header_64*>(base);
    return header != nullptr && (header->flags & MH_DYLIB_IN_CACHE) != 0;
#else
    (void)base;
    return false;
#endif
}

bool Describe(Writer& out, const char* label, std::uintptr_t address, bool onlyCode) {
    GuestLookup guest{address, nullptr, 0, false};
    dl_iterate_phdr(FindGuestImage, &guest);
    if (guest.found) {
        out.Line("[crash]   %s 0x%llx  %s guest+0x%llx (load bias 0x%llx)\n", label, U(address), guest.name, U(address - guest.bias), U(guest.bias));
        return true;
    }
    Dl_info symbol{};
    if (dladdr(reinterpret_cast<const void*>(address), &symbol) == 0 || symbol.dli_fname == nullptr) {
        if (!onlyCode) out.Line("[crash]   %s 0x%llx  ?\n", label, U(address));
        return !onlyCode;
    }
    if (onlyCode && InSharedCache(symbol.dli_fbase)) return false;
    const char* file = std::strrchr(symbol.dli_fname, '/');
    out.Line("[crash]   %s 0x%llx  %s+0x%llx %s+0x%llx\n", label, U(address), file != nullptr ? file + 1 : symbol.dli_fname,
             U(address - reinterpret_cast<std::uintptr_t>(symbol.dli_fbase)), symbol.dli_sname != nullptr ? symbol.dli_sname : "?",
             U(symbol.dli_saddr != nullptr ? address - reinterpret_cast<std::uintptr_t>(symbol.dli_saddr) : 0));
    return true;
}

int ListGuestImage(dl_phdr_info* info, std::size_t, void* data) {
    auto& out = *static_cast<Writer*>(data);
    for (std::uint16_t i = 0; i < info->dlpi_phnum; ++i) {
        const auto& header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) == 0) continue;
        out.Line("[crash] guest code 0x%llx-0x%llx bias 0x%llx %s\n", U(info->dlpi_addr + header.p_vaddr), U(info->dlpi_addr + header.p_vaddr + header.p_memsz), U(info->dlpi_addr),
                 info->dlpi_name != nullptr && info->dlpi_name[0] != '\0' ? info->dlpi_name : "guest executable");
    }
    return 0;
}

void ListImages(Writer& out) {
#ifdef __APPLE__
    for (std::uint32_t index = 0; index < _dyld_image_count(); ++index) {
        const auto* header = _dyld_get_image_header(index);
        if (header == nullptr || InSharedCache(header)) continue;
        out.Line("[crash] image 0x%llx slide 0x%llx %s\n", U(reinterpret_cast<std::uintptr_t>(header)), U(static_cast<std::uint64_t>(_dyld_get_image_vmaddr_slide(index))),
                 _dyld_get_image_name(index));
    }
#endif
    dl_iterate_phdr(ListGuestImage, &out);
}

void Report(int signal, const siginfo_t* info, const void* context) {
    Writer out;
    const auto r = Read(context);
    char hostName[64] = "";
    pthread_getname_np(pthread_self(), hostName, sizeof(hostName));
    const PthreadPrivate* guest = CurrentGuestThread();
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    out.Line("\n[crash] %s (signal %d) code %d at address 0x%llx; process %d, host thread %llu '%s', wall clock %lld.%03ld\n", SignalName(signal), signal, info->si_code,
             U(reinterpret_cast<std::uintptr_t>(info->si_addr)), static_cast<int>(getpid()), U(HostThreadId()), hostName, static_cast<long long>(now.tv_sec), now.tv_nsec / 1000000);
    if (guest != nullptr)
        out.Line("[crash] guest thread %p tid %lld '%s' stack 0x%llx-0x%llx\n", static_cast<const void*>(guest), static_cast<long long>(guest->tid), guest->name.c_str(),
                 U(reinterpret_cast<std::uintptr_t>(guest->stackAddress)), U(reinterpret_cast<std::uintptr_t>(guest->stackAddress) + guest->stackSize));
    else
        out.Line("[crash] not a guest thread\n");
    out.Line("[crash] rip 0x%llx rsp 0x%llx rbp 0x%llx rflags 0x%llx trap %llu error 0x%llx\n", U(r.rip), U(r.rsp), U(r.rbp), U(r.rflags), U(r.trap), U(r.error));
    out.Line("[crash] rax 0x%llx rbx 0x%llx rcx 0x%llx rdx 0x%llx rsi 0x%llx rdi 0x%llx\n", U(r.rax), U(r.rbx), U(r.rcx), U(r.rdx), U(r.rsi), U(r.rdi));
    out.Line("[crash] r8 0x%llx r9 0x%llx r10 0x%llx r11 0x%llx r12 0x%llx r13 0x%llx r14 0x%llx r15 0x%llx\n", U(r.r8), U(r.r9), U(r.r10), U(r.r11), U(r.r12), U(r.r13), U(r.r14), U(r.r15));
    unsigned char code[16];
    if (ReadMemory(r.rip, code, sizeof(code)))
        out.Line("[crash] code at rip: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7],
                 code[8], code[9], code[10], code[11], code[12], code[13], code[14], code[15]);
    out.Line("[crash] frames by rbp:\n");
    Describe(out, "rip", r.rip, false);
    std::uintptr_t frame = r.rbp;
    std::uintptr_t lastReturn = 0;
    int repeats = 0;
    for (int depth = 0; depth < FrameLimit && frame != 0; ++depth) {
        std::uintptr_t link[2];
        if (!ReadMemory(frame, link, sizeof(link))) break;
        if (link[1] == lastReturn) ++repeats;
        else Describe(out, "ret", link[1], false);
        lastReturn = link[1];
        if (link[0] <= frame) break;
        frame = link[0];
    }
    if (repeats > 0) out.Line("[crash]   (%d repeated return addresses folded)\n", repeats);
    out.Line("[crash] code addresses on the stack from rsp:\n");
    std::uintptr_t lastValue = 0;
    for (int word = 0; word < StackWords; ++word) {
        std::uintptr_t value = 0;
        const std::uintptr_t slot = r.rsp + static_cast<std::uintptr_t>(word) * sizeof(value);
        if (!ReadMemory(slot, &value, sizeof(value))) break;
        if (value < 0x10000 || value == lastValue) continue;
        lastValue = value;
        char label[32];
        std::snprintf(label, sizeof(label), "[rsp+0x%x]", word * 8);
        Describe(out, label, value, true);
    }
    out.Line("[crash] host backtrace of the handler:\n");
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    ListImages(out);
    out.Line("[crash] end of report; %s takes its default action\n", SignalName(signal));
}

void Handle(int signal, siginfo_t* info, void* context) {
    const int self = static_cast<int>(HostThreadId() & 0x7fffffff) | 1;
    int expected = 0;
    if (!reportingThread.compare_exchange_strong(expected, self)) {
        if (expected != self)
            for (;;) pause();
    } else {
        Report(signal, info, context);
    }
    struct sigaction fallback{};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(signal, &fallback, nullptr);
    raise(signal);
}

struct AlternateStack {
    void* memory = nullptr;
    ~AlternateStack() {
        if (memory == nullptr) return;
        stack_t disable{};
        disable.ss_flags = SS_DISABLE;
        sigaltstack(&disable, nullptr);
        munmap(memory, AlternateStackBytes);
    }
};

thread_local AlternateStack alternateStack;

bool Handled(int signal) {
    struct sigaction current{};
    if (sigaction(signal, nullptr, &current) != 0) return false;
    if ((current.sa_flags & SA_SIGINFO) != 0) return current.sa_sigaction != nullptr;
    return current.sa_handler != SIG_DFL;
}

struct sigaction Action() {
    struct sigaction action{};
    action.sa_sigaction = Handle;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGSEGV);
    sigaddset(&action.sa_mask, SIGBUS);
    return action;
}

}

void Install() {
    static std::atomic<bool> attempted{false};
    if (attempted.exchange(true)) return;
    if (Handled(SIGSEGV) || Handled(SIGBUS)) {
        static constexpr char message[] = "[crash] a SIGSEGV or SIGBUS handler is already installed; the crash report is not installed\n";
        Writer::Raw(message, sizeof(message) - 1);
        return;
    }
    const auto action = Action();
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    installed.store(true);
    PrepareThread();
}

bool Restore(int hostSignal) {
    if (!Owns(hostSignal)) return false;
    const auto action = Action();
    return sigaction(hostSignal, &action, nullptr) == 0;
}

void PrepareThread() {
    if (!installed.load() || alternateStack.memory != nullptr) return;
    stack_t current{};
    if (sigaltstack(nullptr, &current) == 0 && (current.ss_flags & SS_DISABLE) == 0) return;
    void* memory = mmap(nullptr, AlternateStackBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) return;
    stack_t stack{};
    stack.ss_sp = memory;
    stack.ss_size = AlternateStackBytes;
    if (sigaltstack(&stack, nullptr) != 0) {
        munmap(memory, AlternateStackBytes);
        return;
    }
    alternateStack.memory = memory;
}

bool Owns(int hostSignal) {
    return installed.load() && (hostSignal == SIGSEGV || hostSignal == SIGBUS);
}

namespace {
const bool installedAtLoad = (Install(), true);
}

}

#else

namespace CrashReport {

void Install() {}
void PrepareThread() {}
bool Owns(int) {
    return false;
}
bool Restore(int) {
    return false;
}

}

#endif
