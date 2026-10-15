#include "prx/libc/include/GuestWriteWatch.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__AVX__)
#include <immintrin.h>
#endif
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(__APPLE__)
#error The shadow comparison write watch is the macOS backend
#endif

using namespace GuestWriteWatch;

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Region {
    explicit Region(std::size_t pageCount) : page(static_cast<std::size_t>(::getpagesize())), bytes(pageCount * page) {
        base = static_cast<std::uint8_t*>(::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        Require(base != MAP_FAILED, "cannot map the test region");
    }
    ~Region() {
        GuestWriteWatchUnregister_nid_postfix(base, bytes);
        ::munmap(base, bytes);
    }
    std::uintptr_t Address(std::size_t index) const {
        return reinterpret_cast<std::uintptr_t>(base) + index * page;
    }
    using Collector = bool (*)(std::uintptr_t, std::size_t, void (*)(void*, std::uintptr_t, std::uintptr_t), void*);
    std::set<std::size_t> Collect(Collector collector = &GuestWriteWatchCollect_nid_postfix, std::size_t first = 0, std::size_t count = 0) {
        std::set<std::size_t> pages;
        struct Sink {
            Region* region;
            std::set<std::size_t>* pages;
        } sink{this, &pages};
        const bool complete = collector(Address(first), count == 0 ? bytes - first * page : count * page, [](void* context, std::uintptr_t begin, std::uintptr_t end) {
            auto& target = *static_cast<Sink*>(context);
            for (auto at = begin; at < end; at += target.region->page) target.pages->insert((at - target.region->Address(0)) / target.region->page);
        }, &sink);
        Require(complete, "collection of a watched range is incomplete");
        return pages;
    }
    std::size_t page;
    std::size_t bytes;
    std::uint8_t* base = nullptr;
};

std::set<std::size_t> All(std::size_t count) {
    std::set<std::size_t> pages;
    for (std::size_t index = 0; index < count; ++index) pages.insert(index);
    return pages;
}

void CheckCollect() {
    Region region(8);
    Require(GuestWriteWatchAvailable_nid_postfix(), "write watch is unavailable");
    Require(!GuestWriteWatchCovers_nid_postfix(region.Address(0), region.bytes), "unregistered range is covered");
    GuestWriteWatchRegister_nid_postfix(region.base, region.bytes);
    Require(GuestWriteWatchCovers_nid_postfix(region.Address(0), region.bytes), "registered range is not covered");
    Require(region.Collect() == All(8), "a new range is not reported in full");
    Require(region.Collect().empty(), "an untouched range reports writes");
    region.base[3 * region.page + 17] = 1;
    region.base[5 * region.page] = 2;
    region.base[5 * region.page + 9] = 3;
    Require(region.Collect() == std::set<std::size_t>({3, 5}), "written pages are not reported exactly");
    Require(region.Collect().empty(), "collected writes are reported again");
    Require(region.base[3 * region.page + 17] == 1 && region.base[5 * region.page + 9] == 3, "watched writes were lost");
    std::memset(region.base + region.page - 4, 7, 8);
    Require(region.Collect() == std::set<std::size_t>({0, 1}), "a write across a page boundary is not reported on both pages");
}

void CheckFresh() {
    Region region(8);
    GuestWriteWatchRegister_nid_postfix(region.base, region.bytes);
    Require(region.Collect(&GuestWriteWatchCollect_nid_postfix, 0, 4) == All(4), "the collected half is not reported in full");
    region.base[region.page] = 1;
    Require(region.Collect(&GuestWriteWatchCollectFresh_nid_postfix) == std::set<std::size_t>({4, 5, 6, 7}), "a fresh collection does not report exactly the never collected pages");
    Require(region.Collect(&GuestWriteWatchCollectFresh_nid_postfix).empty(), "a fresh collection reports pages again");
    region.base[6 * region.page] = 1;
    Require(region.Collect() == std::set<std::size_t>({1, 6}), "a fresh collection does not arm the pages it reports");
    Require(region.Collect().empty(), "collected writes are reported again");
}

void CheckArmed() {
    Region region(8);
    GuestWriteWatchRegister_nid_postfix(region.base, region.bytes);
    Require(region.Collect(&GuestWriteWatchCollect_nid_postfix, 0, 4) == All(4), "the collected half is not reported in full");
    region.base[region.page] = 1;
    region.base[6 * region.page] = 1;
    Require(region.Collect(&GuestWriteWatchCollectArmed_nid_postfix) == std::set<std::size_t>({1}), "an armed collection does not report exactly the written armed pages");
    Require(region.Collect() == std::set<std::size_t>({4, 5, 6, 7}), "an armed collection takes never collected pages");
    Require(region.Collect().empty(), "collected writes are reported again");
}

void CheckRegisterAgain() {
    Region region(4);
    GuestWriteWatchRegister_nid_postfix(region.base, region.bytes);
    Require(region.Collect() == All(4), "a new range is not reported in full");
    GuestWriteWatchRegister_nid_postfix(region.base, region.bytes);
    Require(region.Collect() == All(4), "a range registered again is not reported in full");
    region.base[2 * region.page] = 1;
    Require(region.Collect() == std::set<std::size_t>({2}), "a range registered while protected stops reporting writes");
    GuestWriteWatchRegister_nid_postfix(region.base + region.page, region.page);
    Require(region.Collect() == std::set<std::size_t>({1}), "registering part of a range does not report that part alone");
    region.base[0] = 1;
    region.base[3 * region.page] = 1;
    Require(region.Collect() == std::set<std::size_t>({0, 3}), "the rest of a split range stops reporting writes");
}

int KernelProtection(const void* page) {
    mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(page);
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    Require(mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object) == KERN_SUCCESS && address == reinterpret_cast<mach_vm_address_t>(page), "cannot read the kernel protection of a page");
    return ((info.protection & VM_PROT_READ) ? PROT_READ : 0) | ((info.protection & VM_PROT_WRITE) ? PROT_WRITE : 0) | ((info.protection & VM_PROT_EXECUTE) ? PROT_EXEC : 0);
}

void CheckRegisterMapped() {
    Region region(4);
    std::uintptr_t runEnd = 0;
    Require(::mprotect(region.base, region.bytes, PROT_NONE) == 0, "cannot reserve the test region");
    GuestWriteWatchRegister_nid_postfix(region.base, region.bytes);
    Require(::mmap(region.base, region.bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == region.base, "cannot map over the reservation");
    GuestWriteWatchRegister_nid_postfix(region.base, region.bytes);
    Require(GuestWriteWatchProtection_nid_postfix(region.Address(0), region.Address(4), &runEnd) == (PROT_READ | PROT_WRITE) && runEnd == region.Address(4), "a range mapped over its reservation keeps the reservation's protection");
    Require(region.Collect() == All(4), "a mapped range is not reported in full");
    region.base[2 * region.page] = 1;
    Require(region.Collect() == std::set<std::size_t>({2}), "a range mapped over its reservation stops reporting writes");
    Require(::mmap(region.base + region.page, region.page, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == region.base + region.page, "cannot reserve a watched page again");
    GuestWriteWatchRegister_nid_postfix(region.base + region.page, region.page);
    Require(GuestWriteWatchProtection_nid_postfix(region.Address(1), region.Address(2), &runEnd) == PROT_NONE, "a protected page reserved again reports its old protection");
    Require(KernelProtection(region.base + region.page) == PROT_NONE, "registering a protected page reserved again restored its old protection");
}

void CheckHostWrite() {
    Region region(4);
    GuestWriteWatchRegister_nid_postfix(region.base, region.bytes);
    Require(region.Collect() == All(4), "a new range is not reported in full");
    int descriptors[2];
    Require(::pipe(descriptors) == 0, "cannot create a pipe");
    const std::array<char, 8> payload{'w', 'a', 't', 'c', 'h', 'e', 'd', '!'};
    Require(::write(descriptors[1], payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()), "cannot fill the pipe");
    Require(::read(descriptors[0], region.base + region.page, payload.size()) == static_cast<ssize_t>(payload.size()), "a system call cannot write a watched page");
    Require(region.Collect() == std::set<std::size_t>({1}), "a system call write is not reported");
    Require(::write(descriptors[1], payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()), "cannot fill the pipe again");
    GuestWriteWatchBeginHostWrite_nid_postfix(region.base + 2 * region.page, payload.size());
    Require(region.Collect() == std::set<std::size_t>({2}), "a page under a host write is not reported");
    Require(::read(descriptors[0], region.base + 2 * region.page, payload.size()) == static_cast<ssize_t>(payload.size()), "a page under a host write cannot be written");
    Require(region.Collect() == std::set<std::size_t>({2}), "a page under a host write is not reported at every collection");
    GuestWriteWatchEndHostWrite_nid_postfix(region.base + 2 * region.page, payload.size());
    Require(std::memcmp(region.base + 2 * region.page, payload.data(), payload.size()) == 0, "the host write did not reach the page");
    Require(region.Collect() == std::set<std::size_t>({2}), "a host write is not reported after it ends");
    Require(region.Collect().empty(), "a finished host write is reported again");
    ::close(descriptors[0]);
    ::close(descriptors[1]);
}

void CheckProtection() {
    Region region(6);
    std::uintptr_t runEnd = 0;
    Require(GuestWriteWatchProtection_nid_postfix(region.Address(0), region.Address(6), &runEnd) == -1 && runEnd == region.Address(6), "an unwatched range reports a protection");
    GuestWriteWatchRegister_nid_postfix(region.base + 2 * region.page, 3 * region.page);
    Require(GuestWriteWatchProtection_nid_postfix(region.Address(0), region.Address(6), &runEnd) == -1 && runEnd == region.Address(2), "the run before a watched range does not end at it");
    static_cast<void>(GuestWriteWatchCollect_nid_postfix(region.Address(2), 3 * region.page, [](void*, std::uintptr_t, std::uintptr_t) {}, nullptr));
    Require(GuestWriteWatchProtection_nid_postfix(region.Address(2), region.Address(6), &runEnd) == (PROT_READ | PROT_WRITE) && runEnd == region.Address(5), "a protected page does not report the protection it was given");
    Require(::mprotect(region.base + 3 * region.page, region.page, PROT_READ) == 0, "cannot change the protection");
    GuestWriteWatchProtectionChanged_nid_postfix(region.base + 3 * region.page, region.page, PROT_READ);
    Require(GuestWriteWatchProtection_nid_postfix(region.Address(2), region.Address(6), &runEnd) == (PROT_READ | PROT_WRITE) && runEnd == region.Address(3), "a run does not end where the protection changes");
    Require(GuestWriteWatchProtection_nid_postfix(region.Address(3), region.Address(6), &runEnd) == PROT_READ && runEnd == region.Address(4), "a changed protection is not reported");
    std::set<std::size_t> pages;
    static_cast<void>(GuestWriteWatchCollect_nid_postfix(region.Address(2), 3 * region.page, [](void* context, std::uintptr_t begin, std::uintptr_t) { static_cast<std::set<std::size_t>*>(context)->insert(begin); }, &pages));
    Require(pages == std::set<std::size_t>({region.Address(3)}), "a protection change is not reported as a write");
    Require(::mprotect(region.base + 3 * region.page, region.page, PROT_READ | PROT_WRITE) == 0, "cannot restore the protection");
    GuestWriteWatchProtectionChanged_nid_postfix(region.base + 3 * region.page, region.page, PROT_READ | PROT_WRITE);
    region.base[3 * region.page] = 1;
    GuestWriteWatchUnregister_nid_postfix(region.base + 2 * region.page, 3 * region.page);
    region.base[2 * region.page] = 1;
    region.base[4 * region.page] = 1;
    Require(!GuestWriteWatchCovers_nid_postfix(region.Address(2), region.page), "an unregistered range is covered");
}

void CheckThreads() {
    constexpr std::size_t Threads = 4;
    constexpr std::size_t PagesPerThread = 64;
    constexpr int Rounds = 50;
    Region region(Threads * PagesPerThread);
    GuestWriteWatchRegister_nid_postfix(region.base, region.bytes);
    Require(region.Collect() == All(Threads * PagesPerThread), "a new range is not reported in full");
    std::atomic<bool> start{false};
    std::atomic<std::size_t> finished{0};
    std::array<std::thread, Threads> writers;
    for (std::size_t thread = 0; thread < Threads; ++thread) {
        writers[thread] = std::thread([&, thread] {
            while (!start.load()) std::this_thread::yield();
            for (int round = 1; round <= Rounds; ++round)
                for (std::size_t index = 0; index < PagesPerThread; ++index) region.base[(thread * PagesPerThread + index) * region.page] = static_cast<std::uint8_t>(round);
            finished.fetch_add(1);
        });
    }
    std::vector<std::uint8_t> seen(Threads * PagesPerThread, 0);
    const auto take = [&] {
        for (const auto index : region.Collect()) seen[index] = region.base[index * region.page];
    };
    start.store(true);
    while (finished.load() != Threads) {
        take();
        std::this_thread::yield();
    }
    for (auto& writer : writers) writer.join();
    take();
    for (std::size_t index = 0; index < seen.size(); ++index)
        Require(seen[index] == static_cast<std::uint8_t>(Rounds), "a write that raced a collection was not reported");
    Require(region.Collect().empty(), "writes are reported after the writers stopped");
}

void StoreThirtyTwo(std::uint8_t* destination, std::uint8_t value) {
#if defined(__AVX__)
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination), _mm256_set1_epi8(static_cast<char>(value)));
#else
    std::array<std::uint8_t, 32> vector;
    vector.fill(value);
    std::memcpy(destination, vector.data(), vector.size());
#endif
}

void CheckVector() {
    Region region(8);
    GuestWriteWatchRegister_nid_postfix(region.base, region.bytes);
    Require(region.Collect() == All(8), "a new range is not reported in full");
    StoreThirtyTwo(region.base + 3 * region.page - 16, 5);
    Require(region.Collect() == std::set<std::size_t>({2, 3}), "a vector store across a page boundary is not reported on both pages");
    Require(region.base[3 * region.page - 16] == 5 && region.base[3 * region.page + 15] == 5, "a vector store across a page boundary was lost");
    mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(region.base);
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    Require(mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object) == KERN_SUCCESS, "cannot read the kernel protection of the watched range");
    Require(address <= reinterpret_cast<mach_vm_address_t>(region.base) && address + size >= reinterpret_cast<mach_vm_address_t>(region.base + region.bytes) && (info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE), "a collection changed the kernel protection of a watched page");
    std::atomic<bool> stored{false};
    std::thread blocked([&] {
        sigset_t all;
        sigfillset(&all);
        pthread_sigmask(SIG_BLOCK, &all, nullptr);
        StoreThirtyTwo(region.base + 6 * region.page - 16, 9);
        stored.store(true);
    });
    for (int wait = 0; wait < 500 && !stored.load(); ++wait) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!stored.load()) {
        std::fprintf(stderr, "a vector store from a thread with every signal blocked did not complete\n");
        std::_Exit(1);
    }
    blocked.join();
    Require(region.Collect() == std::set<std::size_t>({5, 6}), "a vector store from a thread with every signal blocked is not reported");
}

void CheckReservation() {
    constexpr std::size_t Bytes = std::size_t{16} << 30;
    constexpr int Rounds = 200;
    const auto page = static_cast<std::size_t>(::getpagesize());
    auto* base = static_cast<std::uint8_t*>(::mmap(nullptr, Bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
    Require(base != MAP_FAILED, "cannot reserve the test range");
    Require(::mprotect(base, 4 * page, PROT_READ | PROT_WRITE) == 0, "cannot commit the start of the reservation");
    const auto start = std::chrono::steady_clock::now();
    GuestWriteWatchRegister_nid_postfix(base, Bytes);
    std::size_t reported = 0;
    Require(GuestWriteWatchCollect_nid_postfix(reinterpret_cast<std::uintptr_t>(base), 4 * page, [](void* context, std::uintptr_t begin, std::uintptr_t end) { *static_cast<std::size_t*>(context) += end - begin; }, &reported) && reported == 4 * page, "the committed start of a reservation is not reported");
    for (int round = 0; round < Rounds; ++round) {
        GuestWriteWatchRegister_nid_postfix(base, Bytes);
        for (std::size_t bytes = Bytes / 2; bytes >= (std::size_t{256} << 20); bytes /= 2) {
            GuestWriteWatchRegister_nid_postfix(base + bytes, bytes);
            Require(GuestWriteWatchUnregister_nid_postfix(base + bytes, bytes), "a registered part of a reservation is not unregistered");
        }
        Require(GuestWriteWatchCovers_nid_postfix(reinterpret_cast<std::uintptr_t>(base), page), "the start of a reservation is not covered");
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    base[page] = 1;
    reported = 0;
    Require(GuestWriteWatchCollect_nid_postfix(reinterpret_cast<std::uintptr_t>(base), 4 * page, [](void* context, std::uintptr_t begin, std::uintptr_t end) { *static_cast<std::size_t*>(context) += end - begin; }, &reported) && reported == 4 * page, "a reservation registered again is not reported in full");
    Require(GuestWriteWatchUnregister_nid_postfix(base, Bytes), "the reservation is not unregistered");
    Require(!GuestWriteWatchCovers_nid_postfix(reinterpret_cast<std::uintptr_t>(base), page), "an unregistered reservation is covered");
    ::munmap(base, Bytes);
    Require(elapsed < std::chrono::seconds(2), "registering a large reservation costs time per page");
}


// A range past the shared-compare threshold (APS5_WATCH_COMPARE_MIN_PAGES) is compared by the
// helper crew: every changed page, and only those, is reported, from any thread, including pages
// changed in the first and last chunk, and partial collects of it agree.
void CheckLarge() {
    constexpr std::size_t Pages = 1024;
    Region region(Pages);
    GuestWriteWatchRegister_nid_postfix(region.base, region.bytes);
    std::memset(region.base, 7, region.bytes);
    Require(region.Collect() == All(Pages), "a never-collected large range is not reported in full");
    Require(region.Collect().empty(), "an unchanged large range is reported");
    for (int round = 0; round < 64; ++round) {
        std::set<std::size_t> expected;
        for (std::size_t index : {std::size_t{0}, std::size_t{15}, std::size_t{16}, Pages - 1, static_cast<std::size_t>(round) * 13 % Pages, static_cast<std::size_t>(round) * 977 % Pages}) {
            region.base[index * region.page + (static_cast<std::size_t>(round) * 61) % region.page] ^= 0x5a;
            expected.insert(index);
        }
        std::set<std::size_t> pages;
        if (round % 2 == 0) pages = region.Collect();
        else std::thread([&] { pages = region.Collect(); }).join();
        Require(pages == expected, "a large collect reports other pages than the changed ones");
        Require(region.Collect().empty(), "a large collect leaves a reported page reported again");
    }
    region.base[700 * region.page] ^= 1;
    Require(region.Collect(&GuestWriteWatchCollect_nid_postfix, 0, 512).empty(), "a partial collect reports a page outside it");
    Require(region.Collect(&GuestWriteWatchCollect_nid_postfix, 512, 512) == std::set<std::size_t>{700}, "a partial large collect misses its changed page");
    std::vector<std::thread> threads;
    std::atomic<bool> wrong{false};
    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&, thread] {
            for (int round = 0; round < 50; ++round) {
                const auto pages = region.Collect(&GuestWriteWatchCollect_nid_postfix, static_cast<std::size_t>(thread) * 256, 256);
                if (!pages.empty()) wrong = true;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    Require(!wrong, "concurrent large collects of an unchanged range report pages");
    // The cost of an unchanged collect of the whole range (compare with APS5_WATCH_COMPARE_HELPERS=0).
    constexpr int Rounds = 200;
    const auto start = std::chrono::steady_clock::now();
    for (int round = 0; round < Rounds; ++round) Require(region.Collect().empty(), "an unchanged large range is reported while timed");
    const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / Rounds;
    std::printf("unchanged collect of %zu KiB: %.1f us\n", region.bytes >> 10, us);
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected collect, fresh, armed, register, mapped, host, protection, threads, vector, reservation or large");
        const std::string_view mode(argv[1]);
        if (mode == "collect") CheckCollect();
        else if (mode == "fresh") CheckFresh();
        else if (mode == "armed") CheckArmed();
        else if (mode == "register") CheckRegisterAgain();
        else if (mode == "mapped") CheckRegisterMapped();
        else if (mode == "host") CheckHostWrite();
        else if (mode == "protection") CheckProtection();
        else if (mode == "threads") CheckThreads();
        else if (mode == "vector") CheckVector();
        else if (mode == "reservation") CheckReservation();
        else if (mode == "large") CheckLarge();
        else throw std::runtime_error("unknown write watch test");
        std::printf("write watch %s passed\n", argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
