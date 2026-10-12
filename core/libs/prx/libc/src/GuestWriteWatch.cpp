#include <sys/ucontext.h>
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <pthread.h>
#include <ctime>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#if (defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)) || defined(__APPLE__)
#define APS5_GUEST_WRITE_WATCH 1
#endif

namespace GuestWriteWatch {
namespace {

#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
constexpr std::uintptr_t PageBytes = 4096;

class Watch {
public:
    static Watch& Get() {
        static Watch watch;
        return watch;
    }

    bool Available() const {
        return _pagemap >= 0;
    }

    void Register(std::uintptr_t begin, std::uintptr_t end) {
        if (!Available() || end <= begin) return;
        std::unique_lock lock(_lock);
        remove(_ranges, begin, end);
        remove(_fresh, begin, end);
        uffdio_register registration{};
        registration.range.start = begin;
        registration.range.len = end - begin;
        registration.mode = UFFDIO_REGISTER_MODE_WP;
        if (ioctl(_uffd, UFFDIO_REGISTER, &registration) != 0) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                std::fprintf(stderr, "[memory] write watch: cannot register 0x%llx+0x%llx (%s); the range stays unwatched\n", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin), std::strerror(errno));
            }
            return;
        }
        insert(_ranges, begin, end);
        insert(_fresh, begin, end);
    }

    bool Unregister(std::uintptr_t begin, std::uintptr_t end) {
        if (!Available() || end <= begin) return false;
        std::unique_lock lock(_lock);
        const bool watched = remove(_ranges, begin, end);
        remove(_fresh, begin, end);
        return watched;
    }

    bool Covers(std::uintptr_t begin, std::uintptr_t end) {
        if (!Available()) return false;
        std::shared_lock lock(_lock);
        return covers(begin, end);
    }

    bool Collect(std::uintptr_t begin, std::uintptr_t end, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context) {
        if (!Available()) return false;
        begin &= ~(PageBytes - 1);
        end = (end + PageBytes - 1) & ~(PageBytes - 1);
        if (end <= begin) return true;
        if (!takeFresh(begin, end, true, written, context)) return false;
        return scanWritten(begin, end, written, context);
    }

    bool CollectFresh(std::uintptr_t begin, std::uintptr_t end, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context) {
        if (!Available()) return false;
        begin &= ~(PageBytes - 1);
        end = (end + PageBytes - 1) & ~(PageBytes - 1);
        if (end <= begin) return true;
        {
            std::shared_lock lock(_lock);
            auto it = _fresh.upper_bound(begin);
            if (it != _fresh.begin()) --it;
            while (it != _fresh.end() && it->first < end && it->second <= begin) ++it;
            if (it == _fresh.end() || it->first >= end) return true;
        }
        return takeFresh(begin, end, false, written, context);
    }

    bool CollectArmed(std::uintptr_t begin, std::uintptr_t end, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context) {
        if (!Available()) return false;
        begin &= ~(PageBytes - 1);
        end = (end + PageBytes - 1) & ~(PageBytes - 1);
        if (end <= begin) return true;
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> armed;
        {
            std::shared_lock lock(_lock);
            if (!covers(begin, end)) return false;
            auto cursor = begin;
            auto it = _fresh.upper_bound(begin);
            if (it != _fresh.begin()) --it;
            for (; it != _fresh.end() && it->first < end; ++it) {
                if (it->second <= cursor) continue;
                if (it->first > cursor) armed.emplace_back(cursor, it->first);
                cursor = std::max(cursor, it->second);
            }
            if (cursor < end) armed.emplace_back(cursor, end);
        }
        for (const auto& [from, to] : armed) {
            if (!scanWritten(from, to, written, context)) return false;
        }
        return true;
    }

private:
    bool takeFresh(std::uintptr_t begin, std::uintptr_t end, bool whole, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context) {
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> fresh;
        {
            std::unique_lock lock(_lock);
            if (whole && !covers(begin, end)) return false;
            auto it = _fresh.upper_bound(begin);
            if (it != _fresh.begin()) --it;
            for (; it != _fresh.end() && it->first < end; ++it) {
                const auto from = std::max(it->first, begin);
                const auto to = std::min(it->second, end);
                if (from < to) fresh.emplace_back(from, to);
            }
            for (const auto& [from, to] : fresh) remove(_fresh, from, to);
        }
        for (const auto& [from, to] : fresh) {
            written(context, from, to);
            if (protect(from, to)) continue;
            std::unique_lock lock(_lock);
            for (const auto& [left, right] : fresh) {
                if (!covers(left, right)) continue;
                remove(_fresh, left, right);
                insert(_fresh, left, right);
            }
            if (whole) written(context, begin, end);
            else for (const auto& [left, right] : fresh) written(context, left, right);
            return false;
        }
        return true;
    }

    bool scanWritten(std::uintptr_t begin, std::uintptr_t end, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context) const {
        std::array<page_region, 256> regions;
        auto cursor = begin;
        while (cursor < end) {
            pm_scan_arg scan{};
            scan.size = sizeof(scan);
            scan.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
            scan.start = cursor;
            scan.end = end;
            scan.vec = reinterpret_cast<std::uintptr_t>(regions.data());
            scan.vec_len = regions.size();
            scan.category_mask = PAGE_IS_WRITTEN;
            scan.return_mask = PAGE_IS_WRITTEN;
            const auto count = ioctl(_pagemap, PAGEMAP_SCAN, &scan);
            if (count < 0) {
                written(context, cursor, end);
                return false;
            }
            for (long i = 0; i < count; ++i) written(context, regions[i].start, regions[i].end);
            if (scan.walk_end <= cursor || scan.walk_end >= end) break;
            cursor = scan.walk_end;
        }
        return true;
    }

private:
    Watch() {
        if (std::getenv("APS5_NO_WRITE_WATCH") == nullptr) open();
    }

    void open() {
        _uffd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK));
        if (_uffd < 0 && errno == EPERM) _uffd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
        if (_uffd < 0) return unavailable("userfaultfd");
        uffdio_api api{};
        api.api = UFFD_API;
        api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED;
        if (ioctl(_uffd, UFFDIO_API, &api) != 0 || (api.features & UFFD_FEATURE_WP_ASYNC) == 0 || (api.features & UFFD_FEATURE_WP_UNPOPULATED) == 0) return unavailable("asynchronous userfaultfd write protection");
        const int pagemap = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        if (pagemap < 0) return unavailable("/proc/self/pagemap");
        if (!probe(pagemap)) {
            close(pagemap);
            return unavailable("PAGEMAP_SCAN");
        }
        _pagemap = pagemap;
    }

    void unavailable(const char* what) {
        const int error = errno;
        if (_uffd >= 0) close(_uffd);
        _uffd = -1;
        std::fprintf(stderr, "[memory] write watch unavailable: %s failed (%s); guest memory is compared instead\n", what, std::strerror(error));
    }

    bool protect(std::uintptr_t begin, std::uintptr_t end) const {
        uffdio_writeprotect protection{};
        protection.range.start = begin;
        protection.range.len = end - begin;
        protection.mode = UFFDIO_WRITEPROTECT_MODE_WP;
        return ioctl(_uffd, UFFDIO_WRITEPROTECT, &protection) == 0;
    }

    bool probe(int pagemap) {
        constexpr std::uintptr_t probeBytes = 4 * PageBytes;
        void* pages = mmap(nullptr, probeBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (pages == MAP_FAILED) return false;
        const auto address = reinterpret_cast<std::uintptr_t>(pages);
        auto* bytes = static_cast<volatile char*>(pages);
        bytes[0] = 1;
        uffdio_register registration{};
        registration.range.start = address;
        registration.range.len = probeBytes;
        registration.mode = UFFDIO_REGISTER_MODE_WP;
        bool working = ioctl(_uffd, UFFDIO_REGISTER, &registration) == 0 && protect(address, address + probeBytes);
        std::array<page_region, 4> regions{};
        const auto scan = [&]() -> long {
            pm_scan_arg arguments{};
            arguments.size = sizeof(arguments);
            arguments.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
            arguments.start = address;
            arguments.end = address + probeBytes;
            arguments.vec = reinterpret_cast<std::uintptr_t>(regions.data());
            arguments.vec_len = regions.size();
            arguments.category_mask = PAGE_IS_WRITTEN;
            arguments.return_mask = PAGE_IS_WRITTEN;
            const auto count = ioctl(pagemap, PAGEMAP_SCAN, &arguments);
            if (count < 0) return -1;
            long pagesWritten = 0;
            for (long i = 0; i < count; ++i) pagesWritten += static_cast<long>((regions[i].end - regions[i].start) / PageBytes);
            return pagesWritten;
        };
        working = working && scan() == 0;
        if (working) {
            bytes[0] = 2;
            bytes[2 * PageBytes] = 1;
            working = scan() == 2 && scan() == 0;
        }
        munmap(pages, probeBytes);
        return working;
    }

    bool covers(std::uintptr_t begin, std::uintptr_t end) const {
        auto next = _ranges.upper_bound(begin);
        if (next == _ranges.begin()) return false;
        return std::prev(next)->second >= end;
    }

    static void insert(std::map<std::uintptr_t, std::uintptr_t>& ranges, std::uintptr_t begin, std::uintptr_t end) {
        auto next = ranges.upper_bound(begin);
        if (next != ranges.begin() && std::prev(next)->second == begin) {
            begin = std::prev(next)->first;
            ranges.erase(std::prev(next));
        }
        if (next != ranges.end() && next->first == end) {
            end = next->second;
            ranges.erase(next);
        }
        ranges.emplace(begin, end);
    }

    static bool remove(std::map<std::uintptr_t, std::uintptr_t>& ranges, std::uintptr_t begin, std::uintptr_t end) {
        bool removed = false;
        auto it = ranges.upper_bound(begin);
        if (it != ranges.begin()) --it;
        while (it != ranges.end() && it->first < end) {
            const auto rangeBegin = it->first;
            const auto rangeEnd = it->second;
            if (rangeEnd <= begin) {
                ++it;
                continue;
            }
            removed = true;
            it = ranges.erase(it);
            if (rangeBegin < begin) ranges.emplace(rangeBegin, begin);
            if (rangeEnd > end) ranges.emplace(end, rangeEnd);
        }
        return removed;
    }

    int _uffd = -1;
    int _pagemap = -1;
    std::shared_mutex _lock;
    std::map<std::uintptr_t, std::uintptr_t> _ranges;
    std::map<std::uintptr_t, std::uintptr_t> _fresh;
};

const bool g_opened = (Watch::Get(), true);
#elif defined(__APPLE__)
class Watch {
public:
    static Watch& Get() {
        static Watch watch;
        return watch;
    }

    bool Available() const {
        return _available;
    }

    std::size_t PageSize() const {
        return static_cast<std::size_t>(_page);
    }

    void Register(std::uintptr_t begin, std::uintptr_t end) {
        begin = pageStart(begin);
        end = pageEnd(end);
        if (!_available || end <= begin) return;
        std::vector<std::uint8_t> pages((end - begin) / _page, Written | Fresh);
        std::vector<bool> queried(pages.size(), false);
        const Lock lock(_lock);
        for (auto cursor = begin; cursor < end;) {
            mach_vm_address_t address = cursor;
            mach_vm_size_t size = 0;
            vm_region_basic_info_data_64_t info{};
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object = MACH_PORT_NULL;
            if (mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS || address >= end) break;
            const auto first = std::max<std::uintptr_t>(cursor, address);
            const auto last = std::min<std::uintptr_t>(end, address + size);
            const std::uint8_t protection = ((info.protection & VM_PROT_READ) ? PROT_READ : 0) | ((info.protection & VM_PROT_WRITE) ? PROT_WRITE : 0) | ((info.protection & VM_PROT_EXECUTE) ? PROT_EXEC : 0);
            for (auto page = first; page < last; page += _page) {
                pages[(page - begin) / _page] = Written | Fresh | protection;
                queried[(page - begin) / _page] = true;
            }
            cursor = last;
        }
        forEachPage(begin, end, [&](std::uintptr_t page, std::uint8_t& state) {
            auto& entry = pages[(page - begin) / _page];
            const std::uint8_t recorded = state & ProtectionMask;
            const bool armed = (state & Protected) != 0 && (entry & ProtectionMask) == (recorded & ~PROT_WRITE);
            if (armed || !queried[(page - begin) / _page]) entry = static_cast<std::uint8_t>(Written | Fresh | recorded);
            else state = static_cast<std::uint8_t>(state & ~Protected);
        });
        erase(begin, end);
        _ranges.emplace(begin, Range{end, std::move(pages)});
    }

    bool Unregister(std::uintptr_t begin, std::uintptr_t end) {
        begin = pageStart(begin);
        end = pageEnd(end);
        if (!_available || end <= begin) return false;
        const Lock lock(_lock);
        return erase(begin, end);
    }

    bool Covers(std::uintptr_t begin, std::uintptr_t end) {
        if (!_available) return false;
        const Lock lock(_lock);
        return covers(begin, end);
    }

    enum class Take { All, Armed, Fresh };

    bool Collect(std::uintptr_t begin, std::uintptr_t end, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context, Take take = Take::All) {
        if (!_available) return false;
        begin = pageStart(begin);
        end = pageEnd(end);
        if (end <= begin) return true;
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> runs;
        {
            const Lock lock(_lock);
            if (take != Take::Fresh && !covers(begin, end)) return false;
            forEachPage(begin, end, [&](std::uintptr_t page, std::uint8_t& state) {
                if ((state & Written) == 0) return;
                const bool fresh = (state & Fresh) != 0;
                if ((take == Take::Armed && fresh) || (take == Take::Fresh && !fresh)) return;
                state &= static_cast<std::uint8_t>(~Fresh);
                if (!runs.empty() && runs.back().second == page) runs.back().second = page + _page;
                else runs.emplace_back(page, page + _page);
                for (const auto& [first, last] : _hostWrites)
                    if (page >= first && page < last) return;
                state &= static_cast<std::uint8_t>(~Written);
                if ((state & PROT_WRITE) != 0) state |= Protected;
            });
            for (const auto& [first, last] : runs) protect(first, last);
        }
        for (const auto& [first, last] : runs) written(context, first, last);
        return true;
    }

    void ProtectionChanged(std::uintptr_t begin, std::uintptr_t end, int protection) {
        begin = pageStart(begin);
        end = pageEnd(end);
        if (!_available || end <= begin) return;
        const Lock lock(_lock);
        forEachPage(begin, end, [&](std::uintptr_t, std::uint8_t& state) {
            state = static_cast<std::uint8_t>(Written | (state & Fresh) | (protection & ProtectionMask));
        });
    }

    void BeginHostWrite(std::uintptr_t begin, std::uintptr_t end) {
        begin = pageStart(begin);
        end = pageEnd(end);
        if (!_available || end <= begin) return;
        const Lock lock(_lock);
        _hostWrites.emplace_back(begin, end);
        forEachPage(begin, end, [&](std::uintptr_t page, std::uint8_t& state) {
            if ((state & Protected) != 0) ::mprotect(reinterpret_cast<void*>(page), _page, state & ProtectionMask);
            state = static_cast<std::uint8_t>((state & (ProtectionMask | Fresh)) | Written);
        });
    }

    void EndHostWrite(std::uintptr_t begin, std::uintptr_t end) {
        begin = pageStart(begin);
        end = pageEnd(end);
        if (!_available || end <= begin) return;
        const Lock lock(_lock);
        const auto found = std::find(_hostWrites.begin(), _hostWrites.end(), std::pair{begin, end});
        if (found != _hostWrites.end()) _hostWrites.erase(found);
    }

    int Protection(std::uintptr_t address, std::uintptr_t limit, std::uintptr_t* runEnd) {
        const auto page = pageStart(address);
        const Lock lock(_lock);
        auto it = _ranges.upper_bound(page);
        if (_available && it != _ranges.begin() && page < std::prev(it)->second.end) {
            --it;
            const auto& pages = it->second.pages;
            const std::uint8_t protection = pages[(page - it->first) / _page] & ProtectionMask;
            auto end = page + _page;
            while (end < it->second.end && end < limit && (pages[(end - it->first) / _page] & ProtectionMask) == protection) end += _page;
            *runEnd = std::min(end, limit);
            return protection;
        }
        *runEnd = it != _ranges.end() ? std::min(it->first, limit) : limit;
        return -1;
    }

private:
    static constexpr std::uint8_t ProtectionMask = PROT_READ | PROT_WRITE | PROT_EXEC;
    static constexpr std::uint8_t Written = 0x10;
    static constexpr std::uint8_t Protected = 0x20;
    static constexpr std::uint8_t Fresh = 0x40;

    struct Range {
        std::uintptr_t end;
        std::vector<std::uint8_t> pages;
    };

    struct Turns {
        std::atomic<std::uint64_t> next{0};
        std::atomic<std::uint64_t> serving{0};
    };

    struct Lock {
        explicit Lock(Turns& turns) : _turns(turns), _turn(turns.next.fetch_add(1, std::memory_order_relaxed)) {
            while (_turns.serving.load(std::memory_order_acquire) != _turn) ::sched_yield();
        }
        ~Lock() {
            _turns.serving.store(_turn + 1, std::memory_order_release);
        }
        Turns& _turns;
        std::uint64_t _turn;
    };

    Watch() {
        if (std::getenv("APS5_NO_WRITE_WATCH") != nullptr) return;
        struct sigaction action {};
        action.sa_sigaction = &Watch::fault;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGBUS, &action, &_previousBus) != 0 || sigaction(SIGSEGV, &action, &_previousSegv) != 0) {
            std::fprintf(stderr, "[memory] write watch: cannot install the fault handler (%s); guest buffers are fully re-read every use\n", std::strerror(errno));
            return;
        }
        _available = true;
    }

public:
    static inline std::atomic<std::uint64_t> faultCount{0};
    static inline std::atomic<std::uintptr_t> lastFaultAddress{0};
    static inline std::atomic<std::uint64_t> lastFaultThread{0};

private:
    static void fault(int signal, siginfo_t* info, void* context) {
        auto& watch = Get();
        {
            static std::atomic<std::uint64_t> lastReportNanos{0};
            const auto count = faultCount.fetch_add(1, std::memory_order_relaxed) + 1;
            lastFaultAddress.store(reinterpret_cast<std::uintptr_t>(info->si_addr), std::memory_order_relaxed);
            {
                std::uint64_t tid = 0;
                pthread_threadid_np(nullptr, &tid);
                lastFaultThread.store(tid, std::memory_order_relaxed);
            }
            if (count % 20000 == 0) {
                timespec now{};
                clock_gettime(CLOCK_MONOTONIC, &now);
                const std::uint64_t nanos = static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(now.tv_nsec);
                const auto previous = lastReportNanos.exchange(nanos, std::memory_order_relaxed);
                char text[200];
                const int length = std::snprintf(text, sizeof(text), "[write-watch] %llu faults total, last 20000 in %.1f ms, at 0x%llx\n", static_cast<unsigned long long>(count), previous == 0 ? 0.0 : (nanos - previous) / 1e6, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info->si_addr)));
                const auto written = ::write(STDERR_FILENO, text, static_cast<std::size_t>(length));
                static_cast<void>(written);
            }
        }
        lastFaultContext = context;
        lastFaultCode = info->si_code;
        lastFaultSignal = signal;
        if (watch.record(reinterpret_cast<std::uintptr_t>(info->si_addr))) return;
        const auto& previous = signal == SIGBUS ? watch._previousBus : watch._previousSegv;
        if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
            previous.sa_sigaction(signal, info, context);
            return;
        }
        if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN && previous.sa_handler != nullptr) {
            previous.sa_handler(signal);
            return;
        }
        ::signal(signal, SIG_DFL);
    }

    bool record(std::uintptr_t address) {
        const auto page = pageStart(address);
        static thread_local std::uintptr_t repeatedAddress = 0;
        static thread_local unsigned repeatedCount = 0;
        if (address == repeatedAddress) ++repeatedCount; else { repeatedAddress = address; repeatedCount = 0; }
        const Lock lock(_lock);
        auto it = _ranges.upper_bound(page);
        if (it == _ranges.begin()) return false;
        --it;
        if (page >= it->second.end) return false;
        auto& state = it->second.pages[(page - it->first) / _page];
        if (repeatedCount == 50 || (repeatedCount > 0 && repeatedCount % 200000 == 0)) {
            reportUnwritable("repeated fault at one address", address, state, it->first, it->second.end);
            reportResume(address, state, it->first, it->second.end);
            reportRegisters(true, repeatedCount);
        }
        if ((state & Protected) != 0) {
            state = static_cast<std::uint8_t>((state & (ProtectionMask | Fresh)) | Written);
            if (::mprotect(reinterpret_cast<void*>(page), _page, state & ProtectionMask) != 0) reportFault("[write-watch] unprotecting a watched page in the fault handler failed\n");
            if ((state & PROT_WRITE) == 0) reportUnwritable("unprotected to a read-only recording", address, state, it->first, it->second.end);
            return true;
        }
        if ((state & PROT_WRITE) == 0) {
            reportUnwritable("fault on a page recorded without write access", address, state, it->first, it->second.end);
        }
        if ((state & PROT_WRITE) != 0) {
            static std::atomic<unsigned> resumed{0};
            static std::atomic<std::uintptr_t> previousAddress{0};
            const auto count = resumed.fetch_add(1, std::memory_order_relaxed);
            const bool same = previousAddress.exchange(address, std::memory_order_relaxed) == address;
            if (count < 4 || count % 20000 == 0) {
                reportResume(address, state, it->first, it->second.end);
                reportInstruction();
                reportRegisters(same, count);
                if (count < 4) probeWrite(address);
            }
            if (::mprotect(reinterpret_cast<void*>(page), _page, state & ProtectionMask & ~PROT_WRITE) != 0 || ::mprotect(reinterpret_cast<void*>(page), _page, state & ProtectionMask) != 0) reportFault("[write-watch] refreshing a resumed page's protection failed\n");
            return true;
        }
        return false;
    }

    static inline thread_local void* lastFaultContext = nullptr;
    static inline thread_local int lastFaultCode = 0;
    static inline thread_local int lastFaultSignal = 0;

    static void reportInstruction() {
        const auto* context = static_cast<const ucontext_t*>(lastFaultContext);
        if (context == nullptr) return;
        const auto rip = static_cast<std::uintptr_t>(context->uc_mcontext->__ss.__rip);
        const auto* bytes = reinterpret_cast<const unsigned char*>(rip);
        char text[200];
        int length = std::snprintf(text, sizeof(text), "[write-watch]   at rip 0x%llx (signal %d si_code %d) bytes", static_cast<unsigned long long>(rip), lastFaultSignal, lastFaultCode);
        for (int i = 0; i < 12 && length < 180; ++i) length += std::snprintf(text + length, sizeof(text) - static_cast<std::size_t>(length), " %02x", bytes[i]);
        text[length++] = '\n';
        const auto written = ::write(STDERR_FILENO, text, static_cast<std::size_t>(length));
        static_cast<void>(written);
    }

    static void reportRegisters(bool sameAsPrevious, unsigned count) {
        const auto* context = static_cast<const ucontext_t*>(lastFaultContext);
        if (context == nullptr) return;
        const auto& ss = context->uc_mcontext->__ss;
        char text[240];
        const int length = std::snprintf(text, sizeof(text), "[write-watch]   resume #%u same-address-as-previous %d rax 0x%llx rbx 0x%llx rcx 0x%llx rdx 0x%llx rsi 0x%llx rdi 0x%llx r14 0x%llx\n", count, sameAsPrevious ? 1 : 0, static_cast<unsigned long long>(ss.__rax), static_cast<unsigned long long>(ss.__rbx), static_cast<unsigned long long>(ss.__rcx), static_cast<unsigned long long>(ss.__rdx), static_cast<unsigned long long>(ss.__rsi), static_cast<unsigned long long>(ss.__rdi), static_cast<unsigned long long>(ss.__r14));
        const auto written = ::write(STDERR_FILENO, text, static_cast<std::size_t>(length));
        static_cast<void>(written);
    }

    static void probeWrite(std::uintptr_t address) {
        unsigned char byte = 0;
        mach_vm_size_t outsize = 0;
        vm_offset_t data = 0;
        mach_msg_type_number_t count = 0;
        const auto readStatus = mach_vm_read(mach_task_self(), address & ~std::uintptr_t{15}, 16, &data, &count);
        if (readStatus == KERN_SUCCESS) {
            byte = *reinterpret_cast<const unsigned char*>(data);
            const auto writeStatus = mach_vm_write(mach_task_self(), address & ~std::uintptr_t{15}, data, count);
            char text[160];
            const int length = std::snprintf(text, sizeof(text), "[write-watch]   kernel read ok (first byte %02x), kernel write status %d\n", byte, writeStatus);
            const auto written = ::write(STDERR_FILENO, text, static_cast<std::size_t>(length));
            static_cast<void>(written);
            mach_vm_deallocate(mach_task_self(), data, count);
            static_cast<void>(outsize);
        } else {
            char text[120];
            const int length = std::snprintf(text, sizeof(text), "[write-watch]   kernel read status %d\n", readStatus);
            const auto written = ::write(STDERR_FILENO, text, static_cast<std::size_t>(length));
            static_cast<void>(written);
        }
    }

    static void reportUnwritable(const char* what, std::uintptr_t address, std::uint8_t state, std::uintptr_t rangeBegin, std::uintptr_t rangeEnd) {
        static std::atomic<unsigned> reported{0};
        const auto count = reported.fetch_add(1, std::memory_order_relaxed);
        if (count >= 8 && count % 100000 != 0) return;
        char text[256];
        const int length = std::snprintf(text, sizeof(text), "[write-watch] %s #%u: 0x%llx state 0x%02x range 0x%llx..0x%llx\n", what, count, static_cast<unsigned long long>(address), state, static_cast<unsigned long long>(rangeBegin), static_cast<unsigned long long>(rangeEnd));
        const auto written = ::write(STDERR_FILENO, text, static_cast<std::size_t>(length));
        static_cast<void>(written);
        reportInstruction();
    }

    static void reportResume(std::uintptr_t address, std::uint8_t state, std::uintptr_t rangeBegin, std::uintptr_t rangeEnd) {
        mach_vm_address_t region = address;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        const auto status = mach_vm_region(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        char text[256];
        const int length = std::snprintf(text, sizeof(text), "[write-watch] resuming a write to 0x%llx: state 0x%02x, range 0x%llx..0x%llx, kernel region 0x%llx+0x%llx prot 0x%x max 0x%x status %d\n", static_cast<unsigned long long>(address), state, static_cast<unsigned long long>(rangeBegin), static_cast<unsigned long long>(rangeEnd), static_cast<unsigned long long>(region), static_cast<unsigned long long>(size), info.protection, info.max_protection, status);
        const auto written = ::write(STDERR_FILENO, text, static_cast<std::size_t>(length));
        static_cast<void>(written);
    }

    static void reportFault(const char* message) {
        const auto written = ::write(STDERR_FILENO, message, std::strlen(message));
        static_cast<void>(written);
    }

    std::uintptr_t pageStart(std::uintptr_t address) const {
        return address & ~(_page - 1);
    }

    std::uintptr_t pageEnd(std::uintptr_t address) const {
        return (address + _page - 1) & ~(_page - 1);
    }

    template <typename TFunction>
    void forEachPage(std::uintptr_t begin, std::uintptr_t end, TFunction function) {
        auto it = _ranges.upper_bound(begin);
        if (it != _ranges.begin()) --it;
        for (; it != _ranges.end() && it->first < end; ++it) {
            const auto first = std::max(begin, it->first);
            const auto last = std::min(end, it->second.end);
            for (auto page = first; page < last; page += _page) function(page, it->second.pages[(page - it->first) / _page]);
        }
    }

    void protect(std::uintptr_t begin, std::uintptr_t end) {
        std::uintptr_t runStart = 0;
        std::uintptr_t previous = begin;
        int runProtection = -1;
        const auto flush = [&](std::uintptr_t runEnd) {
            if (runProtection >= 0) ::mprotect(reinterpret_cast<void*>(runStart), runEnd - runStart, runProtection);
            runProtection = -1;
        };
        forEachPage(begin, end, [&](std::uintptr_t page, std::uint8_t& state) {
            const int wanted = (state & Protected) != 0 ? (state & ProtectionMask & ~PROT_WRITE) : -1;
            if (wanted != runProtection || page != previous) {
                flush(previous);
                runStart = page;
                runProtection = wanted;
            }
            previous = page + _page;
        });
        flush(previous);
    }

    bool covers(std::uintptr_t begin, std::uintptr_t end) const {
        auto it = _ranges.upper_bound(begin);
        if (it == _ranges.begin()) return false;
        --it;
        auto cursor = begin;
        for (; it != _ranges.end() && it->first <= cursor; ++it) {
            if (it->second.end > cursor) cursor = it->second.end;
            if (cursor >= end) return true;
        }
        return false;
    }

    bool erase(std::uintptr_t begin, std::uintptr_t end) {
        bool removed = false;
        auto it = _ranges.upper_bound(begin);
        if (it != _ranges.begin()) --it;
        while (it != _ranges.end() && it->first < end) {
            const auto rangeBegin = it->first;
            const auto rangeEnd = it->second.end;
            if (rangeEnd <= begin) {
                ++it;
                continue;
            }
            removed = true;
            auto pages = std::move(it->second.pages);
            it = _ranges.erase(it);
            const auto cutBegin = std::max(begin, rangeBegin);
            const auto cutEnd = std::min(end, rangeEnd);
            for (auto page = cutBegin; page < cutEnd; page += _page) {
                const auto state = pages[(page - rangeBegin) / _page];
                if ((state & Protected) != 0) ::mprotect(reinterpret_cast<void*>(page), _page, state & ProtectionMask);
            }
            if (rangeBegin < cutBegin) _ranges.emplace(rangeBegin, Range{cutBegin, std::vector<std::uint8_t>(pages.begin(), pages.begin() + (cutBegin - rangeBegin) / _page)});
            if (cutEnd < rangeEnd) it = _ranges.emplace(cutEnd, Range{rangeEnd, std::vector<std::uint8_t>(pages.begin() + (cutEnd - rangeBegin) / _page, pages.end())}).first;
        }
        return removed;
    }

    static std::uintptr_t watchPageSize() {
        const auto page = static_cast<std::uintptr_t>(::getpagesize());
        return std::getenv("APS5_WRITE_WATCH_KERNEL_PAGES") != nullptr ? std::max(page, static_cast<std::uintptr_t>(vm_kernel_page_size)) : page;
    }

    const std::uintptr_t _page = watchPageSize();
    bool _available = false;
    Turns _lock;
    std::map<std::uintptr_t, Range> _ranges;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> _hostWrites;
    struct sigaction _previousBus {};
    struct sigaction _previousSegv {};
};

const bool g_opened = (Watch::Get(), true);
#endif

}

bool GuestWriteWatchAvailable_nid_postfix() {
#ifdef APS5_GUEST_WRITE_WATCH
    return Watch::Get().Available();
#else
    return false;
#endif
}

std::uint64_t GuestWriteWatchFaultStatistics_nid_postfix(std::uintptr_t* lastAddress, std::uint64_t* lastThread) {
    if (lastAddress != nullptr) *lastAddress = Watch::lastFaultAddress.load(std::memory_order_relaxed);
    if (lastThread != nullptr) *lastThread = Watch::lastFaultThread.load(std::memory_order_relaxed);
    return Watch::faultCount.load(std::memory_order_relaxed);
}

std::size_t GuestWriteWatchPageSize_nid_postfix() {
    return Watch::Get().PageSize();
}

void GuestWriteWatchRegister_nid_postfix(const void* pointer, std::size_t bytes) {
#ifdef APS5_GUEST_WRITE_WATCH
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    Watch::Get().Register(begin, begin + bytes);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
#endif
}

bool GuestWriteWatchUnregister_nid_postfix(const void* pointer, std::size_t bytes) {
#ifdef APS5_GUEST_WRITE_WATCH
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    return Watch::Get().Unregister(begin, begin + bytes);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
    return false;
#endif
}

bool GuestWriteWatchCovers_nid_postfix(std::uintptr_t address, std::size_t bytes) {
#ifdef APS5_GUEST_WRITE_WATCH
    return bytes != 0 && address + bytes > address && Watch::Get().Covers(address, address + bytes);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    return false;
#endif
}

bool GuestWriteWatchCollect_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context) {
#ifdef APS5_GUEST_WRITE_WATCH
    if (bytes == 0 || address + bytes < address) return false;
    return Watch::Get().Collect(address, address + bytes, written, context);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(written);
    static_cast<void>(context);
    return false;
#endif
}

void GuestWriteWatchBeginHostWrite_nid_postfix(const void* pointer, std::size_t bytes) {
#if defined(__APPLE__)
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    Watch::Get().BeginHostWrite(begin, begin + bytes);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
#endif
}

void GuestWriteWatchEndHostWrite_nid_postfix(const void* pointer, std::size_t bytes) {
#if defined(__APPLE__)
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    Watch::Get().EndHostWrite(begin, begin + bytes);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
#endif
}

void GuestWriteWatchProtectionChanged_nid_postfix(const void* pointer, std::size_t bytes, int protection) {
#if defined(__APPLE__)
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    Watch::Get().ProtectionChanged(begin, begin + bytes, protection);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
    static_cast<void>(protection);
#endif
}

int GuestWriteWatchProtection_nid_postfix(std::uintptr_t address, std::uintptr_t limit, std::uintptr_t* runEnd) {
#if defined(__APPLE__)
    return Watch::Get().Protection(address, limit, runEnd);
#else
    static_cast<void>(address);
    *runEnd = limit;
    return -1;
#endif
}

bool GuestWriteWatchCollectFresh_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context) {
#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
    if (bytes == 0 || address + bytes < address) return false;
    return Watch::Get().CollectFresh(address, address + bytes, written, context);
#elif defined(__APPLE__)
    if (bytes == 0 || address + bytes < address) return false;
    return Watch::Get().Collect(address, address + bytes, written, context, Watch::Take::Fresh);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(written);
    static_cast<void>(context);
    return false;
#endif
}

bool GuestWriteWatchCollectArmed_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context) {
#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
    if (bytes == 0 || address + bytes < address) return false;
    return Watch::Get().CollectArmed(address, address + bytes, written, context);
#elif defined(__APPLE__)
    if (bytes == 0 || address + bytes < address) return false;
    return Watch::Get().Collect(address, address + bytes, written, context, Watch::Take::Armed);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(written);
    static_cast<void>(context);
    return false;
#endif
}

}
