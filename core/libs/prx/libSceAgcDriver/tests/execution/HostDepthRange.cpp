#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DrawMiss;
using DepthFastClearHarness::Compile;
using DepthFastClearHarness::Covered;
using DepthFastClearHarness::Draw;
using DepthFastClearHarness::DrawOptions;
using DepthFastClearHarness::Require;
using DepthFastClearHarness::Shaders;
using DepthFastClearHarness::Surface;

struct DepthPlane {
    alignas(4096) std::array<float, DepthFastClearHarness::Width * DepthFastClearHarness::Height * 2> depth{};
    alignas(256) std::array<std::uint32_t, 1024> htile{};

    Surface Target() const {
        return {reinterpret_cast<std::uintptr_t>(depth.data()), 0, reinterpret_cast<std::uintptr_t>(htile.data()), false, VK_FORMAT_D32_SFLOAT};
    }
};

DepthPlane Stored{};
DepthPlane NewlyCleared{};
DepthPlane FastCleared{};

struct Outcome {
    std::optional<std::uint32_t> covered;
    std::string failure;
};

Outcome Attempt(AgcDriver::VulkanDevice& device, const Shaders& shaders, const Surface& surface, const DrawOptions& options) {
    try {
        return {Draw(device, shaders, surface, options), {}};
    } catch (const std::runtime_error& error) {
        return {std::nullopt, error.what()};
    }
}

DrawOptions Bounded(float low, float high) {
    return {.depthTest = false, .depthBounds = true, .minDepthBounds = low, .maxDepthBounds = high};
}

void RequireClear(const Outcome& outcome, bool restricted, const std::string& what) {
    if (restricted) {
        Require(!outcome.covered && outcome.failure.find("to DB_DEPTH_CLEAR 2 (0x40000000) outside [0, 1] requires VK_EXT_depth_range_unrestricted, which the device lacks") != std::string::npos, "host depth range: " + what + " did not stop without VK_EXT_depth_range_unrestricted: " + outcome.failure);
        return;
    }
    Require(outcome.covered == Covered, "host depth range: a draw at depth 0.5 failed LESS after " + what + ": " + outcome.failure);
}

void DrawThroughTheDevice(AgcDriver::VulkanDevice& device, bool restricted) {
    for (auto* plane : {&Stored, &NewlyCleared, &FastCleared}) plane->htile.fill(0xffffffffu);
    const auto shaders = Compile(device);
    const auto surface = Stored.Target();
    Require(Draw(device, shaders, surface, {.depthWrite = true}) == Covered, "host depth range: the depth 0.5 draw did not cover the target");
    Require(Draw(device, shaders, surface, {.depthTest = false, .minDepth = -1.0f, .maxDepth = 1.0f}) == Covered, "host depth range: a [-1, 1] depth range without a depth test did not draw");
    Require(Draw(device, shaders, surface, {.depthTest = true, .depthCompare = VK_COMPARE_OP_ALWAYS, .minDepth = 1.0f, .maxDepth = -1.0f}) == Covered, "host depth range: a reversed [1, -1] depth range comparing ALWAYS did not draw");
    const auto first = Attempt(device, shaders, surface, Bounded(-0.5f, 2.0f));
    if (!first.covered && first.failure.find("device does not support the depth bounds test") != std::string::npos) {
        Require(!restricted || first.failure.find("device depthRangeUnrestricted=0 depthClamp=1 clipDistance=1 depthBounds=0") != std::string::npos, "host depth range: the depth bounds failure lost the diagnostic: " + first.failure);
        std::puts("depth bounds are unsupported, their checks are skipped");
    } else {
        Require(first.covered == Covered, "host depth range: depth bounds [-0.5, 2] rejected the stored depth 0.5: " + first.failure);
        Require(Draw(device, shaders, surface, Bounded(0.0f, 0.5f)) == Covered, "host depth range: depth bounds [0, 0.5] rejected the stored depth 0.5");
        Require(Draw(device, shaders, surface, Bounded(0.75f, 2.0f)) == 0u, "host depth range: depth bounds [0.75, 2] accepted the stored depth 0.5");
        Require(Draw(device, shaders, surface, {.depthTest = false, .minDepth = -1.0f, .maxDepth = 1.0f, .depthBounds = true, .minDepthBounds = 0.0f, .maxDepthBounds = 0.5f}) == Covered, "host depth range: a [-1, 1] depth range testing bounds [0, 0.5] rejected the stored depth 0.5");
        Require(Draw(device, shaders, surface, {.depthTest = false, .minDepth = -1.0f, .maxDepth = 1.0f, .depthBounds = true, .minDepthBounds = 0.75f, .maxDepthBounds = 2.0f}) == 0u, "host depth range: a [-1, 1] depth range testing bounds [0.75, 2] accepted the stored depth 0.5");
        for (const auto& [low, high] : {std::array{1.5f, 2.0f}, std::array{-1.0f, -0.5f}, std::array{0.75f, 0.25f}}) {
            const auto never = Attempt(device, shaders, surface, Bounded(low, high));
            if (never.covered) Require(*never.covered == 0u, "host depth range: depth bounds that never pass accepted the stored depth 0.5");
            else Require(never.failure.find("depth bounds that never pass") != std::string::npos, "host depth range: " + never.failure);
        }
    }
    const auto observed = Attempt(device, shaders, surface, {.minDepth = -1.0f, .maxDepth = 1.0f});
    if (observed.covered) {
        Require(!restricted, "host depth range: a depth test over [-1, 1] drew under APS5_NO_DEPTH_RANGE_UNRESTRICTED");
        Require(*observed.covered == Covered, "host depth range: a depth test over [-1, 1] at depth 0 failed against the stored 0.5");
    } else {
        Require(observed.failure.find("viewport depth outside [0, 1] requires VK_EXT_depth_range_unrestricted, which the device lacks, and the draw observes depth") != std::string::npos, "host depth range: " + observed.failure);
        Require(observed.failure.find("depth target=D32_SFLOAT test=1 write=0 compare=LESS bounds=0") != std::string::npos && observed.failure.find("device depthRangeUnrestricted=0") != std::string::npos, "host depth range: the diagnostic lost the draw's depth state: " + observed.failure);
        std::printf("observed [-1, 1] depth range rejected: %s\n", observed.failure.c_str());
    }
    const auto fastCleared = FastCleared.Target();
    Require(Draw(device, shaders, fastCleared, {.depthWrite = true}) == Covered, "host depth range: the depth 0.5 draw did not cover the second target");
    Require(Draw(device, shaders, fastCleared, {}) == 0u, "host depth range: a draw at depth 0.5 passed LESS against the stored 0.5");
    AgcDriver::Graphics::NoteHtileDepthClear(fastCleared.htile);
    RequireClear(Attempt(device, shaders, fastCleared, {.clearDepth = 2.0f}), restricted, "an HTILE fast clear to 2");
    RequireClear(Attempt(device, shaders, NewlyCleared.Target(), {.clearDepth = 2.0f}), restricted, "a new depth surface cleared to 2");
}

alignas(256) constexpr std::array<std::uint32_t, 8> VertexCode{0xe0382000u, 0x80020005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> ConstantPixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> DepthPixelCode{0x7e000302u, 0x7e020280u, 0x7e0602f2u, 0xf800180fu, 0x03010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::array<float, 4>, 3> NearTriangle{{{-1.0f, -1.0f, -0.5f, 1.0f}, {3.0f, -1.0f, -0.5f, 1.0f}, {-1.0f, 3.0f, -0.5f, 1.0f}}};
alignas(256) constexpr std::array<std::array<float, 4>, 3> MiddleTriangle{{{-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}}};
alignas(256) std::array<std::byte, 1024> ColorMemory{};
constexpr std::uint32_t ColorPixels = 64 * 4;
constexpr std::uint32_t PositionZInputs = 0x402;

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header ConstantPixelHeader{};
alignas(8) Header DepthPixelHeader{};

void Register(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

std::vector<std::uint32_t> GlDepthDraws(const std::array<std::array<float, 4>, 3>& triangle, const std::uint32_t* pixelCode, std::uint32_t pixelInputs, std::uint32_t draws) {
    const auto color = reinterpret_cast<std::uintptr_t>(ColorMemory.data());
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(pixelCode);
    const auto vertices = reinterpret_cast<std::uintptr_t>(triangle.data());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b3, pixelInputs}, {0x1b4, pixelInputs}, {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x003, 0x00010000}, {0x000, 0}, {0x00b, 0},
    };
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 4u << 1u},
        {0x8c, static_cast<std::uint32_t>(vertices)}, {0x8d, static_cast<std::uint32_t>((vertices >> 32u) & 0xffffu) | (16u << 16u)}, {0x8e, 3}, {0x8f, 0x01016facu},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 1u << 1u}, {0x00c, std::bit_cast<std::uint32_t>(1.0f)},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    for (std::uint32_t draw = 0; draw < draws; ++draw) Append(commands, 0x2d, {3, 2});
    return commands;
}

void Submit(std::vector<std::uint32_t> commands) {
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "host depth range: the draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

std::uint32_t PixelsWith(std::uint8_t red) {
    std::uint32_t count = 0;
    for (std::uint32_t pixel = 0; pixel < ColorPixels; ++pixel) {
        if (std::to_integer<std::uint8_t>(ColorMemory[pixel * 4u]) == red && std::to_integer<std::uint8_t>(ColorMemory[pixel * 4u + 3u]) == 255u) ++count;
    }
    return count;
}

void DrawThroughTheDriver(bool restricted) {
    Register(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Register(ConstantPixelHeader, ConstantPixelCode.data(), sizeof(ConstantPixelCode), 1);
    Register(DepthPixelHeader, DepthPixelCode.data(), sizeof(DepthPixelCode), 1);
    ColorMemory.fill(std::byte{0});
    Submit(GlDepthDraws(NearTriangle, ConstantPixelCode.data(), 0, 2));
    const auto counters = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    const auto layout = counters.misses[static_cast<std::size_t>(DrawMiss::Layout)];
    Require(counters.lookups == 2 && counters.absent == 1 && layout == 0 && counters.hits == 1, "host depth range: two equal [-1, 1] draws without a depth target made " + std::to_string(counters.lookups) + " draw-cache lookups, " + std::to_string(counters.absent) + " without an entry, " + std::to_string(layout) + " layout misses and " + std::to_string(counters.hits) + " hits, expected 2, 1, 0 and 1");
    if (std::getenv("APS5_VERIFY_DRAW_RECIPE") != nullptr) Require(counters.verifyDecodes == 1 && counters.verifyDecodeMismatches == 0 && counters.facadeMismatches == 0, "host depth range: the repeated draw's decode compared " + std::to_string(counters.verifyDecodes) + " times with " + std::to_string(counters.verifyDecodeMismatches) + " differences and " + std::to_string(counters.facadeMismatches) + " register reads outside the draw key, expected 1, 0 and 0");
    Require(PixelsWith(255u) == ColorPixels, "host depth range: the [-1, 1] draws of a triangle at clip z -0.5 without a depth target covered " + std::to_string(PixelsWith(255u)) + " of " + std::to_string(ColorPixels) + " pixels");
    ColorMemory.fill(std::byte{0});
    std::string failure;
    try {
        Submit(GlDepthDraws(MiddleTriangle, DepthPixelCode.data(), PositionZInputs, 1));
    } catch (const std::exception& error) {
        failure = error.what();
    }
    const auto middle = PixelsWith(127u) + PixelsWith(128u);
    try {
        AgcDriverShutdown_nid_postfix();
    } catch (const std::exception& error) {
        if (failure.empty()) failure = error.what();
    }
    if (!restricted) {
        Require(failure.empty(), "host depth range: a [-1, 1] draw whose pixel shader reads POS_Z failed with VK_EXT_depth_range_unrestricted: " + failure);
        Require(middle == ColorPixels, "host depth range: a [-1, 1] draw at clip z 0.5 exported POS_Z 0.5 on " + std::to_string(middle) + " of " + std::to_string(ColorPixels) + " pixels");
        return;
    }
    const std::string expected = "viewport depth outside [0, 1] requires VK_EXT_depth_range_unrestricted, which the device lacks, and the draw observes depth (remapping it is not implemented): "
        "PA_CL_CLIP_CNTL[0x204]=0x00000000 PA_CL_VPORT_ZSCALE[0x113]=0x3f800000 (1) PA_CL_VPORT_ZOFFSET[0x114]=0x00000000 (0) PA_SC_VPORT_ZMIN_0[0x0b4]=0x00000000 (0) PA_SC_VPORT_ZMAX_0[0x0b5]=0x3f800000 (1) "
        "DB_RENDER_OVERRIDE[0x003]=0x00010000 DB_RENDER_CONTROL[0x000]=0x00000000 DB_DEPTH_CLEAR[0x00b]=0x00000000 (0); viewport depth [-1, 1] negativeOneToOne=1 depthClamp=0; depth target=none";
    Require(failure.find(expected) != std::string::npos && failure.find("; posZ=1 rectList=0 mesh=0 tessellation=0; device depthRangeUnrestricted=0") != std::string::npos, "host depth range: a [-1, 1] draw whose pixel shader reads POS_Z failed with \"" + failure + "\", expected \"" + expected + "\" and posZ=1");
}

}

int main() {
    try {
        const bool restricted = std::getenv("APS5_NO_DEPTH_RANGE_UNRESTRICTED") != nullptr;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            DrawThroughTheDevice(*device, restricted);
        }
        DrawThroughTheDriver(restricted);
        std::puts(restricted ? "host depth range tests passed without VK_EXT_depth_range_unrestricted" : "host depth range tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
