# Architecture

How a PS5 executable becomes a native Linux or Windows program. Nothing is emulated: the converted executable runs as a normal process and calls native implementations of the system libraries.

## Overview

```mermaid
flowchart LR
    subgraph input["PS5 game"]
        elf["input.elf"]
        mods["sce_module/*"]
    end

    subgraph relinker["relinker (core/relinker)"]
        direction TB
        intel["--to-intel:<br/>lower AMD-only instructions<br/>(codegen)"]
        pipeline["RelinkerPipeline:<br/>read imports by NID,<br/>check syscalls, filter unused NIDs,<br/>build SysV dynamic section"]
        guest["GuestModuleBuilder:<br/>convert bundled modules"]
        patcher["LinuxElfPatcher / WindowsPePatcher"]
        intel -.-> pipeline --> guest --> patcher
    end

    subgraph build["build (core/libs)"]
        direction TB
        prx["core/libs/prx/*<br/>shared libraries"]
        nid["nid_patcher:<br/>rename exports to their NIDs"]
        prx --> nid
    end

    subgraph out["Native program"]
        app["app.elf / app.exe"]
        app0["app0/sce_module/*"]
        libs["libs/*.prx"]
    end

    elf --> intel
    mods --> guest
    patcher --> app
    guest --> app0
    nid --> libs
    app -- "OS loader binds imports by NID" --> libs
    app0 -- "OS loader binds imports by NID" --> libs
```

- [`core/relinker/main.cpp`](../../core/relinker/main.cpp) runs the steps in this order. `--to-intel` is optional, see [USAGE.md](../user/USAGE.md).
- Each library in [`core/libs/prx`](../../core/libs/prx) builds as a shared library. After the build, `nid_patcher` ([`core/libs/nid`](../../core/libs/nid)) renames every export to its NID, computed from the function name. `APS5_EXPORT("<nid>", func)` sets the NID directly when the name is unknown.

## Thread cancellation

[`Thread.cpp`](../../core/libs/prx/libkernel/Pthread/src/Thread.cpp) records cancellation requests on the target guest thread. New threads start with cancellation enabled and deferred. A disabled request stays pending; enabling deferred cancellation returns before the next cancellation point acts on it. Repeated requests do not add cleanup invocations.

[`Cancel.hpp`](../../core/libs/prx/libkernel/Pthread/include/Cancel.hpp) connects condition, semaphore and join waits to the request. A canceled condition waiter reacquires its guest mutex before cleanup. Semaphore and join waits release their internal locks before exiting. A request pending on entry to a semaphore wait is acted on before consuming an available token; a canceled join leaves its target joinable and its result output untouched.

Cancellation exits through `scePthreadExit` with result pointer `1`. It runs cleanup handlers in reverse push order before thread-specific destructors; cancellation points inside cleanup must not start another exit. The host thread then terminates through the existing Windows or Linux lifecycle, and join waits for that termination. Host thread termination is not used to inject cancellation into a running guest frame.

The reference contract and constants come from FreeBSD libthr, pinned at [`5ed7eb0d`](https://github.com/freebsd/freebsd-src/blob/5ed7eb0d97ba4436218e810f61bd059acba984c2/lib/libthr/thread/thr_cancel.c), with cleanup and exit ordering in [`thr_exit.c`](https://github.com/freebsd/freebsd-src/blob/5ed7eb0d97ba4436218e810f61bd059acba984c2/lib/libthr/thread/thr_exit.c#L196-L259). These are reference semantics, not console measurements. The supported cancellation points and incomplete asynchronous behavior are listed in [TechnicalDebt](TechnicalDebt.md).

[`GuestPthreadCancelRecovery.cpp`](../../core/libs/tests/GuestPthreadCancelRecovery.cpp) checks pending requests before ordinary and timed semaphore acquisition, cleanup reentrancy and destructor ordering, enabled and disabled timed condition waits, canceled join output, and subsequent resource and thread reuse. Host atomic gates establish pending requests; acquiring the condition mutex after the worker releases it establishes wait entry without a scheduling sleep. Both `guest_pthread_cancel_recovery` and `guest_pthread_cancel_recovery_coarse` retain checks in Release and have a 20-second timeout, shorter than the 60-second guest waits. The latter sets `APS5_COARSE_TIMED_WAITS=1` to exercise the Windows fallback; Linux uses that condition-variable path by default.

## APR and AMM command buffers

[`Apr.cpp`](../../core/libs/prx/libkernel/Apr/src/Apr.cpp) executes APR and AMM command buffers. A buffer runs on the submitting thread until a `WaitOnAddress` or `WaitOnCounter` that is not yet satisfied; the rest of it, and every buffer submitted after it, is copied and runs on a worker thread, so a submitter that satisfies the wait after the submit returns does not hang. Buffers without such a wait still complete before the submit returns.

APR and AMM each have their own queue and worker thread. Order is kept inside a queue; a wait in one does not hold up the other, because AMM `WriteAddress` commands advance the counters that APR buffers wait on. Submission ids are per queue, and `sceKernelAprWaitCommandBuffer` and `sceAmprAmmWaitCommandBufferCompletion` block until the queue has completed that id. A command that throws on a worker aborts the process.

[`GuestAmpr.cpp`](../../core/libs/tests/GuestAmpr.cpp) checks both cases: a submit whose wait is satisfied afterwards returns and keeps buffer order, and an AMM buffer submitted after a waiting APR buffer still completes.

## Graphics

```mermaid
flowchart LR
    game["Game:<br/>command buffers"] --> submit["libSceAgcDriver/Submit<br/>DCB / ACB"]
    submit --> pm4["Execution/Pm4:<br/>state, draws,<br/>dispatches"]
    pm4 -- "shader + state" --> cache{"Compiled variant<br/>in memory or<br/>on disk?"}
    cache -- yes --> vk
    cache -- no --> dec

    subgraph recompiler["core/shader/recompiler"]
        dec["RdnaDecoder"] --> cf["ControlFlow:<br/>graph + structurize"]
        cf --> tr["Translation:<br/>RDNA to IR"]
        tr --> opt["Optimization:<br/>SSA, resources,<br/>bindings"]
        opt --> spv["SpirvBackend:<br/>emit SPIR-V"]
    end

    spv --> vk["libSceAgcDriver/Graphics:<br/>Vulkan pipeline"]
```

- [`Recompiler.cpp`](../../core/shader/recompiler/Recompiler.cpp) runs the stages in this order. With `ANYPS5_ENABLE_SPIRV_TOOLS`, the SPIR-V is also validated and optimized with SPIRV-Tools.
- `SpirvTarget::subgroupStages` carries the device's `subgroupSupportedStages`. For a vertex, local or tessellation-evaluation program on a target without subgroups in that stage (MoltenVK), [`LaneLocalLowering`](../../core/shader/recompiler/Optimization/src/LaneLocalLowering.cpp) runs after the static interface is applied: it folds lane-index values that are the same for every lane of the wave, rewrites own-lane bit tests of ballot words to the predicate itself, replaces first-lane reads of wave-uniform values, runs waterfall loops once with the lane's own value of the `v_readfirstlane` operand when every effect and carried value is masked by the lane's match and an exhaustive walk of the loop's boolean mask state shows that each lane matches exactly once before the exit, and marks `execz`/`vccz` branches lane-local when their regions are proven EXEC-masked and their merge phis unchanged for skipping lanes. What remains is reported with its guest addresses, and the SPIR-V emitter refuses any leftover subgroup use for such a stage. `APS5_SUBGROUP_STAGES=<hex>` removes stages from the device's mask to test this on any GPU.
- `ShaderRecompiler::Recompile` keeps compiled variants in memory, and `ShaderDiskCache` stores them on disk so later runs reuse them.
- A compute shader whose LDS plus its lock dword exceeds the host's `maxComputeSharedMemorySize` is not given a `Workgroup` array. `Recompile` sets `ShaderInfo::sharedMemoryBytes` for it and the SPIR-V backend addresses the LDS in a coherent storage buffer at descriptor set 1 (`WorkgroupMemoryDescriptorSet`), with a slice of `RecompileResult::workgroupMemoryDwords` per workgroup, indexed by `WorkgroupID` and `NumWorkGroups`. `VulkanDevice` keeps one such buffer, sized from the dispatch dimensions, and binds it after the resource set; programs that fit keep using shared memory.
- `Driver::RegisterShader` splits shader preparation in two. `PlanRegistered` runs on the registering guest thread: it validates the registers and entry point, decodes the code and builds the recompile request, so invalid registrations still fail there. The RDNA to IR translation and resource plan (`PrepareShader`) run on a background pool of `hardware_concurrency() / 2` detached threads (at least 2, 64 MiB stacks).
- The shader registry maps a code address to every header registered for that code, in registration order. Static, graphics and rectangle ABI resolution pick the header at the bound address (or one equal to it, `SameHeader`, which also takes a hull header fused by `sceAgcFuseShaderHalves` (type 3) as the registered back half (type 7) it copies, the fused registers aside; graphics ABI resolution then uses the fused header's shader registers); draws and dispatches pick the program by the stage's binary type (`RegisteredProgram`), and a front entry matching both type 2 and type 4 throws. Within a type the last registration wins; re-registering an identical earlier header makes its snapshot current again without preparing it again, and changed code drops the older headers.
- The snapshot is published as pending. `SourceHandleFor`, `InvocationFor` and `ShaderPreparationTransaction::Edit` / `Read` wait for the pending job before reading `PreparedShaders`, and rethrow its failure. `APS5_SYNC_SHADER_PREPARE=1` prepares on the registering thread instead.
- A draw or dispatch whose state matches no artifact prepared at registration (a title that builds its static ABI state itself, without `sceAgcDriverResolve*Abi`) gets one prepared at use by `SourceHandleFor` or `InvocationFor`, logged once per artifact and kept in the snapshot for the next use.
- A compute shader whose `s_swappc_b64` calls a user-data pointer cannot be linked at registration, because the callee's address is only known at dispatch. `InvocationFor` in [`ShaderRegistry.cpp`](../../core/libs/prx/libSceAgcDriver/Execution/src/Driver/Shaders/ShaderRegistry.cpp) reads each callee from guest memory, appends one copy per call after the caller (`LinkUserDataCalls`) and prepares that linked code, so a different callee gets its own prepared entry.
- A draw's `Graphics::Pipeline` comes from `CachedPipeline`. When the device has `VK_EXT_graphics_pipeline_library` with fast linking, `VK_EXT_extended_dynamic_state` and `VK_KHR_dynamic_rendering`, draws render with `vkCmdBeginRenderingKHR` instead of render passes and the pipeline is linked from four libraries (vertex input, pre-rasterization shaders, fragment shader, fragment output) kept per device in [`PipelineLibrary.cpp`](../../core/libs/prx/libSceAgcDriver/Graphics/src/PipelineLibrary.cpp). A library built for one pipeline is reused by the next: another blend, vertex layout or target format builds only that part (the shader libraries name no attachment format), and cull mode, front face, depth and stencil state are dynamic, so changing them only links. The fast link is what the draw waits for; a background thread then links the same libraries with link-time optimization and the pipeline binds that one from the next draw on. Push constants then use a 256-byte block with a 128-byte slot per side: the pre-rasterization stages stack from byte 0, the pixel shader always starts at byte 128 (`StagePushOffset`), so its library does not depend on how much push data its vertex shader uses. Pipelines with per-face depth bias, a separate alpha blend into an 8_8 target with the alternate component order (both draw twice), rect lists, geometry shaders or a stage without a variant id are created whole, still with dynamic rendering; devices without the extensions (or with `APS5_NO_GPL=1`) keep render passes and whole pipelines.
- A draw whose `PA_SC_AA_CONFIG` selects 2, 4 or 8 samples renders into host-only multisampled images. [`MultisampleTarget.cpp`](../../core/libs/prx/libSceAgcDriver/Graphics/src/MultisampleTarget.cpp) keeps one image per device, surface address, format, extent and sample count (`MultisampleTargetView`); the pipeline, render pass and library keys carry `State::samples`, and a multisampled depth target is a multisampled depth surface. The guest sample planes and FMASK are never read or written, so the registry also guards the target's guest range: a CPU read (`GuestMemory::SetReadGuard`), a DMA copy, and a sampled or storage image read of it throw. A `CB_COLOR_CONTROL` resolve mode over a multisampled CB0 runs as a render pass whose resolve attachment is CB1's resident image (`ResolveMultisampleTarget`). A zero fill of the target's CMASK (`NoteColorMetadataFill`, from fill kernels and CP DMA fills) clears the image at its next use.
- A sampled texture that has no storage view (block-compressed formats, or memory outside host imports) is cached as a snapshot of its guest bytes in [`ShaderResources.cpp`](../../core/libs/prx/libSceAgcDriver/Graphics/src/ShaderResources.cpp), and each lookup proves the snapshot current. In write-watched memory, `GuestMemory::UnchangedSince` is the proof. Host imports under the macOS write watch, which compares page contents instead of protecting pages, are compared, not watched: a GPU write into an import would be reported as a CPU store. For those, the snapshot is compared with guest memory, and a successful comparison is remembered with the present count (`Recorder::Presents()`), the time, `GuestMemory::ForgetSerial()` and the serial of the driver write log (`GuestMemory::NoteDriverWrite`, fed by every driver store, `MarkWritten` and recorded GPU write). A later lookup reuses the proof while no frame was presented, the proof is younger than `APS5_SNAPSHOT_PROOF_MS` (500 ms by default), no forget happened and `GuestMemory::DriverWroteOver` finds no logged write over the surface since; a write log that wrapped past the proof counts as written. The driver's own waits and syncs do not end a proof, so a surface is compared at most once per presented frame or lifetime, plus once after each driver write over it. This is an approximation: a guest CPU store into host-imported texture memory is seen at the next present or when the proof expires, whichever comes first, not at the next wait. Every 10 seconds a `[resources] snapshot proofs` line reports memo hits and compares, with the reason for each compare. `APS5_NO_SNAPSHOT_PROOF_MEMO=1` compares at every lookup, the exact path.
- A block-compressed surface in host-imported memory whose DCC keys read uncompressed keeps no CPU snapshot and is never compared on the CPU: `Texture`'s host-import constructor and `Texture::RefreshFromImport` detile it on the GPU straight from the imported `VkBuffer` at the surface's offset into a pooled linear buffer and copy that into the BC image. The lookup refreshes it when the proof above does not hold (a present since, an expired proof, a forget, or a logged driver write over the surface), so the GPU rereads guest memory at most once per presented frame and the CPU's stores are seen with the same latency as the compared snapshots. Comparing a frame's worth of large surfaces was the frame time when the host was short of memory (a title mapping about 6 GiB of direct memory on a 24 GB Mac): the compares ran at about 1.5 GB/s against about 24 GB/s for the same compare in a test, and the CPU snapshot was a second copy of every such texture. `APS5_NO_IMPORTED_SNAPSHOT_REFRESH=1` keeps those surfaces compared snapshots; `APS5_NO_SNAPSHOT_PROOF_MEMO=1` disables both and compares at every lookup. The `[resources] snapshot proofs` line also counts the GPU refreshes and, since its previous line, the bytes compared, the compare rate and the process's minor and major page faults.
- A refresh of such a surface is gated by a GPU hash of the imported range (`Graphics/shaders/SurfaceHash.comp`, recorded by `Recorder::RecordSurfaceHash`): a reduction over the surface's 16-byte blocks through its buffer device address into a 128-bit digest of position-mixed 32-bit words summed and XORed in two lanes each, read once from a host-visible buffer. When the proof above lapses at a present the lookup records a hash pass instead of a refresh; a later lookup that finds the pass's batch finished compares the digest with the stored one and refreshes only when it differs, recording a new hash just before the detile so the stored digest describes the bytes the image holds. A logged driver write over the surface refreshes at once. An unchanged surface therefore costs one read of its bytes per presented frame, and a CPU store is seen at the first lookup after the hash that follows it completes. In the recorder test a hash pass over 16 MiB took about 0.23 ms of GPU time against about 0.77 ms for the detile and copy it saves; a compare of a kept tiled copy driving indirect dispatches would read twice the bytes, hold a second GPU copy of every surface and still need an unconditional copy into the BC image. A digest collision would keep a changed surface stale until its next change; `APS5_NO_IMPORTED_SNAPSHOT_HASH=1` refreshes at every lapsed proof instead. The `[resources] snapshot proofs` line counts the hash passes and the unchanged results. A `[resources] snapshot lookup CPU time` line before it gives, since its previous line, the calls and CPU time of every sampled lookup and of the imported path's phases (imported hits, import find, proof, hash record, digest read, refresh record); under `APS5_PROFILE_GPU=1` the hash passes and refreshes are timed on the GPU as the `surface-hash` and `snapshot-refresh` classes of the `[gputime]` line.
- A storage image whose memory is compared, not watched (a resident target in a host import under the macOS watch), compared its whole surface with `original` at every refresh, once per draw that used it. `StorageTexture::compareUntracked` now remembers a whole-surface match with the present count, the time, the forget serial and the driver write log serial, and reuses it while no frame was presented, the match is younger than 500 ms, no forget happened and no driver store or `MarkWritten` touched the surface since (`GuestMemory::DriverWroteOver` with stores only: the recorded GPU writes of draws into the target leave guest memory alone until a write-back, which marks it written). Every change of `original` drops the match. This is an approximation: a guest CPU store into such a surface is seen at the next present or within 500 ms. `APS5_NO_SNAPSHOT_PROOF_MEMO=1` compares every time. The driver write log keeps recorded GPU writes apart from driver stores for this. The `[storage]` line counts the untracked compares and the ones a match answered.

### Shader MODE register access

`GuestContext::floatMode` carries the initial shader mode through `Recompile` and `TranslateOptions` into each block's `TranslationContext`. `s_getreg_b32` reads confined to MODE bits 0–3 return the selected initial rounding bits in the destination's low bits. Bits 0–1 select f32 rounding; bits 2–3 select f16/f64 rounding. Missing mode metadata retains the legacy round-to-nearest-even assumption. Reading other MODE fields or hardware registers throws.

An immediate MODE write confined to the rounding fields is accepted only when its selected replacement bits equal the initial state. Source bits above the selected width are ignored. `s_round_mode` similarly compares its low four immediate bits with the initial rounding fields. A state-changing write or an SGPR-sourced MODE write throws with its instruction and program counter. Runtime transitions require control-flow-aware mode tracking and matching arithmetic lowering. Writes to other hardware registers retain their existing behavior and remain technical debt.

The register layout and instruction contract follow [AMD RDNA2 ISA sections 5.8 and 6.4, Table 24](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna2-shader-instruction-set-architecture.pdf), cross-checked against LLVM 20.1.8's [hardware-register encoding](https://github.com/llvm/llvm-project/blob/87f0227cb60147a26a1eeb4fb06e3b505e9c7261/llvm/lib/Target/AMDGPU/Utils/AMDGPUBaseInfo.h) and [MODE state analysis](https://github.com/llvm/llvm-project/blob/87f0227cb60147a26a1eeb4fb06e3b505e9c7261/llvm/lib/Target/AMDGPU/SIModeRegister.cpp). This contract does not establish arithmetic support for every initial mode or console-specific behavior.

`agc_shader_mode_register` decodes instruction words, builds the control-flow graph and checks production translation without a Vulkan device. It covers rounding subfield reads, masked constant writes, branches, unsupported runtime changes and diagnostics, plus mode propagation through full recompilation. The requirements remain active in Release; CTest bounds execution to 20 seconds.
