# Geometry and mesh programs without host geometry or mesh shaders

The PS5 runs every geometry-stage program on the NGG (primitive generation) path: one merged program per thread group reads the input primitives, optionally runs a geometry-shader half over them, and exports vertices and primitives itself. The driver translates such a program into a Vulkan mesh shader (`VK_EXT_mesh_shader`). MoltenVK exposes neither mesh shaders nor geometry shaders, so on macOS every draw on that path stops, and the barycentric emulation that builds a host geometry shader stops as well. This document inventories what we receive, what the recompiler already expresses, the candidate designs, and the chosen plan.

## What arrives from the title

### Registration and stage selection

`PlanRegistered` in [ShaderRegistry.cpp](../../core/libs/prx/libSceAgcDriver/Execution/src/Driver/Shaders/ShaderRegistry.cpp) maps registered shader types to recompiler stages. Types 4 and 6 are geometry programs and become `ShaderStage::Mesh` (value 7 of the request enum in [Recompiler.hpp](../../core/shader/recompiler/Recompiler.hpp), which `.req` files store). SPIR-V emission errors print the IR stage instead, where `IrShaderStage::Mesh` is 5, and print the shader hash in decimal behind a `0x` prefix: `hash=0x14218391552 stage=5` is the mesh program at 0x34f68d000. A type 2 (vertex) program is also moved to the mesh stage when its draw state routes it through the geometry path. Geometry-path programs are never prepared at registration; they are prepared at their first draw because their configuration lives in the draw's context registers, not in the shader header.

### Draw state

`DecodeShaderStages` in [State.cpp](../../core/libs/prx/libSceAgcDriver/Graphics/src/State.cpp) turns the context registers into a `MeshConfiguration`:

- `VGT_SHADER_STAGES_EN` (context 0x2d5): GS_EN (bit 5) or a vertex program that needs a workgroup selects the geometry path, PRIMGEN_EN (bit 13) is required, bit 22 selects wave32.
- The primitive type (user-config 0x242): point list (1), line list (2), triangle list (4), triangle fan (5), triangle strip (6). Patch topology goes to the tessellation path instead.
- The subgroup size register (user-config 0x25b): input vertices per subgroup in bits 9-17 and input primitives per subgroup in bits 0-8.
- `GE_MAX_OUTPUT_PER_SUBGROUP` (context 0x1ff): the output vertex budget of one subgroup.
- `VGT_GS_MAX_VERT_OUT` (context 0x2ce): output vertices per input primitive, used to bound output primitives as `primitives * (maxVertOut - 2)`.
- `VGT_GS_OUT_PRIM_TYPE` (context 0x29b) must be 2 (triangle strip output); other output types throw.
- `VGT_ESGS_RING_ITEMSIZE` (context 0x2ab): dwords per input vertex in the ES-to-GS LDS ring.
- `SPI_SHADER_PGM_RSRC2_GS` (shader 0x8b) bits 19-26 give the LDS allocation in 128-dword units.

From these the driver derives: primitives per group (the smallest of the subgroup primitive count, what the vertex count admits and what the output budget admits), input vertices per group, maximum output vertices and primitives, and threads per group (the largest of input vertices, input primitives, output vertices and output primitives, rounded up to whole waves).

The draw packet supplies the index count, index size and address, instance count, and, for indirect draws, an argument address in guest memory. `ResolveMeshArguments` and `recordMeshArguments` in [Draw.cpp](../../core/libs/prx/libSceAgcDriver/Graphics/src/Draw.cpp) turn the packet into a 20-byte argument record: a `VkDrawMeshTasksIndirectCommandEXT` (group counts x, y, z), the index count and the first index, followed by an optional primitive-restart table for indexed strips.

### What the program sees at entry

The mesh entry in [InstructionTranslator.cpp](../../core/shader/recompiler/Translation/src/InstructionTranslator.cpp) reproduces the NGG wave inputs: `s2` holds the group's input vertex count (bits 12-20) and primitive count (bits 22-30), `s3` the wave index, the wave count and the per-wave vertex and primitive counts, `v0`/`v1` the three ES-ring offsets of the thread's input primitive (strip parity and restart handled), `v2` the primitive id, `v5` the vertex index (fetched through the index buffer in the program, since hardware index fetch does not exist on this path) and `v8` the instance id (workgroup y). Workgroup x is the group index; workgroup y is the instance.

### What the program produces

- `s_sendmsg GS_ALLOC_REQ` becomes `MeshAllocate`: output vertex count in bits 0-9 and output primitive count in bits 12-22, stored in workgroup memory.
- A primitive export (target 20) packs three 9-bit output vertex indices at bits 0, 10 and 20 and a null-primitive flag at bit 31.
- Position exports (targets 12-15); position 1 carries the render-target layer in its z component.
- Parameter exports (targets 32 and up), one vec4 each.

The SPIR-V backend ([SpirvModuleEmitter.cpp](../../core/shader/recompiler/SpirvBackend/src/SpirvModuleEmitter.cpp), `DefineMeshOutputs`, `EmitMeshEntryPoint`, `EmitMeshAllocate`) keeps every export in a private per-lane array while the guest function runs, then, after a workgroup barrier, calls `SetMeshOutputsEXT` with the allocated counts, copies each lane's vertex to `gl_MeshVerticesEXT`-style output arrays, writes `PrimitiveTriangleIndicesEXT` and `CullPrimitiveEXT`, and takes the layer from the primitive's provoking vertex. A wave64 program on a 32-lane host subgroup runs two guest lanes per invocation (`laneCount` 2).

### The program that stops a UE4 title

A UE4 title aborts with `SPIR-V emission failed: stage=5 reason=the program needs subgroup operations, which the device does not support in this shader stage`. Its request, replayed with `agc_shader_replay --mesh-layout --subgroup-stages=32`, describes:

- wave64, 192 threads (three waves), input triangle strips, 62 input primitives over 64 input vertices per group;
- at most 192 output vertices and 64 output primitives per group, so `VGT_GS_MAX_VERT_OUT` is 3: every input triangle yields at most one output triangle;
- 2176 dwords of LDS and a 4-dword ES ring item.

The ES half fetches each input vertex with two-dword buffer loads, scales and offsets it with constants from a constant buffer, and stores four dwords per vertex into the LDS ring. The GS half reads the three ring entries of its strip triangle, decides which triangles survive, and compacts the survivors with ballots, 64-bit bit counts and a DPP prefix sum (the usual NGG compaction). It records seven dwords per output vertex in LDS (a position, a two-component parameter and a layer), issues `GS_ALLOC_REQ` with the compacted counts, and exports one primitive per thread below the primitive count, then position 0 (xyzw), position 1 with only z (the layer) and one parameter per thread below the vertex count. Six barriers separate the phases. In short, it is a layered-rendering pass-through with culling: one triangle in, at most one triangle out, routed to a render-target layer.

`agc_shader_replay --mesh-layout` prints for it: wave64, three waves per group, one allocation, six barriers, LDS and subgroup operations used, so not a vertex-stage candidate; position 0 with mask `f`, position 1 with mask `4`, one parameter, a primitive export; 8-dword records, 192 per group, 6144 vertex bytes and 768 index bytes per group. The layer is reported absent because requests do not carry `PA_CL_VS_OUT_CNTL` (context 0x207), which says what position 1's z component means: the recompiler then drops that component without an error on the vertex and mesh paths alike (recorded in [TechnicalDebt.md](TechnicalDebt.md)). With the register plumbed, the record grows to 12 dwords (48 bytes) written per corner, about 9 KiB per group.

Two independent things stop it on this Mac: mesh shaders do not exist (`mesh=0`, and `Draw.cpp` throws `device does not support mesh shaders`), and even with them the device offers subgroup operations only in fragment, compute and tessellation-control stages (MoltenVK's stage mask 0x32), while the program needs ballots across the wave. The compute stage has both workgroup memory and subgroups.

## What the recompiler can express today

- The whole NGG program, ES and GS halves, LDS, barriers, `GS_ALLOC_REQ`, primitive and vertex exports, strip and fan input assembly, primitive restart through a table, indexed and non-indexed draws, instancing and indirect arguments, for wave32 and wave64.
- A host mesh shader as the only consumer of those exports.
- Generated auxiliary stages: `BuildRectListShaders` (a tessellation pair for rectangle lists) and `BuildBarycentricGeometryShader` (a host geometry shader that hands the three vertices' attributes to a pixel shader that reads barycentrics). The latter needs `geometryShader` and stops on MoltenVK too.
- Compute programs with workgroup memory, subgroups (where the device has them), buffer device addresses and storage buffers.

Nothing yet runs a mesh-stage program anywhere but in a mesh shader.

## Candidate designs

### (a) Compute pre-pass, then an ordinary draw

Recompile the same mesh-stage IR as a compute shader with the same workgroup size and the same entry inputs (workgroup x = group, y = instance). Only the epilogue changes: instead of `SetMeshOutputsEXT` and output arrays, the workgroup writes its vertices and primitives to device buffers. A generated pass-through vertex shader then reads one record per vertex and outputs the position, the parameters at the locations the mesh shader would have used, and the layer, and the original pixel shader runs unchanged.

Output placement. Each group owns a fixed slot of `maxVertices` vertex records and `maxPrimitives` triangles (group-major, instance-minor). Unused vertex records stay unwritten and unused or null primitives become degenerate triangles (all three indices equal), which every rasterizer discards. No atomic counter is needed, and the primitive order is the hardware order: groups in dispatch order, primitives in export-lane order within a group. Blending and depth-equal passes depend on that order, which an atomic append would lose. A later phase may compact with a prefix sum over group counts when the waste matters.

Draw. `vkCmdDrawIndexed` with `groups * instances * maxPrimitives * 3` 32-bit indices. Each index already includes its group's vertex base, so the draw is a plain triangle list; no primitive restart is involved (list restart would need `VK_EXT_primitive_topology_list_restart`). For indirect draws the group count exists only on the GPU: the existing argument record already starts with a `VkDrawMeshTasksIndirectCommandEXT`, which has the layout of `VkDispatchIndirectCommand`, so the pre-pass is dispatched with `vkCmdDispatchIndirect` from it, and the index count for `vkCmdDrawIndexedIndirect` is written by the pre-pass's first invocation of group 0 (or by the argument-record shader that already exists).

Layer. When the program exports a layer, the layer is per primitive while a vertex shader writes it per vertex. Records are then written per primitive corner (three records per triangle, no sharing), so every vertex of a primitive carries the primitive's layer whatever the host's provoking-vertex rule for `Layer` is. The pass-through vertex shader needs `shaderOutputLayer` (Vulkan 1.2, which MoltenVK reports on macOS) to write it.

Vulkan and SPIR-V needs: storage buffers or physical storage buffer pointers in compute (both present), `drawIndirect` and dispatch indirect (core), 32-bit index buffers (core), subgroup ballot and shuffle in compute (present on MoltenVK), workgroup memory up to the program's LDS size (2176 dwords here, under Metal's 32 KiB), `shaderOutputLayer` for layered programs. It needs no geometry shader and no `VK_EXT_mesh_shader`.

### (b) Mesh program as a vertex shader

When a program's output is a fixed one-to-one image of its input (one output vertex per input vertex, the input topology kept, no LDS exchange, no barrier, no cross-lane operation), it can run as an ordinary vertex shader over the input indices. The driver already does this for PRIMGEN vertex programs that use neither LDS nor `s_barrier`; only programs that use them are moved to the mesh path. Every program that reaches the mesh stage therefore fails at least one of the conditions by construction, and the UE4 program uses LDS, six barriers and wave-wide ballots. A vertex-shader mapping would have to prove that the LDS round trip is a per-lane identity and that culling may be dropped, which is not decidable in general. This design is worth keeping only as a fast path for a classifier that proves it; it cannot be the general mechanism.

### (c) Native mesh shaders

`VK_EXT_mesh_shader` is available on NVIDIA Turing and later, AMD RDNA2 and later (RADV and the AMD driver) and Intel Arc, which is why the current path targets it. Metal 3 has object and mesh functions on Apple-family 7 GPUs (A14, M1) and later and on Mac-family 2 GPUs, but MoltenVK does not translate `VK_EXT_mesh_shader` to them, and Metal has no geometry functions at all. Making MoltenVK expose Metal mesh shaders is a MoltenVK project outside this tree, and Metal mesh functions also limit output vertex and primitive counts per threadgroup and threadgroup memory, so even then the current path would need checks.

### Choice

Design (a). It reuses the whole existing translation, including LDS, barriers and wave64 lane pairing; the compute stage is where MoltenVK offers subgroups, so it also removes the second blocker; it keeps hardware primitive order without atomics; it needs only core Vulkan features plus `shaderOutputLayer`; and its output buffers are the same shape a later barycentric emulation without geometry shaders can use (a vertex program run in compute, expanded to per-corner records that carry all three vertices' attributes). Design (b) stays a possible fast path for proven one-to-one programs; design (c) is not available on the target.

## Sequencing in the driver

The draw path in [Draw.cpp](../../core/libs/prx/libSceAgcDriver/Graphics/src/Draw.cpp) already has the shape the pre-pass needs: an indirect mesh draw forces a new render pass (`StartDrawPass(..., forced)`), and between the upload barrier and `Pipeline::Begin` it records the argument-record work. The pre-pass goes in the same place:

1. Force the pass to end (a dispatch is illegal inside a render pass instance).
2. Bind the compute pipeline of the emulated variant and the draw's guest resources at the compute bind point (the same descriptor set and push data the mesh shader would have used; the guest user data and the mesh draw words keep their offsets).
3. Allocate the transient vertex, index and indirect buffers for this draw from a per-batch ring and release them when the batch's fence signals, like the other per-draw scratch buffers.
4. Dispatch `groups x instances` directly, or indirectly from the argument record.
5. Barrier from compute-shader writes to `VERTEX_ATTRIBUTE_READ | INDEX_READ | INDIRECT_COMMAND_READ | SHADER_READ` at the vertex-input, vertex-shader and draw-indirect stages.
6. Begin the pass and draw with the pass-through vertex shader and the title's pixel shader.

Everything stays on the graphics queue in one command buffer, so no semaphore is needed. The output buffers are device-local scratch, never guest memory, so the guest-memory hazard tracking (`PassAccess`) only needs the reads the mesh program already declared.

## Output formats and strides

A record holds the position (4 dwords), each parameter (4 dwords, raw bits) and, when exported, the layer (1 dword), padded to 16 bytes. Values are written as raw 32-bit words exactly as exported; the pass-through shader bitcasts them, so no format conversion happens between the stages and DCC, tiling and guest formats are not involved (the buffers are host-only). Compressed (16-bit packed) exports are written in their packed form and unpacked by the pass-through shader with the same rule the mesh epilogue uses. Only the color and depth targets of the following draw see guest formats, and those are unchanged.

## Performance expectations

The pre-pass costs about what the mesh shader would, plus the record writes: for the UE4 program 64 triangles of three 48-byte records per group, about 9 KiB per group, or about 16 MiB for a 100,000-triangle draw, in unified memory. The larger cost on Apple GPUs is the render-pass split: every split stores and reloads the color and depth attachments of a tile-based GPU, roughly a full-screen read and write per split. Draws that alternate between mesh and vertex paths inside one pass would split every time. A later phase therefore records pre-passes for all geometry-path draws of a batch before the pass that consumes them when their inputs are not written inside that pass (the existing `PassAccess` hazards give the answer), so a pass is split once at most.

## Phases

Each phase lands with its tests registered in the existing suites. Replays of captured requests from titles are not committed; they are run by hand with `agc_shader_replay`, and the committed tests use synthetic programs such as the NGG geometry program in [tests/Mesh.cpp](../../core/libs/prx/libSceAgcDriver/tests/Mesh.cpp).

1. Mesh program layout (done in this change). `ShaderRecompiler::DescribeMeshProgram` translates a mesh-stage request up to resource analysis and reports the group shape (input topology, primitives and vertices per group, output budget, threads and waves), the exports the program makes (positions with their component masks, parameters, layer, primitive export), the cross-workgroup features it relies on (LDS, barriers, `GS_ALLOC_REQ`, subgroup operations) and the output record size and per-group buffer sizes design (a) will allocate. `agc_shader_replay --mesh-layout` prints it. Test: `agc_mesh_layout_tests` on the synthetic NGG geometry program in wave64 and wave32.
2. Compute variant of the mesh program. Prerequisite: `PA_CL_VS_OUT_CNTL` reaches the request (the draw-time vertex stage info and the cache key), so the layer and viewport index of position 1 are known instead of dropped. A `MeshConfiguration` flag (part of the cache key and of the serialized request) selects a GLCompute entry point with the same guest function, the compute stage's subgroup capability, and an epilogue that writes the fixed per-group slots through a buffer address carried in the mesh argument record. `agc_shader_replay --mesh-compute` replays it. Tests: the synthetic program compiles and validates for a target without mesh shaders and with the MoltenVK stage mask; a device test dispatches it and checks the written records and indices against the triangle grid the mesh tests draw; by hand, the UE4 request compiles with `--subgroup-stages=32`.
3. Pass-through vertex shader. A generator next to `BuildRectListShaders` builds the vertex shader from the mesh result's output list (parameter locations, layer) and the record layout. Tests: generated SPIR-V validates; it links with the synthetic pixel shader in a pipeline on the MoltenVK harness.
4. Driver plumbing behind the opt-in `APS5_MESH_EMULATION=1`: the mesh path in `Draw.cpp` compiles the compute variant when the device has no mesh shaders, records the pre-pass as described above, and draws. Tests: the existing `agc_driver_mesh_tests` pixel checks (lists, strips, fans, restart, wave32, indirect, instancing) run a second time with emulation forced on, and pass on MoltenVK. The approximation list in [TechnicalDebt.md](TechnicalDebt.md) gains an entry for anything that differs from the mesh path.
5. Batching: pre-passes hoisted before the pass that consumes them, the transient ring sized from the batch, and compaction of null primitives when it pays. Test: a recorder test that counts render-pass splits for alternating mesh and vertex draws.
6. Barycentric emulation without geometry shaders through the same pre-pass (vertex program in compute, per-corner records with the three vertices' attributes), then a decision on making emulation the default on devices without mesh shaders.
