#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>

// Draw-cache hits across moved user-data pointers, ported from boykopovar/AnyPS5#4943 (with the
// words a walk never reads, which only reach the shader through its shader data, as free words).

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::size_t MaxAliases = 16384;
constexpr std::size_t MaxRelocations = 4096;
constexpr std::size_t MaxContentWords = 64;

std::uint64_t AlignedWord(std::uint32_t word) {
    return word & ~std::uint32_t{3};
}

template <typename TMap>
std::size_t dropEveryOther(TMap& map) {
    std::size_t dropped = 0;
    bool drop = false;
    for (auto it = map.begin(); it != map.end();) {
        drop = !drop;
        if (!drop) {
            ++it;
            continue;
        }
        it = map.erase(it);
        ++dropped;
    }
    return dropped;
}

}

bool Driver::drawRelocationEnabled() {
    static const bool enabled = std::getenv("APS5_NO_DRAW_RELOCATION") == nullptr;
    return enabled;
}

std::uint64_t Driver::relocatedContentKey(const RelocatableUserWord& word, std::uint32_t value, bool& readable) {
    std::uint64_t hash = 0x9e3779b97f4a7c15ull;
    const auto mix = [&](std::uint64_t mixed) {
        hash ^= mixed;
        hash *= 0x100000001b3ull;
    };
    mix(value & 3u);
    if (value == 0) {
        mix(0x4e554c4cull);
        return hash;
    }
    std::array<std::uint32_t, MaxContentWords> content;
    for (const auto& [offset, words] : word.reads) {
        mix(static_cast<std::uint64_t>(offset));
        mix(words);
        for (std::size_t done = 0; done < words; done += MaxContentWords) {
            const auto count = std::min<std::size_t>(words - done, MaxContentWords);
            const auto address = AlignedWord(value) + static_cast<std::uint64_t>(offset) + done * sizeof(std::uint32_t);
            if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(content).first(count))) != GuestMemory::Compare::Equal) {
                readable = false;
                return hash;
            }
            for (std::size_t i = 0; i < count; ++i) mix(content[i]);
        }
    }
    return hash;
}

void Driver::tagVariantRuns(DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions, const ShaderRecompiler::SrtUserDataUse& use, std::span<const Graphics::DecodeRead> decodeReads, bool eligible, std::uint32_t hiddenWords, std::size_t userWords) {
    eligible = eligible && drawRelocationEnabled();
    const std::uint64_t present = userWords >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << userWords) - 1;
    const std::uint64_t hidden = hiddenWords >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << hiddenWords) - 1;
    // Vertex fetch: the decode read the attribute and buffer tables through a 64-bit pointer in two
    // user words; the low word is dereferenced like a walked base, the high one is a value.
    std::uint64_t fetchBases = 0, fetchHigh = 0;
    for (const auto& read : decodeReads) {
        if (read.userWord < 0 || read.userWord >= 63) {
            eligible = false;
            continue;
        }
        fetchBases |= std::uint64_t{1} << read.userWord;
        fetchHigh |= std::uint64_t{1} << (read.userWord + 1);
    }
    const auto valued = use.asValue | fetchHigh;
    std::uint64_t relocatable = eligible ? (use.asBase | fetchBases) & ~valued & present & ~hidden : 0;
    variant.freeWords = eligible ? ~(use.asBase | fetchBases | valued) & present & ~hidden : 0;
    variant.baseWords = use.asBase | fetchBases;
    variant.valueWords = valued;
    variant.relocationEligible = eligible;
    std::vector<std::pair<std::uint64_t, std::int32_t>> reads;
    const auto covered = [&](std::uint64_t address) {
        return std::any_of(regions.begin(), regions.end(), [&](const ShaderRecompiler::MemoryRegion& region) { return address >= region.guestAddress && address - region.guestAddress + sizeof(std::uint32_t) <= region.bytes.size(); });
    };
    if (relocatable != 0) {
        reads = use.reads;
        for (const auto& read : decodeReads) {
            for (std::size_t word = 0; word < read.bytes.size() / sizeof(std::uint32_t); ++word) reads.emplace_back(read.address + word * sizeof(std::uint32_t), read.userWord);
        }
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        const auto drop = [&](std::int32_t origin) {
            if (origin >= 0 && origin < 64) relocatable &= ~(std::uint64_t{1} << origin);
        };
        for (std::size_t i = 0; i < reads.size(); ++i) {
            if (i + 1 < reads.size() && reads[i].first == reads[i + 1].first) {
                drop(reads[i].second);
                drop(reads[i + 1].second);
            }
            if (reads[i].second >= 0 && ((reads[i].first & 3u) != 0 || !covered(reads[i].first))) drop(reads[i].second);
        }
    }
    const auto origin = [&](std::uint64_t address) -> std::int8_t {
        const auto found = std::lower_bound(reads.begin(), reads.end(), std::make_pair(address, std::numeric_limits<std::int32_t>::min()));
        if (found == reads.end() || found->first != address || found->second < 0 || found->second >= 64) return -1;
        return (relocatable >> found->second & 1u) != 0 ? static_cast<std::int8_t>(found->second) : std::int8_t{-1};
    };
    // A captured region keeps its shape: it moves whole with the one pointer every word of it was
    // read through, or not at all (its pointers are not relocatable then).
    std::vector<std::int8_t> bases(regions.size(), -1);
    for (bool mixed = relocatable != 0; mixed;) {
        mixed = false;
        for (std::size_t r = 0; r < regions.size(); ++r) {
            const auto count = regions[r].bytes.size() / sizeof(std::uint32_t);
            const auto first = count != 0 ? origin(regions[r].guestAddress) : std::int8_t{-1};
            bases[r] = first;
            for (std::size_t word = 1; word < count; ++word) {
                const auto other = origin(regions[r].guestAddress + word * sizeof(std::uint32_t));
                if (other == first) continue;
                if (first >= 0) relocatable &= ~(std::uint64_t{1} << first);
                if (other >= 0) relocatable &= ~(std::uint64_t{1} << other);
                mixed = true;
                break;
            }
            if (mixed) break;
        }
    }
    variant.relocatableWords = relocatable;
    for (std::size_t r = 0; r < regions.size(); ++r) {
        const auto& region = regions[r];
        const auto count = region.bytes.size() / sizeof(std::uint32_t);
        const auto offset = variant.words.size();
        variant.words.resize(offset + count);
        std::memcpy(variant.words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
        variant.runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
        if (relocatable != 0) variant.runBases.push_back(bases[r]);
    }
}

std::shared_ptr<const DrawRelocation> Driver::learnDrawRelocation(std::uint64_t key, std::uint64_t alias, std::uint64_t identity) {
    const auto inserted = drawCache.find(key);
    const auto known = drawRelocations.find(identity);
    const std::shared_ptr<const DrawRelocation> current = known != drawRelocations.end() ? known->second : nullptr;
    if (inserted == drawCache.end() || inserted->second->decode == nullptr) return current;
    const auto& entry = *inserted->second;
    const auto& programs = entry.decode->programs;
    if (entry.stages.size() != programs.size()) return current;
    auto shape = std::make_shared<DrawRelocation>();
    if (current != nullptr) *shape = *current;
    bool changed = current == nullptr;
    const auto ban = [&](std::uint32_t reg) {
        const auto held = std::find_if(shape->words.begin(), shape->words.end(), [&](const RelocatableUserWord& word) { return word.reg == reg; });
        if (held != shape->words.end()) shape->words.erase(held);
        if (std::find(shape->banned.begin(), shape->banned.end(), reg) == shape->banned.end()) shape->banned.push_back(reg);
        changed = true;
    };
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size(); ++index) {
            const auto reg = program.userDataBase + index - program.hiddenUserWords;
            if (std::find(shape->banned.begin(), shape->banned.end(), reg) != shape->banned.end()) continue;
            if (entry.stages[i].empty() || index >= 64) {
                if (std::any_of(shape->words.begin(), shape->words.end(), [&](const RelocatableUserWord& word) { return word.reg == reg; })) ban(reg);
                continue;
            }
            const auto& variant = *entry.stages[i].front();
            const bool pointer = (variant.relocatableWords >> index & 1u) != 0;
            const bool free = (variant.freeWords >> index & 1u) != 0;
            const auto held = std::find_if(shape->words.begin(), shape->words.end(), [&](const RelocatableUserWord& word) { return word.reg == reg; });
            if (!pointer && !free) {
                if (held != shape->words.end()) ban(reg);
                continue;
            }
            if (held != shape->words.end()) {
                if (held->free != free) ban(reg);
                continue;
            }
            RelocatableUserWord word{reg, free, {}};
            if (pointer) {
                const auto base = AlignedWord(program.userData[index]);
                for (std::size_t run = 0; run < variant.runs.size() && run < variant.runBases.size(); ++run) {
                    if (variant.runBases[run] != static_cast<std::int8_t>(index)) continue;
                    word.reads.emplace_back(static_cast<std::int64_t>(variant.runs[run].first - base), static_cast<std::uint32_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t)));
                }
            }
            shape->words.push_back(std::move(word));
            changed = true;
        }
    }
    std::shared_ptr<const DrawRelocation> result = current;
    if (changed) {
        std::sort(shape->words.begin(), shape->words.end(), [](const RelocatableUserWord& a, const RelocatableUserWord& b) { return a.reg < b.reg; });
        if (drawRelocations.size() >= MaxRelocations && known == drawRelocations.end()) {
            drawRelocations.clear();
            drawRelocationTrials.clear();
            drawRelocationLast.clear();
        }
        result = shape;
        drawRelocations[identity] = std::move(shape);
        ++drawRelocationCounters.learned;
    }
    if (drawRelocationLast.size() >= MaxRelocations && !drawRelocationLast.contains(identity)) drawRelocationLast.clear();
    drawRelocationLast[identity] = inserted->second;
    if (alias != 0) {
        if (drawAliases.size() >= MaxAliases) drawRelocationCounters.evicted += dropEveryOther(drawAliases);
        drawAliases[alias] = inserted->second;
    }
    return result;
}

void Driver::relocateDrawEntry(const QueueState& queue, std::uint64_t alias, std::shared_ptr<DrawEntry>& entry, std::shared_ptr<const DrawDecode>& decode, std::vector<DrawUserPatch>& patches) {
    auto& counters = drawRelocationCounters;
    ++counters.lookups;
    const auto found = drawAliases.find(alias);
    if (found == drawAliases.end() || found->second->decode == nullptr) return;
    const auto donor = found->second;
    const auto& programs = donor->decode->programs;
    if (donor->stages.size() != programs.size()) return;
    struct Move {
        std::size_t program;
        std::uint32_t index, from, to;
    };
    std::vector<Move> moves;
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size(); ++index) {
            const auto current = readUserData(queue.shader, program.userDataBase + index - program.hiddenUserWords);
            if (current != program.userData[index]) moves.push_back({i, index, program.userData[index], current});
        }
    }
    if (moves.empty()) {
        ++counters.same;
        patches.clear();
        entry = donor;
        decode = donor->decode;
        return;
    }
    ++counters.donors;
    auto relocated = std::make_shared<DrawEntry>();
    relocated->decode = donor->decode;
    relocated->donor = donor;
    relocated->stages.resize(donor->stages.size());
    for (std::size_t i = 0; i < donor->stages.size(); ++i) {
        if (std::none_of(moves.begin(), moves.end(), [&](const Move& move) { return move.program == i; })) {
            relocated->stages[i] = donor->stages[i];
            continue;
        }
        for (const auto& variant : donor->stages[i]) {
            if (variant == nullptr || variant->compiled == nullptr) continue;
            const bool allowed = std::all_of(moves.begin(), moves.end(), [&](const Move& move) {
                if (move.program != i) return true;
                if (move.index >= 64) return false;
                if ((variant->freeWords >> move.index & 1u) != 0) return true;
                return (variant->relocatableWords >> move.index & 1u) != 0 && move.from != 0 && move.to != 0 && ((move.from ^ move.to) & 3u) == 0;
            });
            if (!allowed) {
                ++counters.refused;
                continue;
            }
            auto copy = std::make_shared<DispatchVariant>();
            copy->runs = variant->runs;
            copy->runBases = variant->runBases;
            copy->relocatableWords = variant->relocatableWords;
            copy->freeWords = variant->freeWords;
            copy->words = variant->words;
            copy->forgetSerial = variant->forgetSerial;
            copy->compiled = variant->compiled;
            copy->shader = variant->shader;
            copy->dataPositions = variant->dataPositions;
            copy->dataSlots = variant->dataSlots;
            copy->flatBinding = variant->flatBinding;
            copy->pushOffset = variant->pushOffset;
            copy->vertexInfo = variant->vertexInfo;
            copy->origin = variant;
            for (const auto& move : moves) {
                if (move.program != i) continue;
                if ((variant->relocatableWords >> move.index & 1u) != 0) {
                    const auto delta = AlignedWord(move.to) - AlignedWord(move.from);
                    for (std::size_t run = 0; run < copy->runs.size() && run < copy->runBases.size(); ++run) {
                        if (copy->runBases[run] != static_cast<std::int8_t>(move.index)) continue;
                        copy->runs[run].first += delta;
                        copy->runs[run].second += delta;
                    }
                }
                const auto& forwarded = variant->compiled->shaderDataUserWords;
                for (std::uint32_t slot = 0; slot < forwarded.size(); ++slot) {
                    if (forwarded[slot] == move.index) copy->shaderDataPatches.emplace_back(slot, move.to);
                }
            }
            relocated->stages[i].push_back(std::move(copy));
        }
    }
    patches.clear();
    for (const auto& move : moves) patches.push_back({move.program, move.index, move.to});
    entry = std::move(relocated);
    decode = donor->decode;
}

void Driver::applyShaderDataPatches(DispatchVariant& variant) {
    if (variant.shaderDataPatches.empty()) return;
    auto rebased = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
    for (const auto& [slot, value] : variant.shaderDataPatches) {
        const auto at = static_cast<std::size_t>(slot) * sizeof(std::uint32_t);
        if (at + sizeof(std::uint32_t) <= rebased->pushConstants.size()) std::memcpy(rebased->pushConstants.data() + at, &value, sizeof(value));
        for (auto& binding : rebased->bindings) {
            if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData && slot < binding.guestDescriptor.size()) binding.guestDescriptor[slot] = value;
        }
    }
    variant.compiled = std::move(rebased);
    variant.shaderDataPatches.clear();
}

void Driver::explainAliasMiss(const QueueState& queue, std::uint64_t identity) {
    auto& counters = drawRelocationCounters;
    const auto last = drawRelocationLast.find(identity);
    if (last == drawRelocationLast.end() || last->second->decode == nullptr || last->second->stages.size() != last->second->decode->programs.size()) {
        ++counters.missNoLast;
        return;
    }
    const auto& entry = *last->second;
    bool words = false;
    for (std::size_t i = 0; i < entry.decode->programs.size(); ++i) {
        const auto& program = entry.decode->programs[i];
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size(); ++index) {
            if (readUserData(queue.shader, program.userDataBase + index - program.hiddenUserWords) == program.userData[index]) continue;
            if (index >= 64 || entry.stages[i].empty()) {
                ++counters.wordBeyond;
                words = true;
                continue;
            }
            const auto& variant = *entry.stages[i].front();
            const auto bit = std::uint64_t{1} << index;
            if (((variant.freeWords | variant.relocatableWords) & bit) != 0) continue;
            words = true;
            if (!variant.relocationEligible) ++counters.wordIneligible;
            else if ((variant.valueWords & bit) != 0) ++counters.wordValue;
            else if ((variant.baseWords & bit) != 0) ++counters.wordDroppedBase;
            else ++counters.wordOther;
        }
    }
    if (words) ++counters.missWords;
    else ++counters.missContent;
}

void Driver::reportDrawRelocation() {
    auto& counters = drawRelocationCounters;
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    AgcDriver::ProfilePrint_nid_no_patch("[draw-relocate] (10 s) %llu alias lookups (%llu more not built: contents unmapped; %llu draws past the governor's pause): %llu donors with moved user words, %llu with the same words, %llu variants refused, %llu hits through a relocated entry; misses against the set's last entry: %llu only pointer/free words moved (contents differ), %llu a moved word not relocatable (words: %llu ineligible stage, %llu used as a value, %llu dereferenced but dropped, %llu other, %llu past 64 or uncached), %llu no last entry; %llu shader sets (re)learned, %zu held, %zu aliases held (%llu dropped at the limit)\n", count(counters.lookups), count(counters.unreadable), count(counters.bypassed), count(counters.donors), count(counters.same), count(counters.refused), count(counters.hits), count(counters.missContent), count(counters.missWords), count(counters.wordIneligible), count(counters.wordValue), count(counters.wordDroppedBase), count(counters.wordOther), count(counters.wordBeyond), count(counters.missNoLast), count(counters.learned), drawRelocations.size(), drawAliases.size(), count(counters.evicted));
    counters = {};
}

}
