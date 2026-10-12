#include "prx/libc/include/GuestWriteWatch.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
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
#include <cstring>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <memory>
#include <new>
#include <unordered_map>
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

    void Register(std::uintptr_t begin, std::uintptr_t end) {
        begin = pageStart(begin);
        end = pageEnd(end);
        if (!_available || end <= begin) return;
        std::vector<std::pair<std::uintptr_t, Segment>> mapped;
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
            mapped.emplace_back(first, Segment{last, protection});
            cursor = last;
        }
        const std::lock_guard lock(_lock);
        std::vector<std::pair<std::uintptr_t, Segment>> segments;
        auto cursor = begin;
        const auto unqueried = [&](std::uintptr_t from, std::uintptr_t to) {
            auto it = _segments.upper_bound(from);
            if (it != _segments.begin()) --it;
            for (; it != _segments.end() && it->first < to && from < to; ++it) {
                if (it->second.end <= from) continue;
                if (it->first > from) segments.emplace_back(from, Segment{it->first, 0});
                const auto last = std::min(to, it->second.end);
                segments.emplace_back(std::max(from, it->first), Segment{last, it->second.protection});
                from = last;
            }
            if (from < to) segments.emplace_back(from, Segment{to, 0});
        };
        for (const auto& [first, segment] : mapped) {
            if (first > cursor) unqueried(cursor, first);
            segments.emplace_back(first, segment);
            cursor = segment.end;
        }
        if (cursor < end) unqueried(cursor, end);
        erase(begin, end);
        for (const auto& [first, segment] : segments) _segments.emplace(first, segment);
    }

    bool Unregister(std::uintptr_t begin, std::uintptr_t end) {
        begin = pageStart(begin);
        end = pageEnd(end);
        if (!_available || end <= begin) return false;
        const std::lock_guard lock(_lock);
        return erase(begin, end);
    }

    bool Covers(std::uintptr_t begin, std::uintptr_t end) {
        if (!_available) return false;
        const std::lock_guard lock(_lock);
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
            const std::lock_guard lock(_lock);
            if (take != Take::Fresh && !covers(begin, end)) return false;
            auto it = _segments.upper_bound(begin);
            if (it != _segments.begin()) --it;
            Leaf* leaf = nullptr;
            std::uintptr_t leafNumber = ~std::uintptr_t{0};
            for (; it != _segments.end() && it->first < end; ++it) {
                const auto first = std::max(begin, it->first);
                const auto last = std::min(end, it->second.end);
                const std::uint8_t initial = Written | Fresh | Materialized | it->second.protection;
                for (auto page = first; page < last; page += _page) {
                    const auto number = page / _page;
                    if ((number >> LeafShift) != leafNumber) {
                        leafNumber = number >> LeafShift;
                        const auto found = _leaves.find(leafNumber);
                        leaf = found != _leaves.end() ? found->second.get() : nullptr;
                    }
                    const auto index = number & (LeafPages - 1);
                    std::uint8_t state = leaf != nullptr && (leaf->states[index] & Materialized) != 0 ? leaf->states[index] : initial;
                    const bool fresh = (state & Fresh) != 0;
                    if ((take == Take::Armed && fresh) || (take == Take::Fresh && !fresh)) continue;
                    const bool compared = (state & Comparable) == Comparable;
                    const auto shadow = leaf != nullptr ? leaf->shadows[index] : NoShadow;
                    if ((state & Written) == 0 && (!compared || shadow == NoShadow || !differs(page, _shadows.At(shadow)))) continue;
                    if (leaf == nullptr) leaf = materializeLeaf(leafNumber);
                    if ((leaf->states[index] & Materialized) == 0) ++leaf->materialized;
                    state &= static_cast<std::uint8_t>(~Fresh);
                    if (!runs.empty() && runs.back().second == page) runs.back().second = page + _page;
                    else runs.emplace_back(page, page + _page);
                    const bool hostWrite = std::any_of(_hostWrites.begin(), _hostWrites.end(), [&](const auto& write) { return page >= write.first && page < write.second; });
                    if (!hostWrite) {
                        if (compared) {
                            if (leaf->shadows[index] == NoShadow) leaf->shadows[index] = _shadows.Allocate(_page);
                            std::memcpy(_shadows.At(leaf->shadows[index]), reinterpret_cast<const void*>(page), _page);
                        }
                        state &= static_cast<std::uint8_t>(~Written);
                    }
                    leaf->states[index] = state;
                }
            }
        }
        for (const auto& [first, last] : runs) written(context, first, last);
        return true;
    }

    void ProtectionChanged(std::uintptr_t begin, std::uintptr_t end, int protection) {
        begin = pageStart(begin);
        end = pageEnd(end);
        if (!_available || end <= begin) return;
        const std::lock_guard lock(_lock);
        split(begin);
        split(end);
        for (auto it = _segments.lower_bound(begin); it != _segments.end() && it->first < end; ++it) it->second.protection = static_cast<std::uint8_t>(protection & ProtectionMask);
        forEachMaterialized(begin, end, [&](Leaf& leaf, std::size_t index) {
            auto& state = leaf.states[index];
            state = static_cast<std::uint8_t>(Materialized | Written | (state & Fresh) | (protection & ProtectionMask));
        });
    }

    void BeginHostWrite(std::uintptr_t begin, std::uintptr_t end) {
        begin = pageStart(begin);
        end = pageEnd(end);
        if (!_available || end <= begin) return;
        const std::lock_guard lock(_lock);
        _hostWrites.emplace_back(begin, end);
        forEachMaterialized(begin, end, [&](Leaf& leaf, std::size_t index) {
            leaf.states[index] |= Written;
        });
    }

    void EndHostWrite(std::uintptr_t begin, std::uintptr_t end) {
        begin = pageStart(begin);
        end = pageEnd(end);
        if (!_available || end <= begin) return;
        const std::lock_guard lock(_lock);
        const auto found = std::find(_hostWrites.begin(), _hostWrites.end(), std::pair{begin, end});
        if (found != _hostWrites.end()) _hostWrites.erase(found);
    }

    int Protection(std::uintptr_t address, std::uintptr_t limit, std::uintptr_t* runEnd) {
        const auto page = pageStart(address);
        const std::lock_guard lock(_lock);
        auto it = _segments.upper_bound(page);
        if (_available && it != _segments.begin() && page < std::prev(it)->second.end) {
            --it;
            const auto protection = it->second.protection;
            auto end = it->second.end;
            for (auto next = std::next(it); next != _segments.end() && next->first == end && end < limit && next->second.protection == protection; ++next) end = next->second.end;
            *runEnd = std::min(end, limit);
            return protection;
        }
        *runEnd = it != _segments.end() ? std::min(it->first, limit) : limit;
        return -1;
    }

private:
    static constexpr std::uint8_t ProtectionMask = PROT_READ | PROT_WRITE | PROT_EXEC;
    static constexpr std::uint8_t Comparable = PROT_READ | PROT_WRITE;
    static constexpr std::uint8_t Written = 0x10;
    static constexpr std::uint8_t Fresh = 0x40;
    static constexpr std::uint8_t Materialized = 0x80;
    static constexpr std::uint32_t NoShadow = ~std::uint32_t{0};
    static constexpr unsigned LeafShift = 16;
    static constexpr std::uintptr_t LeafPages = std::uintptr_t{1} << LeafShift;

    struct Segment {
        std::uintptr_t end;
        std::uint8_t protection;
    };

    struct Leaf {
        Leaf() {
            states.fill(0);
            shadows.fill(NoShadow);
        }
        std::array<std::uint8_t, LeafPages> states;
        std::array<std::uint32_t, LeafPages> shadows;
        std::size_t materialized = 0;
    };

    class Shadows {
    public:
        std::uint32_t Allocate(std::uintptr_t page) {
            if (!_free.empty()) {
                const auto slot = _free.back();
                _free.pop_back();
                return slot;
            }
            if (_used == _chunks.size() * ChunkPages) {
                void* chunk = ::mmap(nullptr, ChunkPages * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
                if (chunk == MAP_FAILED) throw std::bad_alloc();
                _chunks.push_back(static_cast<std::uint8_t*>(chunk));
                _page = page;
            }
            return _used++;
        }

        void Release(std::uint32_t slot) {
            _free.push_back(slot);
        }

        std::uint8_t* At(std::uint32_t slot) const {
            return _chunks[slot / ChunkPages] + static_cast<std::size_t>(slot % ChunkPages) * _page;
        }

    private:
        static constexpr std::uint32_t ChunkPages = 1024;
        std::vector<std::uint8_t*> _chunks;
        std::vector<std::uint32_t> _free;
        std::uint32_t _used = 0;
        std::uintptr_t _page = 0;
    };

    Watch() {
        _available = std::getenv("APS5_NO_WRITE_WATCH") == nullptr;
    }

    bool differs(std::uintptr_t page, const std::uint8_t* shadow) const {
        const auto* current = reinterpret_cast<const std::uint64_t*>(page);
        const auto* previous = reinterpret_cast<const std::uint64_t*>(shadow);
        const auto words = _page / sizeof(std::uint64_t);
        for (std::size_t block = 0; block < words; block += 64) {
            std::uint64_t difference = 0;
            for (std::size_t word = block; word < block + 64; ++word) difference |= current[word] ^ previous[word];
            if (difference != 0) return true;
        }
        return false;
    }

    std::uintptr_t pageStart(std::uintptr_t address) const {
        return address & ~(_page - 1);
    }

    std::uintptr_t pageEnd(std::uintptr_t address) const {
        return (address + _page - 1) & ~(_page - 1);
    }

    Leaf* materializeLeaf(std::uintptr_t number) {
        auto& leaf = _leaves[number];
        if (!leaf) leaf = std::make_unique<Leaf>();
        return leaf.get();
    }

    template <typename TFunction>
    void forEachMaterialized(std::uintptr_t begin, std::uintptr_t end, TFunction function) {
        const auto firstPage = begin / _page;
        const auto lastPage = end / _page;
        const auto firstLeaf = firstPage >> LeafShift;
        const auto lastLeaf = (lastPage - 1) >> LeafShift;
        std::vector<std::uintptr_t> numbers;
        if (lastLeaf - firstLeaf >= _leaves.size()) {
            for (const auto& [number, leaf] : _leaves)
                if (number >= firstLeaf && number <= lastLeaf) numbers.push_back(number);
        } else {
            for (auto number = firstLeaf; number <= lastLeaf; ++number)
                if (_leaves.contains(number)) numbers.push_back(number);
        }
        for (const auto number : numbers) {
            auto& leaf = *_leaves.at(number);
            const auto from = std::max(firstPage, number << LeafShift) - (number << LeafShift);
            const auto to = std::min(lastPage, (number + 1) << LeafShift) - (number << LeafShift);
            for (auto index = from; index < to && leaf.materialized != 0; ++index)
                if ((leaf.states[index] & Materialized) != 0) function(leaf, index);
            if (leaf.materialized == 0) _leaves.erase(number);
        }
    }

    void split(std::uintptr_t at) {
        auto it = _segments.upper_bound(at);
        if (it == _segments.begin()) return;
        --it;
        if (it->first >= at || it->second.end <= at) return;
        const auto tail = it->second;
        it->second.end = at;
        _segments.emplace(at, tail);
    }

    bool covers(std::uintptr_t begin, std::uintptr_t end) const {
        auto it = _segments.upper_bound(begin);
        if (it == _segments.begin()) return false;
        --it;
        auto cursor = begin;
        for (; it != _segments.end() && it->first <= cursor; ++it) {
            if (it->second.end > cursor) cursor = it->second.end;
            if (cursor >= end) return true;
        }
        return false;
    }

    bool erase(std::uintptr_t begin, std::uintptr_t end) {
        split(begin);
        split(end);
        bool removed = false;
        for (auto it = _segments.lower_bound(begin); it != _segments.end() && it->first < end;) {
            it = _segments.erase(it);
            removed = true;
        }
        forEachMaterialized(begin, end, [&](Leaf& leaf, std::size_t index) {
            if (leaf.shadows[index] != NoShadow) _shadows.Release(leaf.shadows[index]);
            leaf.shadows[index] = NoShadow;
            leaf.states[index] = 0;
            --leaf.materialized;
        });
        return removed;
    }

    const std::uintptr_t _page = static_cast<std::uintptr_t>(::getpagesize());
    bool _available = false;
    std::mutex _lock;
    std::map<std::uintptr_t, Segment> _segments;
    std::unordered_map<std::uintptr_t, std::unique_ptr<Leaf>> _leaves;
    Shadows _shadows;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> _hostWrites;
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
