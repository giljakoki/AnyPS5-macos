#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace {

using DepthFastClearHarness::Compile;
using DepthFastClearHarness::Covered;
using DepthFastClearHarness::Draw;
using DepthFastClearHarness::Height;
using DepthFastClearHarness::HtileBytes;
using DepthFastClearHarness::Require;
using DepthFastClearHarness::Surface;
using DepthFastClearHarness::Width;

alignas(4096) std::array<float, Width * Height * 2> Depth{};
alignas(256) std::array<std::uint32_t, 1024> Htile{};

// A tiled color target in guest memory: its resident image makes the draws recorded ones, which
// build draw recipes and can be recorded from them.
alignas(4096) std::array<std::byte, 2 * 65536> TiledStorage{};
std::byte* const Tiled = reinterpret_cast<std::byte*>((reinterpret_cast<std::uintptr_t>(TiledStorage.data()) + 65535u) & ~std::uintptr_t{65535u});

// The harness's draw into the tiled target; with `replay` it is recorded from that recipe
// (`replayed`: whether it was). Returns the covered pixels (R = 255), read back from guest memory.
std::uint32_t DrawTiled(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& compiled, const Surface& surface, bool depthWrite, std::shared_ptr<const AgcDriver::DrawRecipe>* recipeOut, const std::shared_ptr<const AgcDriver::DrawRecipe>* replay, bool* replayed) {
    using AgcDriver::Graphics::ColorTargetLayout;
    using AgcDriver::Graphics::ColorTileMode;
    using AgcDriver::Graphics::StorageTexture;
    const ColorTargetLayout layout(Width, Height, ColorTileMode::RenderTarget, 4);
    const auto address = reinterpret_cast<std::uintptr_t>(Tiled);
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        StorageTexture::FlushPending(address, layout.Bytes(), nullptr, "test");
        AgcDriver::GuestMemory::Write(address, std::vector<std::byte>(layout.Bytes()));
    }
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderRecompiler::ShaderStage::Vertex, &compiled.vertex, 0},
        {ShaderRecompiler::ShaderStage::Fragment, &compiled.pixel, compiled.vertexPush}
    }};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
    state.color = {address, {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, layout.Bytes(), 0xe4u};
    state.color.tileMode = ColorTileMode::RenderTarget;
    state.color.elementBytes = 4;
    state.color.surfaceAddress = address;
    state.color.surfaceExtent = state.color.extent;
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.depth = AgcDriver::Graphics::DepthTarget{surface.depth, surface.stencil, {Width, Height}, surface.format, 1.0f, 0, surface.htile, surface.htileStencil};
    state.depthTest = true;
    state.depthWrite = depthWrite;
    state.depthCompare = VK_COMPARE_OP_LESS;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0.0f, 1.0f};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(DepthFastClearHarness::Triangles().size()), 0, 1, 0, false};
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        if (replay != nullptr) *replayed = device.DrawFromRecipe(state, draw, shaders, {}, *replay) == AgcDriver::RecipeOutcome::Recorded;
        else device.Draw(state, draw, shaders, {}, recipeOut);
    }
    device.WaitIdle();
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    StorageTexture::FlushPending(address, layout.Bytes(), nullptr, "test");
    device.WaitIdle();
    std::uint32_t covered = 0;
    for (std::uint32_t y = 0; y < Height; ++y) {
        for (std::uint32_t x = 0; x < Width; ++x) {
            if (std::to_integer<std::uint8_t>(Tiled[layout.Offset(x, y)]) == 255u) ++covered;
        }
    }
    return covered;
}

// A recorded depth draw builds a recipe (unless APS5_NO_DEPTH_RECIPE=1), and a draw recorded from
// it renders with the depth attachment the ordinary path looks up: LESS at 0.5 fails over the 0.5
// the recipe's own draw wrote, and passes again once a fast clear, applied by the recipe draw's
// depth lookup, reset the surface to DB_DEPTH_CLEAR (1.0).
void DepthRecipeDraws(AgcDriver::VulkanDevice& device) {
    // Everything the draw reads lives in guest memory, so its resources are a reusable template
    // (the condition of a recipe): the vertices, the depth surface and its HTILE.
    alignas(65536) static std::array<std::byte, 65536> vertexBlock{};
    alignas(65536) static std::array<float, 65536 / 4> depth{};
    alignas(65536) static std::array<std::uint32_t, 65536 / 4> htile{};
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(TiledStorage.data(), TiledStorage.size(), true, true, true);
        mutation.Add(vertexBlock.data(), vertexBlock.size(), true, true, true);
        mutation.Add(depth.data(), sizeof(depth), true, true, true);
        mutation.Add(htile.data(), sizeof(htile), true, true, true);
    }
    const auto& triangles = DepthFastClearHarness::Triangles();
    std::memcpy(vertexBlock.data(), triangles.data(), triangles.size() * sizeof(triangles[0]));
    htile.fill(0xffffffffu);
    const auto shaders = Compile(device, vertexBlock.data());
    const Surface surface{reinterpret_cast<std::uintptr_t>(depth.data()), 0, reinterpret_cast<std::uintptr_t>(htile.data()), false, VK_FORMAT_D32_SFLOAT};
    std::shared_ptr<const AgcDriver::DrawRecipe> recipe;
    Require(DrawTiled(device, shaders, surface, true, &recipe, nullptr, nullptr) == Covered, "the recorded depth draw did not pass over a new (cleared) depth surface");
    if (!AgcDriver::Graphics::DepthRecipes() || !AgcDriver::Graphics::DrawRecipes()) {
        Require(recipe == nullptr, "a depth draw built a recipe with depth recipes off");
        return;
    }
    Require(recipe != nullptr, "a recorded depth draw built no draw recipe");
    bool replayed = false;
    Require(DrawTiled(device, shaders, surface, true, nullptr, &recipe, &replayed) == 0u && replayed, "a draw recorded from the depth recipe passed LESS against the 0.5 written before it (or the recipe missed)");
    AgcDriver::Graphics::NoteDepthMetadataFill(surface.htile, HtileBytes, 0u);
    replayed = false;
    Require(DrawTiled(device, shaders, surface, true, nullptr, &recipe, &replayed) == Covered && replayed, "a draw recorded from the depth recipe after a fast clear did not see the cleared depth (or the recipe missed)");
    std::puts("depth recipe draws passed");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Htile.fill(0xffffffffu);
        const auto shaders = Compile(*device);
        const Surface surface{reinterpret_cast<std::uintptr_t>(Depth.data()), 0, reinterpret_cast<std::uintptr_t>(Htile.data()), false, VK_FORMAT_D32_SFLOAT};
        const auto htile = surface.htile;
        Require(Draw(*device, shaders, surface, {.depthWrite = true}) == Covered, "a new depth surface was not cleared to DB_DEPTH_CLEAR (1.0)");
        Require(Draw(*device, shaders, surface, {}) == 0u, "a draw at depth 0.5 passed LESS against the 0.5 written before it");
        AgcDriver::Graphics::NoteDepthMetadataFill(htile, HtileBytes - 4u, 0u);
        Require(Draw(*device, shaders, surface, {}) == 0u, "a fill shorter than the HTILE cleared the depth");
        AgcDriver::Graphics::NoteDepthMetadataFill(htile, HtileBytes, 0xffffffffu);
        Require(Draw(*device, shaders, surface, {}) == 0u, "an expanded HTILE fill (ZMask 0xf) cleared the depth");
        AgcDriver::Graphics::NoteDepthMetadataFill(htile, HtileBytes, 0u);
        Require(Draw(*device, shaders, surface, {.depthWrite = true}) == Covered, "a fill of ZMask 0 over the whole HTILE did not clear the depth to DB_DEPTH_CLEAR (1.0)");
        Require(Draw(*device, shaders, surface, {}) == 0u, "the depth written after a fast clear was lost");
        AgcDriver::Graphics::NoteDepthMetadataFill(htile, HtileBytes, 0u);
        Require(Draw(*device, shaders, surface, {}) == Covered, "a second whole-HTILE fill of ZMask 0 did not clear the depth again");
        DepthRecipeDraws(*device);
        std::puts("depth fast clear tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
