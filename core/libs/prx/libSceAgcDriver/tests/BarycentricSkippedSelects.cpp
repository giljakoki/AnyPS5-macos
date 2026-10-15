// A pixel shader that overwrites the raw perspective I VGPR in both arms of an EXEC if/else, each
// arm skippable with s_cbranch_execz (Unity's v_cmpx / s_andn2_b64 exec pattern), only exports
// the overwritten values, so it lowers to host interpolation without barycentric emulation. If
// one arm leaves I in place, I reaches the export and the shader still needs emulation.
#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>

namespace {

// s_mov_b32 m0, s0; s_mov_b64 s[30:31], exec; s_wqm_b64 exec, exec
// v_interp_p1_f32 v2, v0, attr0.x; v_interp_p2_f32 v2, v1, attr0.x
// s_mov_b64 vcc, exec; v_cmpx_lt_f32 0, v2; s_cbranch_execz else
// v_mov_b32 v0, 1.0
// else: s_andn2_b64 exec, vcc, exec; s_cbranch_execz done
// v_mov_b32 v0, 2.0   (v_mov_b32 v3, 2.0 in the kept variant)
// done: s_mov_b64 exec, s[30:31]; exp mrt0, v0, v0, v0, v0 done vm; s_endpgm
constexpr std::array<std::uint32_t, 16> Overwritten{0xbefc0300u, 0xbe9e047eu, 0xbefe0a7eu, 0xc8080000u, 0xc8090001u, 0xbeea047eu, 0x7c220480u, 0xbf880001u,
                                                     0x7e0002f2u, 0x8afe7e6au, 0xbf880001u, 0x7e0002f4u, 0xbefe041eu, 0xf800180fu, 0x00000000u, 0xbf810000u};
constexpr std::array<std::uint32_t, 16> Kept{0xbefc0300u, 0xbe9e047eu, 0xbefe0a7eu, 0xc8080000u, 0xc8090001u, 0xbeea047eu, 0x7c220480u, 0xbf880001u,
                                              0x7e0002f2u, 0x8afe7e6au, 0xbf880001u, 0x7e0602f4u, 0xbefe041eu, 0xf800180fu, 0x00000000u, 0xbf810000u};

bool emulates(std::span<const std::uint32_t> code) {
    using namespace ShaderRecompiler;
    ShaderPixelStageInfo pixel{};
    pixel.interpolatorCount = 1;
    pixel.inputAddr = PixelInputBit(PixelInput::PerspectiveCenter);
    pixel.hasPerspectiveCenterVgpr = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const std::array<std::uint32_t, 1> userData{0u};
    RecompileRequest request{};
    request.shader = {ShaderStage::Fragment, 0x30000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.userData = userData;
    request.context.pixel = pixel;
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.layout.pushConstantSizeBytes = 128;
    request.useCache = false;
    return Recompile(request).barycentricEmulation.active;
}

bool run(const char* name, bool expected, std::span<const std::uint32_t> code) {
    bool passed = false;
    try {
        passed = emulates(code) == expected;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: %s\n", name, error.what());
    }
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
}

}

int main() {
    bool passed = true;
    passed &= run("I overwritten in both execz-skippable arms lowers to host interpolation", false, Overwritten);
    passed &= run("I kept in one arm still needs barycentric emulation", true, Kept);
    return passed ? 0 : 1;
}
