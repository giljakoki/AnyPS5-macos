#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWCACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWCACHE_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <vector>
#include "prx/libc/include/general/AtomicSharedPtr.hpp"

namespace AgcDriver::DriverDetail {

struct DrawCacheGovernor {
    static constexpr std::uint32_t Window = 4096;
    static std::uint32_t MinimumHits() {
        static const std::uint32_t hits = [] {
            const char* text = std::getenv("APS5_DRAW_CACHE_MIN_HITS");
            return text != nullptr ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10)) : Window / 10;
        }();
        return hits;
    }
    static constexpr std::uint32_t Pause = 16384;
    std::uint32_t lookups = 0;
    std::uint32_t hits = 0;
    std::uint32_t paused = 0;
    std::uint32_t streak = 0;
    bool report = true;

    bool Use() {
        static const bool disabled = std::getenv("APS5_NO_DRAW_CACHE_GOVERNOR") != nullptr;
        if (disabled || paused == 0) return true;
        --paused;
        return false;
    }

    void Note(bool hit) {
        ++lookups;
        if (hit) ++hits;
        if (lookups < Window) return;
        if (hits < MinimumHits()) {
            paused = Pause << std::min<std::uint32_t>(streak, 6);
            streak = std::min<std::uint32_t>(streak + 1, 6);
            if (report) std::fprintf(stderr, "[draw-cache] %u of the last %u lookups hit: draw entries are skipped for the next %u draws of this queue\n", hits, Window, paused);
        } else {
            streak = 0;
        }
        lookups = 0;
        hits = 0;
    }
};


struct DrawProgram {
    ShaderRecompiler::ShaderBinary binary;
    std::uint32_t userDataBase;
    std::uint32_t firstUserSgpr = 8;
    std::vector<std::uint32_t> userData;
    std::array<ShaderRecompiler::MemoryRegion, 2> memory;

    std::shared_ptr<const ShaderSnapshot> snapshot;
    std::size_t codeOffset = 0;
};

struct DrawDecode {
    Graphics::State state;
    ShaderRecompiler::ShaderPixelStageInfo pixel;
    std::vector<DrawProgram> programs;
    std::vector<ShaderRecompiler::ProgramRole> roles;
};

void DecodeGraphicsPrograms(DrawDecode& decoded, const QueueState& queue, const ShaderRegistry& registry, bool staticAbi, bool includeFragment);

struct PreparedGraphicsStage {
    std::shared_ptr<const ShaderSnapshot> snapshot;
    PreparedShaders::Entry entry;
};

std::vector<PreparedGraphicsStage> PrepareGraphicsStages(const DrawDecode& decoded, const ShaderRecompiler::SpirvTarget& target, bool meshEmulation = false);

struct DrawRecipeRecord {
    std::vector<std::weak_ptr<const DispatchVariant>> stages;
    std::shared_ptr<const DrawRecipe> recipe;
    bool Matches(const std::vector<std::shared_ptr<DispatchVariant>>& variants) const;
    bool Expired() const;
};

struct DrawEntry {

    std::shared_ptr<const DrawDecode> decode;

    std::vector<std::vector<std::shared_ptr<DispatchVariant>>> stages;

    AtomicSharedPtr<const std::vector<DrawRecipeRecord>> recipes;
    std::uint64_t touched = 0;
    std::list<std::uint64_t>::iterator order;
};

enum class DrawMiss : std::size_t { FrontDiffering, FragmentDiffering, OtherDiffering, Layout, Gate, Stages, Count };

struct DrawEntryCounters {
    std::uint64_t lookups = 0, absent = 0, hits = 0, stageValidations = 0, stageEqual = 0, variantsCompared = 0, inserts = 0, variantsInserted = 0, variantsEvicted = 0, present = 0, unstable = 0, touches = 0, verifyHits = 0, verifyMismatches = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrawMiss::Count)> misses{};
    std::array<std::uint64_t, MaxDispatchVariants> variantHitsByRank{};
    double validateUs = 0;

    std::uint64_t registerKeyLookups = 0, registerKeyHits = 0, decodeSkipped = 0, decodePartial = 0, facadeMismatches = 0, verifyDecodes = 0, verifyDecodeMismatches = 0;
    double keyUs = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

enum class DrawVerdict { Drawn, Nothing, Rejected };

struct StageCapture {
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiled;
    std::vector<ShaderRecompiler::MemoryRegion> regions;
    std::uint64_t forgetSerial = 0;
    std::uint32_t pushOffset = 0;
};

bool DecodeProgramsCurrent(const DrawDecode& decode, const ShaderRegistry& registry);

}

#endif
