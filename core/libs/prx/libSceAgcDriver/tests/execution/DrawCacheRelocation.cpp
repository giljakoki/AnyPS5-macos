#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libSceAgc/Shader/include/ShaderConstants.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0xf4000100u, 0xfa000000u, 0x7e000204u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> FreePixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(16) std::array<float, 4> Other{0.5f, 0.0f, 0.0f, 0.0f};
alignas(256) std::array<std::byte, 1024> ColorMemory{};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};
alignas(8) Header FreePixelHeader{};

struct FetchHeader {
    Shader shader{};
    ShaderUserData userData{};
    std::array<std::uint16_t, ShaderRegs::AGC_DIRECT_RESOURCE_TYPE_COUNT> direct{};
    ShaderSemantic semantic{};
};

alignas(256) constexpr std::array<std::uint32_t, 8> FetchVertexCode{0xe0382000u, 0x80020005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(8) FetchHeader FetchVertexHeader{};

void RegisterFetchVertex() {
    auto& header = FetchVertexHeader;
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(FetchHeader);
    header.shader.shader_size = static_cast<std::uint32_t>(sizeof(FetchVertexCode));
    header.shader.code = FetchVertexCode.data();
    header.shader.type = 2;
    header.shader.user_data = &header.userData;
    header.direct.fill(ShaderRegs::AGC_ILLEGAL_DIRECT_OFFSET);
    header.direct[static_cast<std::size_t>(ShaderRegs::AgcDirectResourceType::PtrVertexBufferTable)] = 4;
    header.direct[static_cast<std::size_t>(ShaderRegs::AgcDirectResourceType::PtrVertexAttribDescTable)] = 6;
    header.userData.direct_resource_offset = header.direct.data();
    header.userData.direct_resource_count = ShaderRegs::AGC_DIRECT_RESOURCE_TYPE_COUNT;
    header.semantic.semantic = 0;
    header.semantic.hardware_mapping = 8;
    header.semantic.size_in_elements = 4;
    header.shader.input_semantics = &header.semantic;
    header.shader.num_input_semantics = 1;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

const Shader* Registered(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
    return &header.shader;
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

std::vector<std::uint32_t> DrawWith(const std::uint32_t* pixelCode, std::uint32_t userWords, std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> words, const std::uint32_t* vertexCode = nullptr, std::uint32_t vertexUserWords = 4) {
    const auto color = reinterpret_cast<std::uintptr_t>(ColorMemory.data());
    const auto vertex = reinterpret_cast<std::uintptr_t>(vertexCode != nullptr ? vertexCode : VertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(pixelCode);
    const auto vertices = reinterpret_cast<std::uintptr_t>(Triangle.data());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b3, 0u}, {0x1b4, 0u}, {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
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
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, vertexUserWords << 1u},
        {0x8c, static_cast<std::uint32_t>(vertices)}, {0x8d, static_cast<std::uint32_t>((vertices >> 32u) & 0xffffu) | (16u << 16u)}, {0x8e, 3}, {0x8f, 0x01016facu},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, userWords << 1u},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    for (const auto& [offset, value] : words) Append(commands, 0x76, {offset, value});
    Append(commands, 0x2d, {3, 2});
    return commands;
}

std::vector<std::uint32_t> DrawFrom(const float* source) {
    const auto address = reinterpret_cast<std::uintptr_t>(source);
    return DrawWith(PixelCode.data(), 2, {{0x00c, static_cast<std::uint32_t>(address)}, {0x00d, static_cast<std::uint32_t>(address >> 32u)}});
}

AgcDriver::DriverDetail::DrawEntryCounters Submit(std::vector<std::uint32_t> commands) {
    ColorMemory.fill(std::byte{0});
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
    return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
}

std::uint32_t Red() {
    std::uint32_t red = 0;
    for (std::size_t pixel = 0; pixel < ColorMemory.size() / 4u; ++pixel) red = std::max(red, static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(ColorMemory[pixel * 4u])));
    return red;
}

void MovedDataHitsTheCache(const std::string& mode) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    static std::array<std::array<float, 4>, 8> same{};
    for (auto& entry : same) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    const auto start = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (std::size_t i = 0; i < same.size(); ++i) {
        counters = Submit(DrawFrom(same[i].data()));
        Require(Red() == 255u, mode + ": the draw through pointer " + std::to_string(i) + " drew red " + std::to_string(Red()) + ", expected 255");
    }
    const auto lookups = counters.lookups - start.lookups, hits = counters.hits - start.hits, inserts = counters.inserts - start.inserts;
    Require(lookups == same.size() && hits == same.size() - 1 && inserts == 1, mode + ": draws whose pointer moved to equal contents made " + std::to_string(hits) + " hits and " + std::to_string(inserts) + " inserts in " + std::to_string(lookups) + " lookups, expected " + std::to_string(same.size() - 1) + " and 1");
    const auto other = Submit(DrawFrom(Other.data()));
    Require(other.hits - start.hits == hits, mode + ": a draw whose moved pointer holds another value reused the cached draw");
    Require(Red() >= 126u && Red() <= 129u, mode + ": the draw through a pointer to 0.5 drew red " + std::to_string(Red()));
    const auto again = Submit(DrawFrom(same[0].data()));
    Require(again.hits - start.hits == hits + 1, mode + ": a draw at an address seen before did not reuse the cached draw");
    Require(Red() == 255u, mode + ": the draw at an address seen before drew red " + std::to_string(Red()));
}

void MovedFreeWordHitsTheCache(const std::string& mode) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(FreePixelHeader, FreePixelCode.data(), sizeof(FreePixelCode), 1);
    const auto start = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    const std::array<std::pair<float, std::uint32_t>, 4> values{{{1.0f, 255u}, {0.5f, 128u}, {0.25f, 64u}, {0.75f, 191u}}};
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (const auto& [value, red] : values) {
        counters = Submit(DrawWith(FreePixelCode.data(), 1, {{0x00c, std::bit_cast<std::uint32_t>(value)}}));
        Require(Red() + 1u >= red && Red() <= red + 1u, mode + ": the draw of user word " + std::to_string(value) + " drew red " + std::to_string(Red()) + ", expected " + std::to_string(red));
    }
    const auto hits = counters.hits - start.hits, inserts = counters.inserts - start.inserts;
    Require(hits == values.size() - 1 && inserts == 1, mode + ": draws that differ in a user word the shader only reads at run time made " + std::to_string(hits) + " hits and " + std::to_string(inserts) + " inserts, expected " + std::to_string(values.size() - 1) + " and 1");
}

struct FetchTables {
    alignas(16) std::array<std::uint32_t, 4> buffers{};
    alignas(16) std::array<std::uint32_t, 4> attributes{};
};

void MovedFetchTablesHitTheCache(const std::string& mode) {
    RegisterFetchVertex();
    Registered(FreePixelHeader, FreePixelCode.data(), sizeof(FreePixelCode), 1);
    const auto vertices = reinterpret_cast<std::uintptr_t>(Triangle.data());
    static std::array<FetchTables, 6> tables{};
    for (auto& table : tables) {
        table.buffers = {static_cast<std::uint32_t>(vertices), static_cast<std::uint32_t>((vertices >> 32u) & 0xffffu) | (16u << 16u), 3u, 0x01016facu};
        table.attributes = {0x2cu << 5u, 0, 0, 0};
    }
    tables.back().buffers[2] = 2u;
    const auto draw = [&](const FetchTables& table) {
        const auto buffers = reinterpret_cast<std::uintptr_t>(table.buffers.data());
        const auto attributes = reinterpret_cast<std::uintptr_t>(table.attributes.data());
        return Submit(DrawWith(FreePixelCode.data(), 1, {{0x00c, std::bit_cast<std::uint32_t>(1.0f)}, {0x090, static_cast<std::uint32_t>(buffers)}, {0x091, static_cast<std::uint32_t>(buffers >> 32u)}, {0x092, static_cast<std::uint32_t>(attributes)}, {0x093, static_cast<std::uint32_t>(attributes >> 32u)}}, FetchVertexCode.data(), 8));
    };
    const auto start = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (std::size_t i = 0; i + 1 < tables.size(); ++i) {
        counters = draw(tables[i]);
        Require(Red() == 255u, mode + ": the draw through vertex tables " + std::to_string(i) + " drew red " + std::to_string(Red()));
    }
    const auto hits = counters.hits - start.hits, inserts = counters.inserts - start.inserts;
    Require(hits == tables.size() - 2 && inserts == 1, mode + ": draws whose vertex tables moved to equal contents made " + std::to_string(hits) + " hits and " + std::to_string(inserts) + " inserts, expected " + std::to_string(tables.size() - 2) + " and 1");
    const auto other = draw(tables.back());
    Require(other.hits - start.hits == hits, mode + ": a draw whose moved vertex buffer table holds another descriptor reused the cached draw");
}

}

int main() {
    try {
        bool libraries = false;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            libraries = device->GraphicsPipelineLibraries();
        }
        if (!libraries && std::getenv("APS5_NO_GPL") == nullptr) return VulkanTestSkipped;
        const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
        MovedDataHitsTheCache(mode);
        MovedFreeWordHitsTheCache(mode);
        MovedFetchTablesHitTheCache(mode);
        AgcDriverShutdown_nid_postfix();
        std::cout << "Draws whose user-data pointers or run-time user words moved reused the cached draw " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
