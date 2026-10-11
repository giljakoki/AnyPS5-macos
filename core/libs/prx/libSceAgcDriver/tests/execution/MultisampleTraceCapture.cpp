#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "SceShaders.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::uint32_t Threads = 32;
alignas(256) constexpr std::array<std::uint32_t, 6> Code{0xf0001f30u, 0x00000400u, 0xbf8c3f70u, 0xe0700000u, 0x80020400u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> VertexCode{0x7e000208u, 0x7e020280u, 0x7e040280u, 0x7e0602f2u, 0xf80008cfu, 0x03020100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0xf0001f30u, 0x00000400u, 0xbf8c3f70u, 0xf800180fu, 0x07060504u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(256) std::array<std::uint8_t, 4096> Texels{};
alignas(256) std::array<std::uint32_t, 4> Output{};
alignas(256) std::array<std::byte, 1024> ColorMemory{};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};

std::array<std::uint32_t, 8> Descriptor() {
    const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Texels.data()));
    return {static_cast<std::uint32_t>(base >> 8u), static_cast<std::uint32_t>((base >> 40u) & 0xffu) | (56u << 20u) | (3u << 30u), 3u << 14u, 0xfacu | (2u << 16u) | (14u << 28u), 0u, 2u << 4u, 0u, 0u};
}

std::vector<std::uint32_t> Commands() {
    std::vector<std::uint32_t> words;
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{Threads, 1, 1};
    registers(0x207, shape);
    const auto address = reinterpret_cast<std::uintptr_t>(Code.data());
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)};
    registers(0x20c, program);
    std::array<std::uint32_t, 12> userData{};
    const auto descriptor = Descriptor();
    std::copy(descriptor.begin(), descriptor.end(), userData.begin());
    const auto output = reinterpret_cast<std::uintptr_t>(Output.data());
    userData[8] = static_cast<std::uint32_t>(output);
    userData[9] = static_cast<std::uint32_t>((output >> 32u) & 0xffffu);
    userData[10] = static_cast<std::uint32_t>(sizeof(Output));
    userData[11] = 0x31016facu;
    const std::array<std::uint32_t, 1> resource{static_cast<std::uint32_t>(userData.size()) << 1u};
    registers(0x213, resource);
    registers(0x240, userData);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
    return words;
}

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

std::vector<std::uint32_t> DrawCommands() {
    const auto color = reinterpret_cast<std::uintptr_t>(ColorMemory.data());
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
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
    };
    std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 1u << 1u}, {0x8c, 0},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 8u << 1u},
    };
    const auto descriptor = Descriptor();
    for (std::uint32_t index = 0; index < descriptor.size(); ++index) shader.emplace_back(0x00cu + index, descriptor[index]);
    std::vector<std::uint32_t> commands;
    const auto append = [&](std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
        commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
        commands.insert(commands.end(), payload);
    };
    append(0x79, {0x242, 4});
    for (const auto& [offset, value] : context) append(0x69, {offset, value});
    for (const auto& [offset, value] : shader) append(0x76, {offset, value});
    append(0x2d, {3, 2});
    return commands;
}

template <typename TAction>
void ExpectFailure(TAction action, const char* expected) {
    try {
        action();
    } catch (const std::exception& error) {
        Require(std::string(error.what()).find(expected) != std::string::npos, std::string("unexpected failure: ") + error.what());
        return;
    }
    throw std::runtime_error(std::string("expected a failure containing ") + expected);
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 1 || (argc == 2 && std::string(argv[1]) == "draw"), "usage: [draw]");
        const bool draw = argc == 2;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        Require(AgcDriver::Graphics::TraceMultisampling(), "the multisampling trace capture test needs APS5_TRACE_MSAA=1");
        std::vector<std::string> log;
        AgcDriver::Graphics::MultisampleTraceLog() = &log;
        const auto words = draw ? DrawCommands() : Commands();
        Packet packet{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
        if (draw) {
            Register(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
            Register(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
            Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        } else {
            AgcDriver::Submit(&packet, 0);
        }
        constexpr const char* Refusal = "multisampled image resources have no descriptor image shape";
        ExpectFailure([] { AgcDriverWaitIdle_nid_postfix(); }, Refusal);
        AgcDriver::Graphics::MultisampleTraceLog() = nullptr;
        const auto program = draw ? reinterpret_cast<std::uintptr_t>(PixelCode.data()) : reinterpret_cast<std::uintptr_t>(Code.data());
        const auto expected = AgcDriver::Graphics::MultisampleDescriptorLine(Descriptor(), program);
        Require(expected.has_value() && std::count(log.begin(), log.end(), *expected) == 1, std::string(draw ? "a draw" : "a dispatch") + " whose shader reads a 2D MSAA texture failed without tracing its descriptor and shader (" + std::to_string(log.size()) + " trace lines)");
        ExpectFailure([] { AgcDriverShutdown_nid_postfix(); }, Refusal);
        std::puts("multisampling trace capture tests passed");
        return 0;
    } catch (const std::exception& error) {
        AgcDriver::Graphics::MultisampleTraceLog() = nullptr;
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception&) {
        }
        return 1;
    }
}
