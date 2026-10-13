#include "Recompiler.hpp"
#include "BdaAbi.hpp"
#include "RuntimeAbi.hpp"
#include "SpirvBackend/SpirvModule.hpp"
#if ANYPS5_ENABLE_SPIRV_TOOLS
#include "SpirvBackend/SpirvOptimizer.hpp"
#endif
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>
#include <vector>

namespace ShaderRecompiler {
namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("Mesh pass-through SPIR-V: " + message);
}

bool supports(const SpirvTarget& target, spv::Capability capability) {
    return std::find(target.supportedCapabilities.begin(), target.supportedCapabilities.end(), static_cast<std::uint32_t>(capability)) != target.supportedCapabilities.end();
}

class MeshPassThroughEmitter {
public:
    explicit MeshPassThroughEmitter(std::uint32_t version) : builder(version), version(version) {
        builder.AddMemoryModel(spv::AddressingModelPhysicalStorageBuffer64, spv::MemoryModelGLSL450);
        voidType = builder.Type(spv::OpTypeVoid);
        uintType = builder.Type(spv::OpTypeInt, 32u, 0u);
        intType = builder.Type(spv::OpTypeInt, 32u, 1u);
        ulongType = builder.Type(spv::OpTypeInt, 64u, 0u);
        floatType = builder.Type(spv::OpTypeFloat, 32u);
        uvec4Type = builder.Type(spv::OpTypeVector, uintType, 4u);
        vec4Type = builder.Type(spv::OpTypeVector, floatType, 4u);
        functionType = builder.Type(spv::OpTypeFunction, voidType);
    }

    std::vector<std::uint32_t> Emit(std::uint32_t recordDwords, const std::vector<std::uint32_t>& locations, bool layer, bool layerExtension, std::uint32_t faultBinding) {
        builder.EmitCapability(spv::CapabilityShader);
        builder.EmitCapability(spv::CapabilityInt64);
        builder.EmitCapability(spv::CapabilityPhysicalStorageBufferAddresses);
        builder.EmitExtension("SPV_KHR_physical_storage_buffer");
        if (layer) {
            builder.EmitCapability(layerExtension ? spv::CapabilityShaderViewportIndexLayerEXT : spv::CapabilityShaderLayer);
            if (layerExtension) builder.EmitExtension("SPV_EXT_shader_viewport_index_layer");
        }
        main = result(spv::OpFunction, voidType, spv::FunctionControlMaskNone, functionType);

        const auto pushArray = builder.DecoratedType(spv::OpTypeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}}, uintType, constant(uintType, PushDwords));
        const auto pushType = builder.DecoratedType(spv::OpTypeStruct, {{spv::OpMemberDecorate, {0u, spv::DecorationOffset, 0u}}, {spv::OpDecorate, {spv::DecorationBlock}}}, pushArray);
        const auto push = builder.DefineGlobalVariable(builder.Type(spv::OpTypePointer, spv::StorageClassPushConstant, pushType), spv::StorageClassPushConstant);
        const auto faultWords = builder.DecoratedType(spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}}, uintType);
        const auto faultType = builder.DecoratedType(spv::OpTypeStruct, {{spv::OpDecorate, {spv::DecorationBlock}}, {spv::OpMemberDecorate, {0u, spv::DecorationOffset, 0u}}}, faultWords);
        const auto fault = builder.DefineGlobalVariable(builder.Type(spv::OpTypePointer, spv::StorageClassStorageBuffer, faultType), spv::StorageClassStorageBuffer);
        decorate(fault, spv::DecorationDescriptorSet, 0u);
        decorate(fault, spv::DecorationBinding, faultBinding);
        if (version >= 0x00010400u) interfaces.push_back(fault);
        const auto vertexIndex = addInterface(spv::StorageClassInput, intType);
        decorate(vertexIndex, spv::DecorationBuiltIn, spv::BuiltInVertexIndex);
        const auto perVertexType = builder.DecoratedType(spv::OpTypeStruct, {{spv::OpMemberDecorate, {0u, spv::DecorationBuiltIn, spv::BuiltInPosition}}, {spv::OpMemberDecorate, {0u, spv::DecorationInvariant}}, {spv::OpDecorate, {spv::DecorationBlock}}}, vec4Type);
        const auto perVertex = addInterface(spv::StorageClassOutput, perVertexType);
        std::vector<std::uint32_t> parameters;
        for (const auto location : locations) {
            parameters.push_back(addInterface(spv::StorageClassOutput, vec4Type));
            decorate(parameters.back(), spv::DecorationLocation, location);
        }
        std::uint32_t layerOutput = 0;
        if (layer) {
            layerOutput = addInterface(spv::StorageClassOutput, intType);
            decorate(layerOutput, spv::DecorationBuiltIn, spv::BuiltInLayer);
        }
        builder.EmitEntryPoint(spv::ExecutionModelVertex, main, "main", interfaces);
        builder.AddFunction(spv::OpLabel, builder.AllocateId());

        const auto ptrPushUint = builder.Type(spv::OpTypePointer, spv::StorageClassPushConstant, uintType);
        const auto pushWord = [&](std::uint32_t dword) {
            return wide(load(uintType, result(spv::OpAccessChain, ptrPushUint, push, constant(intType, 0u), constant(intType, dword))));
        };
        const auto address = [&](std::uint32_t low, std::uint32_t high) {
            return result(spv::OpBitwiseOr, ulongType, low, result(spv::OpShiftLeftLogical, ulongType, high, constant(uintType, 32u)));
        };
        const auto argumentDword = MeshDrawPushOffsetBytes / 4u + MeshArgumentAddressDword;
        const auto arguments = address(pushWord(argumentDword), pushWord(argumentDword + 1u));
        const auto argumentWord = [&](std::uint32_t dword) { return wide(loadAt(uintType, offset(arguments, constantLong(4ull * dword)))); };
        const auto records = address(argumentWord(MeshComputeVertexAddressDword), argumentWord(MeshComputeVertexAddressDword + 1u));
        const auto indices = address(argumentWord(MeshComputeIndexAddressDword), argumentWord(MeshComputeIndexAddressDword + 1u));
        const auto corner = wide(result(spv::OpBitcast, uintType, load(intType, vertexIndex)));
        const auto vertex = wide(loadAt(uintType, offset(indices, result(spv::OpIMul, ulongType, corner, constantLong(4u)))));
        const auto record = offset(records, result(spv::OpIMul, ulongType, vertex, constantLong(4ull * recordDwords)));
        const auto vector = [&](std::uint32_t dword) { return result(spv::OpBitcast, vec4Type, loadAt(uvec4Type, offset(record, constantLong(4ull * dword)))); };
        const auto ptrOutputVec4 = builder.Type(spv::OpTypePointer, spv::StorageClassOutput, vec4Type);
        store(result(spv::OpAccessChain, ptrOutputVec4, perVertex, constant(intType, 0u)), vector(0u));
        for (std::size_t i = 0; i < parameters.size(); ++i) store(parameters[i], vector(4u + 4u * static_cast<std::uint32_t>(i)));
        if (layer) store(layerOutput, result(spv::OpBitcast, intType, loadAt(uintType, offset(record, constantLong(4ull * (4u + 4u * locations.size()))))));
        builder.AddFunction(spv::OpReturn);
        builder.AddFunction(spv::OpFunctionEnd);
        return builder.Finalize();
    }

    static constexpr std::uint32_t PushDwords = 32u;

private:
    SpirvModule builder;
    std::uint32_t version;
    std::vector<std::uint32_t> interfaces;
    std::uint32_t main = 0;
    std::uint32_t voidType = 0;
    std::uint32_t uintType = 0;
    std::uint32_t intType = 0;
    std::uint32_t ulongType = 0;
    std::uint32_t floatType = 0;
    std::uint32_t uvec4Type = 0;
    std::uint32_t vec4Type = 0;
    std::uint32_t functionType = 0;

    std::uint32_t constant(std::uint32_t type, std::uint32_t value) { return builder.Constant(spv::OpConstant, type, value); }
    std::uint32_t constantLong(std::uint64_t value) { return builder.Constant(spv::OpConstant, ulongType, static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32u)); }

    template <typename... TArgs>
    std::uint32_t result(spv::Op opcode, std::uint32_t type, TArgs... operands) {
        const auto id = builder.AllocateId();
        builder.AddFunction(opcode, type, id, operands...);
        return id;
    }

    std::uint32_t wide(std::uint32_t value) { return result(spv::OpUConvert, ulongType, value); }
    std::uint32_t offset(std::uint32_t base, std::uint32_t bytes) { return result(spv::OpIAdd, ulongType, base, bytes); }
    std::uint32_t load(std::uint32_t type, std::uint32_t pointer) { return result(spv::OpLoad, type, pointer); }

    std::uint32_t loadAt(std::uint32_t type, std::uint32_t address) {
        const auto pointer = result(spv::OpConvertUToPtr, builder.Type(spv::OpTypePointer, spv::StorageClassPhysicalStorageBuffer, type), address);
        return result(spv::OpLoad, type, pointer, spv::MemoryAccessAlignedMask, 4u);
    }

    void store(std::uint32_t pointer, std::uint32_t value) { builder.AddFunction(spv::OpStore, pointer, value); }
    void decorate(std::uint32_t target, spv::Decoration decoration, std::uint32_t value) { builder.AddAnnotation(spv::OpDecorate, target, decoration, value); }

    std::uint32_t addInterface(spv::StorageClass storage, std::uint32_t type) {
        const auto variable = builder.DefineGlobalVariable(builder.Type(spv::OpTypePointer, storage, type), storage);
        interfaces.push_back(variable);
        return variable;
    }
};

}

RecompileResult BuildMeshPassThroughVertexShader(const MeshProgramLayout& layout, const SpirvTarget& target, bool writeLayer) {
    require(target.spirvVersion >= 0x00010300u && target.spirvVersion <= 0x00010600u, "unsupported SPIR-V target version");
    require(supports(target, spv::CapabilityPhysicalStorageBufferAddresses) && supports(target, spv::CapabilityInt64), "buffer device addresses are unavailable");
    require(layout.recordDwords >= 4u && layout.recordDwords % 4u == 0u, "invalid record size " + std::to_string(layout.recordDwords));
    require(!layout.viewportIndex && !layout.pointSize && layout.clipCullDistances == 0u, "the mesh program exports a viewport index, point size or clip distance");
    std::vector<std::uint32_t> locations;
    for (std::uint32_t location = 0; location < 32u; ++location) {
        if ((layout.parameterMask & (1u << location)) != 0u) locations.push_back(location);
    }
    require(4u + 4u * locations.size() + (layout.layer ? 1u : 0u) <= layout.recordDwords, "the record does not hold every export");
    const bool layer = layout.layer && writeLayer;
    const bool layerExtension = layer && !(target.spirvVersion >= 0x00010500u && supports(target, spv::CapabilityShaderLayer));
    require(!layer || !layerExtension || supports(target, spv::CapabilityShaderViewportIndexLayerEXT), "the device cannot write a layer from a vertex shader");
    MeshPassThroughEmitter emitter(target.spirvVersion);
    RecompileResult result;
    const auto faultBinding = RuntimeAbi::BindingNumber(RuntimeAbi::Stage::Main, RuntimeAbi::Binding::FaultBuffer);
    result.spirv = emitter.Emit(layout.recordDwords, locations, layer, layerExtension, faultBinding);
    result.bindings.push_back({DescriptorKind::StorageBuffer, DescriptorRole::FaultBuffer, 0, faultBinding, 1, {}});
    result.parameterExports = locations;
    result.bdaAbiVersion = BdaAbi::Version;
    result.pushConstants.resize(MeshDrawPushOffsetBytes + MeshDrawPushBytes);
#if ANYPS5_ENABLE_SPIRV_TOOLS
    result.spirv = ValidateAndOptimizeSpirv(result.spirv, target.vulkanVersion, target.spirvVersion);
#endif
    std::uint64_t variant = 0xcbf29ce484222325ull;
    for (const auto word : result.spirv.Words()) variant = (variant ^ word) * 0x100000001b3ull;
    result.variantId = variant | 1u;
    return result;
}

RecompileResult BuildMeshPassThroughVertexShader(const RecompileResult& prepass, const SpirvTarget& target) {
    require(prepass.meshRecordDwords != 0u, "the program was not compiled as a mesh pre-pass");
    MeshProgramLayout layout;
    for (const auto location : prepass.parameterExports) {
        require(location < 32u, "parameter location " + std::to_string(location) + " is out of range");
        layout.parameterMask |= 1u << location;
    }
    layout.recordDwords = prepass.meshRecordDwords;
    layout.layer = prepass.meshRecordDwords > 4u + 4u * layout.ParameterCount();
    return BuildMeshPassThroughVertexShader(layout, target, false);
}

}
