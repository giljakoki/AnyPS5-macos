#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

alignas(256) constexpr std::array<std::uint32_t, 8> VertexCode{0xe0382000u, 0x80020005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::array<float, 4>, 3> Triangle{{{-1.0f, -1.0f, -0.5f, 1.0f}, {3.0f, -1.0f, -0.5f, 1.0f}, {-1.0f, 3.0f, -0.5f, 1.0f}}};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 9> FillKernel{0xd7460004u, 0x04010c08u, 0x7e000204u, 0x7e020205u, 0x7e040206u, 0x7e060207u, 0xe01c2000u, 0x80000004u, 0xbf810000u};
alignas(256) std::array<std::byte, 1024> ColorMemory{};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};

void Registered(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

std::uint32_t Low(std::uint64_t value) { return static_cast<std::uint32_t>(value); }
std::uint32_t High(std::uint64_t value) { return static_cast<std::uint32_t>(value >> 32u); }

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

void AppendDraw(std::vector<std::uint32_t>& commands) {
    const auto color = reinterpret_cast<std::uintptr_t>(ColorMemory.data());
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    const auto vertices = reinterpret_cast<std::uintptr_t>(Triangle.data());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b3, 0}, {0x1b4, 0}, {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
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
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    Append(commands, 0x2d, {3, 2});
}

// The title's buffer fill kernel over the color target's bytes (the HLE fill path).
void AppendFill(std::vector<std::uint32_t>& commands, std::uint32_t pattern) {
    const auto kernel = reinterpret_cast<std::uintptr_t>(FillKernel.data());
    const auto base = reinterpret_cast<std::uintptr_t>(ColorMemory.data());
    const auto bytes = static_cast<std::uint32_t>(ColorMemory.size());
    for (const auto& [offset, value] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{
             {0x20c, Low(kernel >> 8u)}, {0x20d, High(kernel >> 8u) & 0xffu}, {0x207, 64}, {0x208, 1}, {0x209, 1}, {0x213, (8u << 1u) | 0x80u},
             {0x240, Low(base)}, {0x241, (High(base) & 0xffffu) | (16u << 16u)}, {0x242, bytes / 16u}, {0x243, 0x4bfacu},
             {0x244, pattern}, {0x245, pattern}, {0x246, pattern}, {0x247, pattern}}) {
        Append(commands, 0x76, {offset, value});
    }
    Append(commands, 0x15, {bytes / 1024u, 1, 1, 0x41});
}

void Submit(std::vector<std::uint32_t>& commands) {
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "pipelined order: the submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

std::size_t WordsEqualTo(std::uint32_t pattern) {
    std::size_t equal = 0;
    for (std::size_t at = 0; at < ColorMemory.size(); at += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, ColorMemory.data() + at, 4);
        if (word == pattern) ++equal;
    }
    return equal;
}

}

int main() {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
        Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
        constexpr std::uint32_t Before = 0x11223344u, After = 0x5a5a5a5au;
        const auto words = ColorMemory.size() / 4;

        std::vector<std::uint32_t> fillThenDraw;
        AppendFill(fillThenDraw, Before);
        AppendDraw(fillThenDraw);
        Submit(fillThenDraw);
        const auto drawnOver = words - WordsEqualTo(Before);
        if (drawnOver == 0) {
            std::puts("pipelined order: the draw stored nothing over the fill on this device; ordering not checked");
            AgcDriverShutdown_nid_postfix();
            return VulkanTestSkipped;
        }

        // Draws, then a fill of the same bytes in the same submission: with the pipeline on, the draws
        // are still queued when the fill packet is reached, and the fill must land after them.
        std::vector<std::uint32_t> drawsThenFill;
        AppendDraw(drawsThenFill);
        AppendDraw(drawsThenFill);
        AppendFill(drawsThenFill, After);
        Submit(drawsThenFill);
        const auto filled = WordsEqualTo(After);
        Require(filled == words, "pipelined order: a fill queued after draws to the same target did not land after them (" + std::to_string(filled) + " of " + std::to_string(words) + " words hold the fill)");
        AgcDriverShutdown_nid_postfix();
        std::puts("pipelined order tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
