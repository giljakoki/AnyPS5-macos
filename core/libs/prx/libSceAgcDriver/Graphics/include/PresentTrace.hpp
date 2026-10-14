#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace AgcDriver::Graphics {

// Debug aid: APS5_TRACE_PRESENT_DRAWS=1 prints, per queue-0 flip, one [present-draws] line with
// every colour target drawn since the previous flip (the worker's draw packets per CB0 address and
// their verdicts, the draws Graphics recorded per target with the resident image and its write
// generations, recorded on the pipeline's committer or directly) and every texture bound by those
// draws over a surface drawn earlier in the same frame (a view of which storage image, or a CPU
// snapshot, and how many draws the surface had had when it was first and last sampled); and per
// present one line naming the presented image and the flip that drew it. The output stays bounded
// (at most 48 targets and 48 sampled surfaces per flip); with the variable unset nothing is kept.
class PresentTrace {
public:
    static bool Enabled() {
        static const bool enabled = std::getenv("APS5_TRACE_PRESENT_DRAWS") != nullptr;
        return enabled;
    }

    // Set by the draw pipeline's committer around each item it runs.
    static bool& Committing() {
        static thread_local bool committing = false;
        return committing;
    }

    // The write tracker's generation when the worker queued the draw being committed (0: none).
    static std::uint64_t& EnqueueGeneration() {
        static thread_local std::uint64_t generation = 0;
        return generation;
    }

    // A committed draw into `target` read [begin, end) (kind as GuestBufferMemory::TracedRegions, 'i'
    // the index buffer), which the CPU wrote after the worker queued the draw.
    static void WindowWrite(std::uint64_t target, std::uint64_t begin, std::uint64_t end, char kind) {
        if (!Enabled()) return;
        auto& self = instance();
        std::lock_guard lock(self.mutex);
        auto it = self.frame.windowWrites.find(begin);
        if (it == self.frame.windowWrites.end()) {
            if (self.frame.windowWrites.size() >= Bound) {
                ++self.frame.droppedWindowWrites;
                return;
            }
            it = self.frame.windowWrites.emplace(begin, WindowWritten{end - begin, target, kind, 0}).first;
        }
        ++it->second.count;
    }

    // A committed draw whose reads were checked for CPU writes since its enqueue.
    static void WindowChecked(bool clean) {
        if (!Enabled()) return;
        auto& self = instance();
        std::lock_guard lock(self.mutex);
        ++(clean ? self.frame.windowClean : self.frame.windowDirty);
    }

    // A queue-0 draw packet the worker handled with CB0 at `address`; verdict 0 drawn (recorded or
    // queued), 1 nothing to draw, 2 rejected or thrown.
    static void WorkerDraw(std::uint64_t address, int verdict) {
        if (!Enabled()) return;
        auto& self = instance();
        std::lock_guard lock(self.mutex);
        auto* target = self.target(address);
        if (target == nullptr) return;
        ++(verdict == 0 ? target->worker : verdict == 1 ? target->workerNothing : target->workerFailed);
    }

    // A draw Graphics recorded into the colour target at `address` (kind 'f' full, 'r' recipe, 'c'
    // copied in and out, 'n' nothing to draw); `image` is the resident storage image (or null).
    static void TargetDraw(std::uint64_t address, const void* image, std::uint64_t generation, char kind) {
        if (!Enabled()) return;
        auto& self = instance();
        std::lock_guard lock(self.mutex);
        auto* target = self.target(address);
        if (target == nullptr) return;
        const bool pipelined = Committing();
        ++(pipelined ? target->pipelined : target->direct);
        switch (kind) {
            case 'f': ++target->full; break;
            case 'r': ++target->recipe; break;
            case 'c': ++target->copied; break;
            default: ++target->nothing; return;
        }
        if (target->image == nullptr && image != nullptr) {
            target->image = image;
            target->firstGeneration = generation;
        } else if (image != nullptr && image != target->image) {
            target->otherImage = image;
            ++target->imageChanges;
        }
        target->lastGeneration = generation;
    }

    // A texture a draw bound over the surface at `address`: `source` is the storage image a view
    // follows (null: a snapshot read at `generation`). Kept only for surfaces drawn this frame.
    static void Sampled(std::uint64_t address, const void* source, std::uint64_t generation) {
        if (!Enabled()) return;
        auto& self = instance();
        std::lock_guard lock(self.mutex);
        auto found = self.frame.targets.find(address);
        if (found == self.frame.targets.end()) {
            // A surface the previous frame drew, sampled before this frame draws it.
            if (self.finished.empty() || !self.finished.back().targets.contains(address) || self.target(address) == nullptr) return;
            found = self.frame.targets.find(address);
        }
        const auto draws = found->second.full + found->second.recipe + found->second.copied;
        const auto key = std::make_pair(address, source);
        auto it = self.frame.samples.find(key);
        if (it == self.frame.samples.end()) {
            if (self.frame.samples.size() >= Bound) {
                ++self.frame.droppedSamples;
                return;
            }
            it = self.frame.samples.emplace(key, Sample{}).first;
            it->second.firstAt = draws;
            it->second.generation = generation;
            it->second.sameImage = source != nullptr && source == found->second.image;
        }
        ++it->second.count;
        it->second.lastAt = draws;
        ++(Committing() ? it->second.pipelined : it->second.direct);
    }

    // A draw whose failure was skipped (SkipFailedWork).
    static void Skipped() {
        if (!Enabled()) return;
        auto& self = instance();
        std::lock_guard lock(self.mutex);
        ++self.frame.skipped;
    }

    // The worker reached a queue-0 flip (after the pipeline drained): prints the frame.
    static void Flip() {
        if (!Enabled()) return;
        auto& self = instance();
        std::string line;
        {
            std::lock_guard lock(self.mutex);
            const auto flip = ++self.flips;
            line = self.describe(flip);
            Finished finished{flip, {}};
            for (const auto& [address, target] : self.frame.targets) finished.targets.emplace(address, target.image);
            self.finished.push_back(std::move(finished));
            while (self.finished.size() > 16) self.finished.pop_front();
            self.frame = {};
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }

    // The presenter showed the display buffer at `address` from `image` (null: through guest memory).
    static void Present(int dump, std::uint64_t address, const void* image, std::uint64_t generation, const char* how) {
        if (!Enabled()) return;
        auto& self = instance();
        long long flip = -1;
        const void* drawnImage = nullptr;
        std::uint64_t serial = 0;
        {
            std::lock_guard lock(self.mutex);
            serial = ++self.presents;
            for (auto it = self.finished.rbegin(); it != self.finished.rend(); ++it) {
                const auto found = it->targets.find(address);
                if (found == it->targets.end()) continue;
                flip = static_cast<long long>(it->flip);
                drawnImage = found->second;
                break;
            }
        }
        std::fprintf(stderr, "[present-draws] present=%llu dump=%d buffer=%llx image=%llx@%llu how=%s flip=%lld drawn-image=%llx%s\n", static_cast<unsigned long long>(serial), dump, static_cast<unsigned long long>(address), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(image)), static_cast<unsigned long long>(generation), how, flip, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(drawnImage)), drawnImage != nullptr && image != nullptr && drawnImage != image ? " IMAGE-DIFFERS" : "");
    }

private:
    static constexpr std::size_t Bound = 48;

    struct Target {
        const void* image = nullptr;
        const void* otherImage = nullptr;
        std::uint64_t firstGeneration = 0;
        std::uint64_t lastGeneration = 0;
        std::uint32_t worker = 0, workerNothing = 0, workerFailed = 0;
        std::uint32_t full = 0, recipe = 0, copied = 0, nothing = 0;
        std::uint32_t pipelined = 0, direct = 0;
        std::uint32_t imageChanges = 0;
    };
    struct Sample {
        std::uint32_t count = 0, firstAt = 0, lastAt = 0, pipelined = 0, direct = 0;
        std::uint64_t generation = 0;
        bool sameImage = false;
    };
    struct WindowWritten {
        std::uint64_t bytes;
        std::uint64_t target;
        char kind;
        std::uint32_t count;
    };
    struct Frame {
        std::map<std::uint64_t, WindowWritten> windowWrites;
        std::uint32_t droppedWindowWrites = 0, windowClean = 0, windowDirty = 0;
        std::map<std::uint64_t, Target> targets;
        std::map<std::pair<std::uint64_t, const void*>, Sample> samples;
        std::uint32_t droppedTargets = 0, droppedSamples = 0, skipped = 0;
    };
    struct Finished {
        std::uint64_t flip;
        std::map<std::uint64_t, const void*> targets;
    };

    static PresentTrace& instance() {
        static PresentTrace& trace = *new PresentTrace();
        return trace;
    }

    Target* target(std::uint64_t address) {
        if (const auto found = frame.targets.find(address); found != frame.targets.end()) return &found->second;
        if (frame.targets.size() >= Bound) {
            ++frame.droppedTargets;
            return nullptr;
        }
        return &frame.targets[address];
    }

    std::string describe(std::uint64_t flip) const {
        std::string line = "[present-draws] flip=" + std::to_string(flip);
        char text[256];
        for (const auto& [address, t] : frame.targets) {
            // t=<address>:<image>@<first>-<last generation>:w<worker drawn>/<nothing>/<failed>:f<full>r<recipe>c<copied>n<nothing>:p<pipelined>d<direct>[:changes<n>]
            std::snprintf(text, sizeof(text), " t=%llx:%llx@%llu-%llu:w%u/%u/%u:f%ur%uc%un%u:p%ud%u", static_cast<unsigned long long>(address), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(t.image)), static_cast<unsigned long long>(t.firstGeneration), static_cast<unsigned long long>(t.lastGeneration), t.worker, t.workerNothing, t.workerFailed, t.full, t.recipe, t.copied, t.nothing, t.pipelined, t.direct);
            line += text;
            if (t.imageChanges != 0) {
                std::snprintf(text, sizeof(text), ":changes%u(%llx)", t.imageChanges, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(t.otherImage)));
                line += text;
            }
        }
        for (const auto& [key, s] : frame.samples) {
            // s=<address>:<view <image>[=target image]|snap@<generation>>:x<count>@<first>-<last draws of the target>/<total>:p<pipelined>d<direct>
            const auto& t = frame.targets.at(key.first);
            const auto total = t.full + t.recipe + t.copied;
            if (key.second != nullptr) std::snprintf(text, sizeof(text), " s=%llx:view%s%llx:x%u@%u-%u/%u:p%ud%u", static_cast<unsigned long long>(key.first), s.sameImage ? "=" : "!", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(key.second)), s.count, s.firstAt, s.lastAt, total, s.pipelined, s.direct);
            else std::snprintf(text, sizeof(text), " s=%llx:snap@%llu:x%u@%u-%u/%u:p%ud%u", static_cast<unsigned long long>(key.first), static_cast<unsigned long long>(s.generation), s.count, s.firstAt, s.lastAt, total, s.pipelined, s.direct);
            line += text;
        }
        if (frame.windowClean != 0 || frame.windowDirty != 0) {
            // window=<committed draws whose reads the CPU left alone>/<draws reading CPU-written bytes>
            std::snprintf(text, sizeof(text), " window=%u/%u", frame.windowClean, frame.windowDirty);
            line += text;
        }
        for (const auto& [begin, w] : frame.windowWrites) {
            // ww=<begin>+<bytes>:<kind>:x<draws>:t<target>
            std::snprintf(text, sizeof(text), " ww=%llx+%llx:%c:x%u:t%llx", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(w.bytes), w.kind, w.count, static_cast<unsigned long long>(w.target));
            line += text;
        }
        if (frame.droppedWindowWrites != 0) line += " ww-dropped=" + std::to_string(frame.droppedWindowWrites);
        if (frame.droppedTargets != 0 || frame.droppedSamples != 0) {
            std::snprintf(text, sizeof(text), " dropped=%u/%u", frame.droppedTargets, frame.droppedSamples);
            line += text;
        }
        if (frame.skipped != 0) line += " skipped=" + std::to_string(frame.skipped);
        return line;
    }

    std::mutex mutex;
    Frame frame;
    std::deque<Finished> finished;
    std::uint64_t presents = 0;
    std::uint64_t flips = 0;
};

}
