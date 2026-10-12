#include "prx/libSceAgcDriver/Execution/include/Driver/Packets/HitchSampler.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#ifdef __APPLE__
#include <atomic>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mutex>
#include <thread>
#endif

namespace AgcDriver::DriverDetail {

std::string SummarizeSamples(const std::vector<ThreadSample>& samples, const std::function<std::string(std::uint64_t)>& symbolize, std::size_t shown) {
    std::map<std::string, std::size_t> leaves, callers;
    for (const auto& sample : samples) {
        const auto leaf = symbolize(sample.frames[0]);
        ++leaves[leaf];
        const auto module = leaf.substr(0, leaf.find('!'));
        std::string caller = "none";
        for (std::size_t i = 1; i < sample.frames.size() && sample.frames[i] != 0; ++i) {
            const auto frame = symbolize(sample.frames[i]);
            if (frame.substr(0, frame.find('!')) == module) continue;
            caller = frame;
            break;
        }
        ++callers[module + " <- " + caller];
    }
    const auto top = [&](const std::map<std::string, std::size_t>& counts) {
        std::vector<std::pair<std::string, std::size_t>> sorted(counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        std::string text;
        for (std::size_t i = 0; i < sorted.size() && i < shown; ++i) {
            char part[48];
            std::snprintf(part, sizeof(part), " %.0f%% ", samples.empty() ? 0.0 : 100.0 * static_cast<double>(sorted[i].second) / static_cast<double>(samples.size()));
            text += part + sorted[i].first + (i + 1 < sorted.size() && i + 1 < shown ? "," : "");
        }
        return text;
    };
    return std::to_string(samples.size()) + " samples; leaves:" + top(leaves) + "; entered from:" + top(callers);
}

#ifdef __APPLE__

namespace {

std::uint64_t ReadWord(std::uint64_t address) {
    std::uint64_t value = 0;
    mach_vm_size_t read = 0;
    if (address == 0 || mach_vm_read_overwrite(mach_task_self(), address, sizeof(value), reinterpret_cast<mach_vm_address_t>(&value), &read) != KERN_SUCCESS || read != sizeof(value)) return 0;
    return value;
}

class MachHitchSampler final : public HitchSampler {
public:
    MachHitchSampler() : ring(16384) {
        std::thread([this] { run(); }).detach();
    }

    void Follow() override {
        const auto self = mach_thread_self();
        const auto previous = followed.exchange(self, std::memory_order_acq_rel);
        if (previous != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), previous);
    }

    std::string Describe(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) override {
        std::vector<ThreadSample> window;
        {
            std::lock_guard lock(mutex);
            for (std::size_t i = 0; i < std::min(written, ring.size()); ++i) {
                const auto& sample = ring[(written - 1 - i) % ring.size()];
                if (sample.at < from) break;
                if (sample.at <= to) window.push_back(sample);
            }
        }
        return SummarizeSamples(window, [this](std::uint64_t address) { return symbolize(address); }, 6);
    }

    std::string Period() override {
        std::vector<ThreadSample> period;
        {
            std::lock_guard lock(mutex);
            const auto count = std::min(written - reported, ring.size());
            for (std::size_t i = 0; i < count; ++i) period.push_back(ring[(written - count + i) % ring.size()]);
            reported = written;
        }
        return SummarizeSamples(period, [this](std::uint64_t address) { return symbolize(address); }, 12);
    }

private:
    std::string symbolize(std::uint64_t address) {
        std::lock_guard lock(symbolMutex);
        const auto found = symbols.find(address);
        if (found != symbols.end()) return found->second;
        Dl_info info{};
        std::string text;
        if (dladdr(reinterpret_cast<const void*>(address), &info) != 0 && info.dli_fname != nullptr) {
            const char* base = std::strrchr(info.dli_fname, '/');
            text = base != nullptr ? base + 1 : info.dli_fname;
            if (info.dli_sname != nullptr && address - reinterpret_cast<std::uint64_t>(info.dli_saddr) < 0x10000) text += std::string("!") + info.dli_sname;
            else {
                char offset[32];
                std::snprintf(offset, sizeof(offset), "!+0x%llx", static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(info.dli_fbase)));
                text += offset;
            }
        } else {
            char unknown[40];
            std::snprintf(unknown, sizeof(unknown), "?!0x%llx", static_cast<unsigned long long>(address & ~std::uint64_t{0xfff}));
            text = unknown;
        }
        symbols.emplace(address, text);
        return text;
    }

    void run() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const auto thread = followed.load(std::memory_order_acquire);
            if (thread == MACH_PORT_NULL || thread_suspend(thread) != KERN_SUCCESS) continue;
            x86_thread_state64_t state{};
            mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
            const auto status = thread_get_state(thread, x86_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count);
            ThreadSample sample{std::chrono::steady_clock::now(), {}};
            if (status == KERN_SUCCESS) {
                sample.frames[0] = state.__rip;
                std::uint64_t frame = state.__rbp;
                for (std::size_t i = 1; i < sample.frames.size() && frame != 0; ++i) {
                    sample.frames[i] = ReadWord(frame + 8);
                    const auto next = ReadWord(frame);
                    if (next <= frame) break;
                    frame = next;
                }
            }
            thread_resume(thread);
            if (status != KERN_SUCCESS) continue;
            std::lock_guard lock(mutex);
            ring[written % ring.size()] = sample;
            ++written;
        }
    }

    std::atomic<thread_act_t> followed{MACH_PORT_NULL};
    std::mutex mutex;
    std::vector<ThreadSample> ring;
    std::size_t written = 0;
    std::size_t reported = 0;
    std::mutex symbolMutex;
    std::unordered_map<std::uint64_t, std::string> symbols;
};

}

HitchSampler* HitchSampler::Get() {
    static HitchSampler* const sampler = std::getenv("APS5_TRACE_HITCH_SAMPLES") != nullptr ? new MachHitchSampler() : nullptr;
    return sampler;
}

#else

HitchSampler* HitchSampler::Get() {
    if (std::getenv("APS5_TRACE_HITCH_SAMPLES") != nullptr) throw std::runtime_error("APS5_TRACE_HITCH_SAMPLES samples threads through Mach thread states and is implemented on macOS only");
    return nullptr;
}

#endif

}
