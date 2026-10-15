#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include <algorithm>
#include <cstdlib>

namespace AgcDriver::DriverDetail {

bool DrawRecipeRecord::Matches(const std::vector<std::shared_ptr<DispatchVariant>>& variants) const {
    if (stages.size() != variants.size()) return false;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        if (stages[i].owner_before(variants[i]) || variants[i].owner_before(stages[i])) return false;
    }
    return true;
}

bool DrawRecipeRecord::Expired() const {
    return std::any_of(stages.begin(), stages.end(), [](const std::weak_ptr<const DispatchVariant>& stage) { return stage.expired(); });
}

bool Driver::drawEntries() {
    static const bool entries = std::getenv("APS5_NO_DRAW_SRT_ENTRIES") == nullptr && !stampValidate();
    return entries;
}

bool Driver::verifyDrawEntries() {
    static const bool verify = std::getenv("APS5_VERIFY_DRAW_ENTRIES") != nullptr;
    return verify;
}

bool Driver::registerKeyEnabled() {
    static const bool registerKey = std::getenv("APS5_NO_DRAW_KEY") == nullptr;
    return registerKey;
}

bool Driver::verifyDrawRecipe() {
    static const bool verify = std::getenv("APS5_VERIFY_DRAW_RECIPE") != nullptr;
    return verify;
}

std::size_t Driver::drawCacheEntries() {
    static const std::size_t entries = [] {
        const char* text = std::getenv("APS5_DRAW_CACHE_ENTRIES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        return parsed != 0 ? static_cast<std::size_t>(parsed) : std::size_t{4096};
    }();
    return entries;
}

void Driver::accountDrawVariant(const DispatchVariant& variant, bool added) {
    if (added) {
        ++drawCacheVariants;
        drawCacheVariantBytes += variantBytes(variant);
    } else {
        --drawCacheVariants;
        drawCacheVariantBytes -= variantBytes(variant);
    }
}

void Driver::insertDrawEntry(std::uint64_t key, std::vector<std::shared_ptr<DispatchVariant>>& fresh, std::shared_ptr<const DrawDecode> decode) {
    std::lock_guard cacheLock(drawCacheMutex);
    auto& counters = drawEntryCounters;
    ++counters.inserts;
    const auto found = drawCache.find(key);
    auto replacement = std::make_shared<DrawEntry>();
    replacement->stages.resize(fresh.size());
    replacement->decode = std::move(decode);
    if (found != drawCache.end()) {
        if (found->second->stages.size() == fresh.size()) replacement->stages = found->second->stages;
        if (found->second->decode != nullptr) replacement->decode = found->second->decode;
        replacement->recipes.store(found->second->recipes.load());
    }
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        if (fresh[i] == nullptr) continue;
        auto& variants = replacement->stages[i];
        const auto present = std::find_if(variants.begin(), variants.end(), [&](const std::shared_ptr<DispatchVariant>& kept) { return kept->pushOffset == fresh[i]->pushOffset && kept->runs == fresh[i]->runs && kept->words == fresh[i]->words; });
        if (present != variants.end()) {
            ++counters.present;
            fresh[i] = *present;
            continue;
        }
        accountDrawVariant(*fresh[i], true);
        variants.insert(variants.begin(), fresh[i]);
        ++counters.variantsInserted;
        while (variants.size() > dispatchVariants()) {
            accountDrawVariant(*variants.back(), false);
            variants.pop_back();
            ++counters.variantsEvicted;
        }
    }
    replacement->touched = drawCacheHits;
    if (found == drawCache.end()) {
        drawOrder.push_front(key);
        replacement->order = drawOrder.begin();
        drawCache.emplace(key, std::move(replacement));
    } else {
        replacement->order = found->second->order;
        drawOrder.splice(drawOrder.begin(), drawOrder, replacement->order);
        found->second = std::move(replacement);
    }
    while (drawCache.size() > drawCacheEntries()) {
        const auto last = drawCache.find(drawOrder.back());
        for (const auto& variants : last->second->stages) {
            for (const auto& variant : variants) accountDrawVariant(*variant, false);
        }
        drawOrder.erase(last->second->order);
        drawCache.erase(last);
        ++drawCacheEvictions;
    }
}

void Driver::eraseDrawEntry(std::uint64_t key, const std::shared_ptr<DrawEntry>& expected) {
    const auto found = drawCache.find(key);
    if (found == drawCache.end() || found->second != expected) return;
    for (const auto& variants : found->second->stages) {
        for (const auto& variant : variants) accountDrawVariant(*variant, false);
    }
    drawOrder.erase(found->second->order);
    drawCache.erase(found);
}

bool DecodeProgramsCurrent(const DrawDecode& decode, const ShaderRegistry&) {
    return std::all_of(decode.programs.begin(), decode.programs.end(), [&](const DrawProgram& program) {
        return !program.snapshot->header.empty() || ReadRawShader(program.snapshot->codeAddress + program.codeOffset * sizeof(std::uint32_t)) == program.snapshot;
    });
}

// Relocated recipes (APS5_NO_RELOCATED_RECIPE=1 turns them off): a draw-cache hit through a
// relocated entry (moved user-data pointers) finds and attaches recipes on the donor entry, its
// stages matched by the cached variants they were copied from; such a hit is rebased (its push
// constants, data words and moved buffers re-derived, the template's content key compared).
bool Driver::relocatedRecipes() {
    static const bool disabled = std::getenv("APS5_NO_RELOCATED_RECIPE") != nullptr;
    return !disabled;
}

// While the draw-cache governor pauses entries, the draws of a shader set with a recipe keep them
// (APS5_NO_RECIPE_GOVERNOR_BYPASS=1: they are paused with the rest).
bool Driver::recipeGovernorBypass() {
    static const bool disabled = std::getenv("APS5_NO_RECIPE_GOVERNOR_BYPASS") != nullptr;
    return !disabled;
}

std::shared_ptr<const DrawRecipe> Driver::findDrawRecipe(std::uint64_t key, const std::shared_ptr<DrawEntry>& home, const std::vector<std::shared_ptr<DispatchVariant>>& stages, bool& rebased) {
    std::shared_ptr<const std::vector<DrawRecipeRecord>> records;
    if (home != nullptr) {
        records = home->recipes.load();
    } else {
        std::lock_guard cacheLock(drawCacheMutex);
        const auto found = drawCache.find(key);
        if (found == drawCache.end()) return nullptr;
        records = found->second->recipes.load();
    }
    if (records == nullptr) return nullptr;
    for (const auto& record : *records) {
        if (!record.Matches(stages)) continue;
        rebased = rebased || record.rebased;
        return record.recipe;
    }
    return nullptr;
}

void Driver::attachDrawRecipe(std::uint64_t key, const std::shared_ptr<DrawEntry>& home, const std::vector<std::shared_ptr<DispatchVariant>>& stages, std::shared_ptr<const DrawRecipe> recipe, bool rebased, std::uint64_t identity) {
    std::shared_ptr<DrawEntry> entry = home;
    {
        std::lock_guard cacheLock(drawCacheMutex);
        if (entry == nullptr) {
            const auto found = drawCache.find(key);
            if (found == drawCache.end()) return;
            entry = found->second;
        }
        if (identity != 0 && recipeGovernorBypass()) {
            if (drawRecipeIdentities.size() >= 65536) drawRecipeIdentities.clear();
            drawRecipeIdentities.insert(identity);
        }
    }
    // Two committers may attach to one entry at once (a lost record is a later miss, never wrong).
    const auto old = entry->recipes.load();
    auto records = std::make_shared<std::vector<DrawRecipeRecord>>();
    records->push_back({std::vector<std::weak_ptr<const DispatchVariant>>(stages.begin(), stages.end()), std::move(recipe), rebased});
    if (old != nullptr) {
        for (const auto& record : *old) {
            if (record.Matches(stages) || record.Expired() || records->size() >= dispatchVariants()) continue;
            records->push_back(record);
        }
    }
    entry->recipes.store(std::move(records));
    VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Attach, VulkanDevice::RecipeKind::Draw);
}

}
