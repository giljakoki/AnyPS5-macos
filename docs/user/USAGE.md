# Relinker usage

## Input and conversion

Use a clean ELF executable. Place its bundled ELF modules in `sce_module/`, `sce_modules/`, or `prx/` beside the input executable, or under the parent directory selected with `--sce-module-path <path>`. The option accepts absolute paths and paths relative to the current working directory; do not include the `sce_module` directory itself. Only files directly inside the module directories are processed. A bundled module in a SELF container (encrypted) is not converted; the relinker warns about each one and says when the executable needs it. An executable in a SELF container is an error; a fake-signed one, as homebrew releases ship, has its segments unencrypted, and `python3 tools/unfself.py eboot.bin eboot.elf` unpacks it. `prx/` can coexist with either `sce_module/` or `sce_modules/`. Both `sce_module/` and `sce_modules/` present, or all three absent, is an error.

```text
source/
    input.elf
    sce_module/
        <bundled ELF modules>
```

```text
relinker [options] <input.elf> <output>
```

Linux output:

```sh
relinker source/input.elf app.elf
```

Windows output:

```sh
relinker --windows source/input.elf app.exe
```

macOS output (x86-64, runs under Rosetta on Apple silicon), packaged as an application that starts without the Vulkan SDK:

```sh
relinker --macos --to-rosetta source/input.elf out/eboot
python3 tools/package_macos_app.py --relinked out --game source --libs build/core/libs/libs --vulkan ~/VulkanSDK/<version>/macOS Title.app
```

The bundle holds the title, the prx libraries, the Vulkan loader and MoltenVK, and stays read-only: the title runs from `~/Library/Application Support/AnyPS5/<title id>`, where `app0` links to the bundle's game folder and the title's saves are written. The shader cache goes to `~/Library/Caches/<bundle id>` and the output to `~/Library/Logs/AnyPS5/<title id>.log`.

Add `--to-intel` for Intel hosts, or `--to-rosetta` for Rosetta; a title that runs under Rosetta on Apple silicon is relinked with `--macos --to-rosetta`. The output format defaults to Linux ELF regardless of the filename; `.exe` alone does not select Windows.

The executable, registry, icon and desktop entry output files must not refer to the input executable or a bundled module being converted, including through a hard link or symbolic link. These collisions fail before any output is written. An existing output file can be replaced if it is separate from those inputs.

## Options

All switches are disabled by default. `unused-filter` defaults to `0`; `--rpath` defaults to `$ORIGIN/libs`.

| Option                        | Effect                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
|-------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `--windows`                   | Produce a Windows PE executable.                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `--macos`                     | Produce an x86-64 Mach-O executable, with bundled modules as Mach-O libraries in `app0/sce_module`. Conflicts with `--windows`.                                                                                                                                                                                                                                                                                                                                             |
| `--windows-diagnostics`       | Include startup dependency diagnostics. Requires `--windows`.                                                                                                                                                                                                                                                                                                                                                                                                               |
| `--windows-gui`               | Select the Windows GUI subsystem instead of the console subsystem. Requires `--windows`.                                                                                                                                                                                                                                                                                                                                                                                    |
| `--to-intel`                  | Convert supported AMD-only instructions in the executable and bundled modules. Unsupported instructions or unreachable conversion stubs cause an error.                                                                                                                                                                                                                                                                                                                     |
| `--to-rosetta`                | Convert as `--to-intel` does and also lower what Rosetta lacks: RDSEED becomes RDRAND, CLWB a NOP, and RDPID returns processor 0. Implies `--to-intel`. Independent of `--macos`: `--macos` alone leaves these instructions as they are. Conflicts with `--windows`.                                                    |
| `unused-filter=0`             | Keep all imported NID references.                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| `unused-filter=1`             | Filter unused non-PLT imports using control-flow and GOT access analysis, treating address-taken code as reachable; preserve PLT imports.                                                                                                                                                                                                                                                                                                                                   |
| `unused-filter=2`             | Apply strict unused-import analysis and compact the PLT. Unsupported analysis cases cause an error.                                                                                                                                                                                                                                                                                                                                                                         |
| `--registry`                  | Write `<output-stem>.registry.json` beside the output executable, and `<output-stem>.<module>.registry.json` for each converted bundled module.                                                                                                                                                                                                                                                                                                                             |
| `--rpath <path>`              | Set the system library search path. Quote `$ORIGIN` to prevent shell expansion, for example `--rpath '$ORIGIN/libs'` in Bash or PowerShell. Linux guest modules require an absolute path or a path beginning with `$ORIGIN`. Windows requires a nonempty ASCII path and supports `$ORIGIN` as the executable directory.                                                                                                                                                     |
| `--autorun`                   | Run the output after conversion, print its exit code, and wait for Enter. Adds executable permissions for Linux output. Requires the target OS and prepared runtime layout.                                                                                                                                                                                                                                                                                                 |
| `--sce-module-path <path>`    | Parent of the module directories, not the module directory itself. Default: input ELF parent; relative input paths resolve from the process working directory. Explicit absolute path: used directly. Explicit relative path (including `.` and `..`): relative to the process working directory. Output path has no effect. Requires one nonempty value; repeats, nonexistent or nondirectory paths, access failures, and combination with `--skip-sce-module` are errors. |
| `--module-dir <dir>`          | Relink the ELF modules of a directory under the module parent directory, besides `sce_module`/`sce_modules`/`prx`; outputs keep the relative path. A module the executable or another module needs is initialized at startup, any other when the title loads it with `sceKernelLoadStartModule`, with its start arguments (for example Unity plugins in `Media/Plugins`). Repeat for multiple directories. Conflicts with `--skip-sce-module`.                              |
| `--skip-sce-module`           | Deprecated. Disable bundled module processing and module directory checks. Conflicts with explicit `--sce-module-path`.                                                                                                                                                                                                                                                                                                                                                     |
| `--exclude-sce-module <file>` | Deprecated. Exclude a bundled module by exact filename, not path. Repeat for multiple files; a missing filename is an error. Conflicts with `--skip-sce-module`.                                                                                                                                                                                                                                                                                                            |
| `--skip-syscall-check`        | Deprecated. Disable syscall scanning in the executable and bundled modules.                                                                                                                                                                                                                                                                                                                                                                                                 |
| `--trap-sdk-syscall-stubs`    | Replace the exact ps5-payload-dev generic and numbered syscall wrapper stubs with `UD2`, so unused wrappers do not block relinking. Other syscall instructions still cause an error. If a trapped wrapper runs, it raises an illegal-instruction exception. For debugging only.                                                                                                                                                                    |
| `--lazy-binding`              | Deprecated. Enable lazy symbol binding instead of eager binding. Incompatible with bundled ELF modules.                                                                                                                                                                                                                                                                                                                                                                     |

Specify `unused-filter=0|1|2` without `--`, at most once. Unknown options and extra positional arguments are errors. There is no `--help` flag; invoking `relinker` without arguments prints the usage syntax and exits with an error.

The `--skip-sce-module`, `--exclude-sce-module <file>`, `--skip-syscall-check`, and `--lazy-binding` flags are deprecated. If the application runs with these flags enabled, it will be extremely unstable and unsuitable for general use. These flags are only for debugging.

`--trap-sdk-syscall-stubs` is also for debugging. It preserves a runtime failure if a syscall wrapper is called; it does not implement FreeBSD syscalls or make these applications generally safe to run.

## Runtime layout

Paths are relative to the output executable:

```text
app.elf (Linux) or app.exe (Windows)
libs/
    *.prx
app0/
    <app resources>
    sce_module/
        <converted modules>
```

Use `sce_modules/` or `prx/` instead of `sce_module/` if that is the input directory name. Relinker preserves each module's directory under `app0/` and prints its exact path. Place app resources in `app0/` separately. Copy the built system libraries from `build/core/libs/libs/*.prx` into `libs/`; use libraries built for the target OS. A custom `--rpath` changes the system library location.

Use the generated files printed as `Guest module:` for bundled title modules. `libs/` is for AnyPS5 system libraries, not the original PS5 `.prx` files. Placing an original PS5 module in `libs/` on Windows makes Windows try to load it as a DLL and can fail with error 193 (not a valid Win32 application).

On Windows, a self-built `libs/` also needs `libgcc_s_seh-1.dll`, `libstdc++-6.dll` and `libwinpthread-1.dll` from the `mingw64/bin` directory of the toolchain that built the libraries; the release archives already contain them. The libraries are loaded without searching `PATH`, so a copy elsewhere on the system is not used, and a library that needs one of them fails with error 126 (the specified module could not be found) although its `.prx` file is present.

On Windows, direct memory (`sceKernelAllocateDirectMemory`, up to 13824 MiB per title) is committed in full when the title allocates it, not when its pages are first used. The system commit limit (installed memory plus page file size, the second value of Committed in Task Manager) must cover it together with all other committed memory. Otherwise the allocation throws `create direct memory backing of 0x<n> bytes (<m> MiB)` with the Windows error; enlarge the page file or close other applications.

On Linux, when the Vulkan driver imports dma-buf memory (not the NVIDIA proprietary driver), shared direct memory is imported through `/dev/udmabuf`, and the user who runs the game needs read-write access to it. Many distributions create it as `root:kvm` with mode `0660`: add the user to the `kvm` group and log in again (an ACL such as `setfacl -m u:$USER:rw /dev/udmabuf` lasts until the next reboot). Without access, startup prints `[gpu] open /dev/udmabuf: Permission denied`, these ranges are copied instead of imported, and GPU stores to them through FLAT/GLOBAL addresses fail with `BDA access failed`. Ranges above udmabuf's `size_limit_mb` (64 MiB by default) are copied as well.

Linux:

```sh
chmod +x app.elf
./app.elf
```

Windows PowerShell:

```powershell
.\app.exe
```

### Controller input for test runs

Set `APS5_PAD_INPUT_FILE` to an absolute path before launching the application to enable controller pulses from a local command file. Once the application is reading controller input, send a button with the helper from the repository root:

```sh
APS5_PAD_INPUT_FILE=/tmp/anyps5-pad ./app.elf
python3 tools/pad_input.py /tmp/anyps5-pad cross
python3 tools/pad_input.py /tmp/anyps5-pad down --hold-ms 500
python3 tools/pad_input.py /tmp/anyps5-pad release
```

Run the helper in a second terminal. Commands are polled at most every 100 ms during controller reads and add to physical controller input. A press lasts 300 ms by default, starting when the application reads it, and releases automatically; `--hold-ms` accepts 1–5000 ms. Wait for the application to observe a release before sending another press. The file holds only the latest command; rapid commands can replace one another. Commands left over when the application first reads the controller are ignored. Without the environment variable, file input is disabled.

### Fast presses at low frame rates

A title that reads the controller with `scePadReadState` sees one snapshot per frame, so at a low frame rate a tap shorter than a frame, a second tap of the same button, or a chord pressed and released between two frames would be lost or merged. Every button change waits until a controller read has shown it, and the reads show the changes in order, each one to the reads of a 10 ms window, so every reader of one frame sees the same buttons; a held or idle controller reads its live state without delay, and at most four changes wait. `APS5_PAD_NO_PRESS_LATCH=1` returns the bare snapshot instead. `APS5_TRACE_PAD=1` prints `[pad]` lines to stderr: each key event with how long it waited before the presenter took it from the window, each published button or left stick change, and the first read that shows it with the delay since it was published.

### System fonts

Games that open the console's system font sets (`sceFontOpenFontSet`) need font files in an `anyps5-fonts/` directory beside the output executable; set `ANYPS5_SYSTEM_FONTS` to use another directory. Files dumped from the console are used under their own names (`SST-Roman.otf`, `SST-Bold.otf`, `SSTJpPro-Regular.otf`, ...). Without them, these openly licensed substitutes are used when present: `NotoSans-{Light,Regular,Medium,Bold}.ttf` and `NotoSans-{LightItalic,Italic,MediumItalic,BoldItalic}.ttf` (Latin and Vietnamese), `NotoSansMono-{Light,Regular,Medium,Bold}.ttf` (typewriter), `NotoSansThai-{Light,Regular,Medium,Bold}.ttf` (Thai) and `NotoSansCJK-{Light,Regular,Medium,Bold}.ttc` (Japanese and Chinese). Without either, opening a system font set fails and the game shows no text in those fonts.

### Console language

Games ask for the console's language with `sceSystemServiceParamGetInt` and show their text in it when they have it. It is English (United States) unless `ANYPS5_LANGUAGE` is set to another console language number: 0 Japanese, 1 English (United States), 2 French, 3 Spanish, 4 German, 5 Italian, 6 Dutch, 7 Portuguese (Portugal), 8 Russian, 9 Korean, 10 Chinese (Traditional), 11 Chinese (Simplified), 12 Finnish, 13 Swedish, 14 Danish, 15 Norwegian, 16 Polish, 17 Portuguese (Brazil), 18 English (United Kingdom), 19 Turkish, 20 Spanish (Latin America), 21 Arabic, 22 French (Canada), 23 Czech, 24 Hungarian, 25 Greek, 26 Romanian, 27 Thai, 28 Vietnamese, 29 Indonesian, 30 Ukrainian. For example, `ANYPS5_LANGUAGE=21` selects Arabic. An empty value is the same as an unset one; any other value stops the game at its first language query with an error that names the variable.

### GPU selection

The game runs on the first Vulkan 1.1 device with graphics and compute queues and swapchain presentation, preferring a discrete GPU over an integrated one. Set `ANYPS5_GPU` to a part of a device name, compared without regard to case, to run on another device; the names are printed at start-up in the `Physical device candidate` lines. When no usable device contains the text, the start fails and the error lists the device names.

On Windows, the OpenCL, OpenGL, and Vulkan Compatibility Pack can install Mesa Dozen, which exposes D3D12 devices through Vulkan. Some D3D12 drivers can crash while the Vulkan loader enumerates adapters, even when AnyPS5 would select a native Vulkan device. If this happens and the system has another Vulkan driver, disable Dozen for the current PowerShell session before launching AnyPS5:

```powershell
$env:VK_LOADER_DRIVERS_DISABLE = '*dzn*'
.\app.exe
```

Do not disable Dozen if it is the only Vulkan driver available; the application would have no device to use. `VK_LOADER_DRIVERS_DISABLE` matches Vulkan driver manifest filenames and requires a Vulkan loader built with headers 1.3.234 or later ([loader documentation](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md#filter-environment-variables)).

### Storing GPU results at each flip

GPU results in storage images and render targets stay on the GPU until something reads their memory. A title that frees such an image after waiting for its GPU work and reuses the memory can have its new data overwritten when the results are stored later ([TechnicalDebt](../dev/TechnicalDebt.md)). `APS5_STORE_AT_FLIP=1` stores every pending result when a display buffer is flipped, at the cost of one write-back per pending image and frame. Any other value stops the title at its first flip.

### Barycentric registers reused as temporaries

A pixel shader that reuses its barycentric registers as temporaries in lanes a branch leaves inactive normally needs a geometry shader on devices without `VK_KHR_fragment_shader_barycentric`, which MoltenVK lacks, and stops with `Barycentric geometry SPIR-V: geometry shaders are unavailable`. `APS5_BARYCENTRIC_MASKED_ZERO=1` replaces those leftover barycentric values with 0 when they only reach inactive lanes, sample coordinates, arithmetic whose result those lanes never export, or cross-lane moves out of lanes that are not running; the only visible effect is the mip level of some pixels on the edge of partially covered quads ([TechnicalDebt](../dev/TechnicalDebt.md)). Any other value leaves it off.

### Slowing a title's loader

Some titles stream data into fixed-size staging pools that they recycle once per rendered frame and do not check for exhaustion. When the title renders more slowly than on the console (for example while it runs translated), its loader can fill a pool before a frame returns it, and the title crashes. `APS5_LOADER_THROTTLE_US=<n>` delays every file read (`sceKernelRead`, `sceKernelPread`, `sceKernelPreadv` and asynchronous reads) by n microseconds per MiB read during the first `APS5_LOADER_THROTTLE_SECONDS` seconds of the process (20 by default), which caps the read rate at about 1,000,000 / n MiB/s. Loading takes longer; values that are not non-negative integers stop the title at its first read ([TechnicalDebt](../dev/TechnicalDebt.md)).

`APS5_LOADER_MIB_PER_FLIP=<n>` instead ties reading to the title's frames for the whole run: from the title's first completed flip, every completed flip adds n MiB of read credit, at most two flips' worth is kept (idle frames do not bank more), and a file read that finds no credit left waits for the next flip completion. Only if no flip completes for 2 s (the render loop is itself waiting for the read) does the read go on, with the credit refilled. The title's renderer pools hold 128 MiB of small and 512 MiB of large blocks for two frames, so 4 is a safe value for it even when it renders at a few frames per second; larger values load faster and risk exhausting the pools. Both knobs can be combined; the log shows `[throttle] flip pacing on` at the first flip and `[throttle] ... flip pacing held` lines every 5 s while reads are held.

### Flip events before the first present

`APS5_EARLY_FIRST_FLIPS=1` reports the title's flips as completed as soon as the GPU has finished the work before them, until the first frame has actually been presented. Titles that free loading memory only on flip events then get it back during the window's first present instead of after it; one of the first frames may show later contents ([TechnicalDebt](../dev/TechnicalDebt.md)). Any other value leaves it off.

### Flips ahead of the GPU worker

A frame that ends with a flip is held at its submission until the GPU worker has reached the previous frame's flip, so a title never gets more than one frame ahead of what the worker has read ([TechnicalDebt](../dev/TechnicalDebt.md)). While the worker is slow, for example while it creates the pipelines of a title's first frames, a title that frees upload memory only from its rendering thread can run out of that memory and crash. `APS5_FLIPS_AHEAD=<n>` with n from 1 to 8 lets n frames wait for the worker before the next one is held (1 is the default). Values above 2 can show frames drawn with a later frame's data in titles that keep a three-frame ring of per-frame memory. Any other value stops the title at its first command buffer submission.

`APS5_PIPELINED_DRAWS=1` (or a queue depth up to 256) records queue 0's draws on a second thread while the GPU worker prepares the next ones. It is off by default. A packet the pipeline cannot order, or one that reads or writes memory a queued draw writes, waits until the second thread has recorded everything before it; labels, the end of a submission and the title's buffer fills also wait. `APS5_PIPELINE_ORDERED_ITEMS=1` queues those behind the draws instead, as upstream does (`APS5_PIPELINE_DRAIN_LABELS`, `APS5_PIPELINE_DRAIN_SUBMISSIONS` and `APS5_PIPELINE_DRAIN_FILLS` then make each of them wait again), and `APS5_PIPELINE_DMA=1` queues DMA_DATA stores and copies.

### Multisampled render targets

Multisampled (MSAA) color and depth targets are rendered as host multisampled images, and a CB resolve resolves them. A multisampled state those images do not model stops the title at its first such draw with a message that names `APS5_MSAA_SINGLE_SAMPLE` when the variable would accept the state, such as `AGC graphics: color targets with fewer fragments than samples (EQAA) are unsupported; APS5_MSAA_SINGLE_SAMPLE=1 renders multisampled targets with one sample`. `APS5_MSAA_SINGLE_SAMPLE=1` renders multisampled color and depth targets with one sample instead: edges are not anti-aliased, and a CB resolve becomes a copy. Multisampled textures, per-sample shading and the other cases listed in [TechnicalDebt](../dev/TechnicalDebt.md) still stop the title.

`APS5_TRACE_MSAA=1` prints `[msaa]` lines to stderr, each distinct one once: the register state of every draw that uses multisampling or a CB metadata pass, the descriptors of multisampled and FMASK textures with the address of the shader that reads them, printed before that shader is compiled, and uniform fills of up to 4 MiB, where CMASK, FMASK and HTILE clears show. The log of one run with both variables shows how a title uses multisampling and what it still needs. Any other value stops the title: for `APS5_MSAA_SINGLE_SAMPLE` at its first draw, for `APS5_TRACE_MSAA` at its first draw, compute dispatch or buffer fill.

### Devices without subgroup operations in vertex shaders

A device whose `Selected GPU` log line lacks the vertex bit in `stages=` (MoltenVK reports `stages=0x32`) runs vertex, local and tessellation-evaluation programs without subgroup operations. The recompiler then rewrites the wave-level constructs it can prove exact for each vertex on its own: the own-lane bit of masks built from compares, EXEC and `s_*_saveexec`; lane-index arithmetic whose result is the same for every lane of the wave the entry `s3` describes; `v_readfirstlane` and `v_readlane` of wave-uniform values; `s_cbranch_execz`, `execnz`, `vccz` and `vccnz` regions whose side effects are masked by EXEC and whose results merge back unchanged for the lanes that skip them; and waterfall loops (`v_readfirstlane` of a per-lane index, a compare against it that narrows EXEC, work under that mask, and an exit once no lane is left), which run once per vertex with its own index. Everything else stops the title with a message such as `ShaderRecompiler::Recompile: vertex program 0x… (wave64, s3 counts 32) needs subgroup operations, which the device does not support in vertex shaders (subgroup stages 0x32); exact lowering left: a wave-mask branch controls a loop at 0x…-0x…`, listing each construct that is left with its address range. Include that line, and the `shader_*.req` file `APS5_DUMP_SHADERS=1` writes for it, when reporting the title.

### Geometry programs on devices without mesh shaders

Titles draw geometry-shader and other NGG programs through the mesh path, which needs `VK_EXT_mesh_shader`. MoltenVK has no mesh shaders, so such a draw stops the title with `device does not support mesh shaders`, or earlier with `needs subgroup operations` when the program uses wave-wide operations. `APS5_MESH_EMULATION=1` runs these programs as a compute pre-pass instead: each draw records the program as a compute dispatch that writes its vertices and triangles into scratch buffers, then draws them with a generated vertex shader and the title's pixel shader under the draw's own render state ([GeometryEmulation](../dev/GeometryEmulation.md)). It is ignored on devices with mesh shaders. Every emulated draw ends the open render pass and starts a new one, which costs a store and reload of the color and depth targets on tile-based GPUs. Indirect mesh draws whose arguments are only on the GPU and indexed strips or fans with primitive restart still stop the title with a message naming `APS5_MESH_EMULATION`. Any other value leaves the emulation off.

`APS5_SKIP_FAILED_DRAWS=1` drops a draw or dispatch the driver cannot run (it throws while decoding the packet, preparing its shaders or building its pipeline), and ignores a failed static shader ABI resolution a guest thread requests, instead of stopping the title, and logs `[draw] skipped a draw the driver cannot run` (or `a dispatch`) with the reason once per reason; the frame then lacks that geometry ([TechnicalDebt](../dev/TechnicalDebt.md)). Any other value leaves it off.

The driver prints `[env] APS5 settings in effect: ...` once its Vulkan device is ready, listing every `APS5_` variable the title process received, so a launcher that drops the environment shows up in the log.

Debug aid: `APS5_SKIP_PROGRAMS=<hex>[,<hex>...]` drops every draw whose vertex, geometry or pixel program, and every dispatch whose compute program, starts at one of the listed addresses (as printed by `[recent-work]`), logging each address once; it bisects which program breaks a frame or the device, and the frame then lacks that work.

Debug aid: `APS5_SERIALIZE_WORK=1` waits for the GPU after every draw and dispatch packet, so a GPU fault surfaces right after the packet that caused it (combine it with `APS5_TRACE_RECENT_WORK`, whose last line is then that packet). It is very slow.

Debug aid: `APS5_TRACE_RECENT_WORK=<n>` keeps the last `n` draw and dispatch packets (queue, submission, program addresses, colour target, index base, the first packet words and, for each user-data pair that points at readable guest memory, the first four dwords the CPU sees there) and prints them as `[recent-work]` lines when the process terminates on an uncaught error, such as a lost Vulkan device, so the work submitted just before a GPU fault is known.

Debug aid: `APS5_TRACE_IMPORT_RETIRE=1` prints one `[import-retire]` line per retired host import (a guest range imported as a Vulkan buffer that the driver drops because the range was remapped, unmapped or merged with its neighbours): its range, buffer handle and serial, what retired it (a new import of its run, a registry reconcile or the pin waiter that runs before a guest remap or unmap), whether the retiring thread held the device lock and, when it did, the open batch, the batches in flight (and how many the GPU has not finished), the batches the submit thread has not handed to `vkQueueSubmit` yet and the objects carried into the next batch. On a failed recorder fence wait, a failed submit on the submit thread or an uncaught error it prints the last 64 retirements again, with their age, so a lost device can be matched to the imports dropped just before it. It also prints a line whenever a draw takes its in-place index, vertex or indirect buffers again because an import retired between their lookup and the draw's recording, and one per pin wait (the guest range, whether imports were retired and synced, and the objects still carried). Retirements are frequent while a title streams memory, so the log grows quickly. `APS5_TRACE_IMPORT_RETIRE=ring` keeps only the record and prints nothing until one of those failures, which costs far less.

On MoltenVK with an Apple GPU (every host-visible memory type coherent), a global memory barrier whose destination is a host read makes MoltenVK walk every live buffer and image to synchronize managed-storage resources, of which there are none; the driver leaves the host read out of such barriers' destination access and keeps the host stage (`APS5_KEEP_HOST_BARRIERS=1` passes them unchanged). With thousands of host imports live, the walk was most of Crash 4's `vkQueueSubmit` time.

Debug aid: `APS5_TRACE_GPU_FREES=1` records every `vkFreeMemory`, `vkDestroyBuffer`, `vkDestroyImage`, `vkDestroyImageView` and `vkDestroyBufferView` the driver makes, with the calling frames as module offsets, and prints the last 256 as `[gpu-free]` lines on a failed recorder fence wait, a failed submit on the submit thread or an uncaught error. A Metal `Invalid Resource` device loss means a command buffer used memory that was freed while it ran (remapping or unmapping the guest pages under a host import does not cause it), so these lines name the candidates. `APS5_DEFER_GPU_FREES=<ms>` makes each of those calls that many milliseconds after the driver releases the object instead of at once (all of a device's pending ones before it is destroyed): if a lost device goes away with it, an object was freed while the GPU still used it. `APS5_DEFER_GPU_FREES=batches:<n>` instead makes each call once the recorder's batch that was open when the driver released the object, and `n` batches after it, have completed. Deferred objects keep their memory until then.

Debug aid: `APS5_TRACE_STALE_HANDLES=1` follows every buffer, image, view, framebuffer and descriptor set the driver creates back to the memory it is bound to, and the memory every command buffer records (bound descriptor sets, vertex and index buffers, copies, fills, clears, barriers, indirect arguments, attachments) until its fence is seen signaled. It prints a `[stale]` line, with the calling frames as module offsets, when memory is freed while a recording, submitted or running command buffer still uses it, when a query pool is destroyed while a command buffer that resets, writes or copies it is pending, when a command records an object whose memory was already freed or an object already destroyed, and when a command buffer is submitted with such memory (the first 64). It also remembers the device address of every buffer the driver queried; once such a buffer's memory is freed it reports a command whose push constants or inline buffer update pass an address into it, and a submitted command buffer one of whose buffers holds such an address in its host-mapped contents (up to 4 MiB of each buffer), and prints the last 64 of those frees when the device is lost or the process terminates. Addresses that reach a shader any other way are not followed. It slows recording.

Guest memory the title maps for the GPU is imported as a host buffer in windows of `APS5_HOST_IMPORT_WINDOW_MIB` (8 by default): adjacent mappings inside one window are imported together once they fill it. A mapping that leaves its window partly unmapped is no longer imported when it is mapped but when work first uses it, so a level that streams its memory in 64 KiB pieces does not import and retire every piece; `APS5_EAGER_PARTIAL_IMPORTS=1` imports them at mapping time as before.

Debug aid: `APS5_VERIFY_MESH_PREPASS=<n>` checks what the pre-pass writes for the first `n` emulated draws. It fills the scratch buffers with a sentinel, waits for the pre-pass, and prints one `[mesh-verify]` line per draw: slots without a live triangle, live, degenerate and never-written triangles, indices outside their group slot, corners on records the program never wrote, non-finite positions or w <= 0, how many live triangles reach the view and the NDC range of the referenced positions. Each checked draw drains the GPU, so keep `n` small.

### Mixed-precision arithmetic on devices without 64-bit floats

Programs that use v_fma_mixlo_f16, v_fma_mixhi_f16 or the 16-bit interpolation instructions round their exact product-sum through 64-bit floats, so on a device without `shaderFloat64` (MoltenVK) they stop with `64-bit float instructions need the Float64 capability, which the device lacks`. `APS5_F64_AS_F32=1` computes them in 32-bit floats with error-free transformations instead; the result matches the hardware in round to nearest even except near the ends of the f32 range ([TechnicalDebt](../dev/TechnicalDebt.md)). Programs with real 64-bit arithmetic still stop. It is ignored on devices with `shaderFloat64`; any other value leaves it off.

## Exit codes

`0`: conversion succeeded. `1`: invalid arguments. `2`: conversion failed; the error is printed to stderr. With `--autorun`, successful conversion returns the launched application's exit code.

## Import audit

[`tools/import_audit.py`](../../tools/import_audit.py) lists the system functions a converted game imports and whether the built libraries provide them, before the game is launched. It needs Python 3 and nothing else.

```sh
relinker --registry source/input.elf app.elf
python3 tools/import_audit.py app.registry.json app.libc.prx.guest.prx.registry.json --libs build/core/libs/libs --modules source/sce_module
```

`--registry` writes `app.registry.json` for the executable and one `app.<module>.registry.json` for each converted bundled module; pass all of them. `--libs` is the directory the `libs` target fills ([build instructions](../dev/BUILD.md)); the exports are read from the built `.prx` files, so the result matches what the loader finds. Imports are counted once per NID and library, and every reference lands in exactly one class:

| Class | Meaning |
|-------|---------|
| `implemented` | A built library exports the NID and its function is not a throwing stub. |
| `stub` | Exported, but the whole function calls `NotImplemented_nid_no_patch`: the game loads and throws when it calls it. |
| `absent` | No built library exports the NID: the loader fails. |
| `module` | The import names a file found in `--modules` (the title's own library); its exports are not checked. |

The report also lists needed libraries that have no file in `--libs` or `--modules`, and imports exported only by a library other than the one they name; those resolve on Linux and can fail on Windows.

| Option | Effect |
|--------|--------|
| `--libs <dir>` | Built `.prx` directory. Required; repeat for several. |
| `--modules <dir>` | Directory of the title's own modules; repeat for several. |
| `--source <dir>` | `core/libs/prx` tree that is searched for throwing stubs. Defaults to this repository's. |
| `--names <file>` | NID and name per line, as in `aerolib.csv` from [`tools/nid_names.py`](../../tools/nid_names.py); names the absent imports. Never downloaded; a name that does not hash to its NID is marked. |
| `--json <file>` | Full result, stamped with the repository's commit and whether its tree was modified. |

Exit codes: `0` nothing blocks loading, `1` there are `absent` imports or needed libraries without a file, `2` an input could not be read; the message names the file and the value.

A module registry lists `nid`, `library` and `targetOffset` for each relocation against a function that no other bundled module exports, without the `#` suffix of the executable's NIDs.
