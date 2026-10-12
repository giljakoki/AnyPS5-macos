# macOS on Apple silicon

This fork is [boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5) with the macOS runtime port from [mugurc/AnyPS5 `macos-port`](https://github.com/mugurc/AnyPS5/tree/macos-port) merged in, kept current with both by [Sync upstream](../../.github/workflows/sync-upstream.yml).

## How it runs

The relinker converts the title to an x86-64 Mach-O executable. The system libraries are built as x86-64 Mach-O libraries. Apple silicon (M1 to M5) runs both through Rosetta 2; GPU work goes through Vulkan and MoltenVK to Metal. Nothing is native arm64.

## Requirements

- An Apple silicon Mac. Prebuilt packages from CI need macOS 15 or newer.
- Rosetta 2: `softwareupdate --install-rosetta --agree-to-license`.
- For a source build: the [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home#mac) for macOS. Its loader and MoltenVK are universal binaries. Homebrew's `vulkan-loader` and `molten-vk` are arm64 only and cannot be loaded by an x86-64 process. The prebuilt package already contains both.
- To build: Xcode command-line tools (`xcode-select --install`), CMake 3.22.1 or newer, Ninja and Python 3 (`brew install cmake ninja python`).

## Getting the binaries

### Prebuilt

Every push to `main` runs the [macOS workflow](../../.github/workflows/macos.yml). Open Actions > macOS > the latest successful run and download, under Artifacts:

- `anyps5-macos-x86_64`: `relinker`, `libs/*.prx`, `vulkan/` (the Vulkan loader and MoltenVK), `package_macos_app.py` and these documents.
- `anyps5-breakout-demo`: `AnyPS5 Breakout.app`, a small homebrew title converted and packaged by the same tools.

GitHub wraps each artifact in a zip, which Safari may unpack by itself. Unpack the package and clear the download quarantine, without which macOS refuses to start the unsigned programs:

```sh
cd ~/Downloads
unzip anyps5-macos-x86_64.zip
tar -xzf anyps5-macos-x86_64.tar.gz
xattr -dr com.apple.quarantine anyps5-macos-x86_64
```

### From source

```sh
git clone --recursive https://github.com/giljakoki/AnyPS5-macos
cd AnyPS5-macos
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --build build --target libs --parallel
```

The full build selects x86-64 itself; do not set `CMAKE_OSX_ARCHITECTURES`. The relinker is `build/core/relinker/relinker` and the libraries are in `build/core/libs/libs`. See [BUILD.md](../dev/BUILD.md) for the other options.

## Testing without a game

Unpack `anyps5-breakout-demo.zip`, then double-click the `AnyPS5-Breakout.zip` inside it (Archive Utility keeps the bundle's links and permissions), and start the application:

```sh
cd ~/Downloads
xattr -dr com.apple.quarantine "AnyPS5 Breakout.app"
open "AnyPS5 Breakout.app"
```

A window with a Breakout game opens and plays a tone when the ball bounces. Left and Right move the paddle, Space (Cross) launches the ball and Escape (Options) quits; a controller works too. Without input the paddle plays by itself after three seconds. The demo goes through the relinker, Rosetta, the prx libraries, video out on Vulkan and MoltenVK, pad and audio out, as a title does. Its output is in `~/Library/Logs/AnyPS5/APS5TEST1.log`.

## Converting and running a title

The input is the same as on Linux and Windows: a decrypted ELF executable with its bundled modules, see [USAGE.md](USAGE.md#input-and-conversion).

```sh
relinker --macos --to-rosetta source/eboot.elf out/eboot
python3 package_macos_app.py --relinked out --game source --libs libs --vulkan ~/VulkanSDK/<version>/macOS Title.app
open Title.app
```

With the prebuilt package, run these commands inside `anyps5-macos-x86_64` and pass `--vulkan vulkan`. The package's `convert-ps5.sh` runs both steps for a title folder: `bash convert-ps5.sh <title folder> <AppName> [relinker options]`. It uses decrypted `<module>.elf` copies in place of the encrypted modules beside them, unpacks fake-signed executables and modules with `unfself.py`, adds `--module-dir` for a Unity title's `Media/Modules` and `Media/Plugins`, checks for `sce_sys/param.json`, and opens the app; `INPUT=<file>` adds a key mapping. From a source build, the packager is `tools/package_macos_app.py`, `--libs` is `build/core/libs/libs` and `--vulkan` is the SDK's `macOS` directory. `--to-rosetta` lowers instructions Rosetta does not run (RDSEED, CLWB, RDPID); titles for Apple silicon always need it.

The bundle contains the title, the libraries, the Vulkan loader and MoltenVK, and starts without the SDK. The title runs in `~/Library/Application Support/AnyPS5/<title id>`, where its saves are written. Shaders are cached in `~/Library/Caches/<bundle id>`; the first start of a title compiles them and is slower. Output goes to `~/Library/Logs/AnyPS5/<title id>.log`. [Input mapping](INPUT_MAPPING.md) applies unchanged; pass `--input-config anyps5-input.ini` to the packager to put a mapping file in the bundle.

## Tested on macOS

| Title | ID | Result | Mac |
|-------|----|--------|-----|
| DOOM Shareware (homebrew, [lowbit/ps5-doom](https://github.com/lowbit/ps5-doom) v1.1.1) | PPSA99666 | In game: stable frame rate, keyboard input without noticeable latency | M5 Pro, macOS package of `b11de50f` |

DOOM's release ships `eboot.bin` and `sce_module/libc.prx` as fake-signed SELF containers. Unpack the executable with `python3 tools/unfself.py eboot.bin eboot.elf`, set `eboot.bin` and the bundled `libc.prx` aside (the title runs on AnyPS5's libc, as on [Linux and Windows](COMPATIBILITY.md)), then convert `eboot.elf` as above.

The first commercial titles tried stop at their first draws on features MoltenVK lacks or the driver does not implement yet: 4x multisampled render targets, pixel shaders that need barycentrics without geometry shaders, a viewport depth range outside [0, 1] where the draw uses the depth, which needs `VK_EXT_depth_range_unrestricted`, and subgroup operations in vertex shaders.

MoltenVK does not have `VK_EXT_depth_range_unrestricted`. A draw whose viewport depth range lies outside [0, 1] still renders when its depth cannot be seen: it has no depth target, or it does not write depth and its depth test is off or always or never passes, and its pixel shader does not read the fragment depth. A depth bounds test does not count, since it compares the depth already in the target. Any other such draw stops with `viewport depth outside [0, 1] requires VK_EXT_depth_range_unrestricted`, followed by the draw's depth registers and depth state; include that line when reporting the title. `APS5_NO_DEPTH_RANGE_UNRESTRICTED=1` makes a Linux or Windows device behave the same way.

MoltenVK offers the depth bounds test only on GPUs of Metal's Apple10 family. On other Macs a draw that tests depth bounds stops with `device does not support the depth bounds test`, followed by the same depth registers and state. Where the test is available, depth bounds outside [0, 1] are narrowed to [0, 1], which gives the same result, but bounds that can never pass stop the title, including bounds inside [0, 1] whose minimum is above the maximum, which earlier versions passed to Metal unchanged: what Metal does with them is unknown. A depth clear to a value outside [0, 1] stops the title with `outside [0, 1] requires VK_EXT_depth_range_unrestricted`.

The driver does not use fragment shader barycentrics on MoltenVK, whose translation to Metal rejects per-vertex inputs, so pixel shaders interpolate through Metal's own interpolation. That covers `v_interp_p1_f32`/`v_interp_p2_f32` of an I/J pair that is unchanged or copied under the EXEC of the `v_interp_p2_f32`, and inputs read only as P0 through `v_interp_mov_f32`, which read the provoking vertex. A VGPR holding I or J that is overwritten under `s_wqm_b64` of the entry EXEC keeps nothing of it: whole-quad mode of the covered-pixel mask is every invocation the host runs, so the recompiler folds it to all lanes. Other patterns, such as P10/P20 reads, an input read both as P0 and interpolated, or arithmetic on the I/J VGPRs, need a geometry stage that Metal does not have: the draw stops with `geometry shaders are unavailable`, or with `needs a vertex shader before it` for rect-list, mesh and tessellation draws. The log then contains a `[recompile] pixel shader 0x… emulates barycentrics:` line that names the instruction, and the error names the draw's shaders, topology and colour target; include both when reporting the title.

The bundle's launcher passes its own environment to the title, so a runtime variable from [USAGE.md](USAGE.md#runtime-layout), such as [`APS5_MSAA_SINGLE_SAMPLE=1`](USAGE.md#multisampled-render-targets) for titles that stop on multisampled render targets, is set when the bundle is started while the title is not running:

```sh
open --env APS5_MSAA_SINGLE_SAMPLE=1 Title.app
APS5_MSAA_SINGLE_SAMPLE=1 Title.app/Contents/MacOS/launch
```

Repeat `--env` for each variable. The second form starts the launcher from Terminal and waits until the title exits; either way the output still goes to the log.

MoltenVK has no subgroup operations in vertex and tessellation-evaluation shaders (its `Selected GPU` log line shows `stages=0x32`: fragment, compute and tessellation control). The recompiler lowers what it can exactly to per-invocation code: lane masks that come from compares and EXEC, lane-index tests against the wave's vertex count, `v_readfirstlane` of wave-uniform values, and `s_cbranch_execz`/`vccz` regions whose effects are masked by EXEC. A program that still needs the wave, such as a waterfall loop over `v_readfirstlane` of a varying value, stops the title with `vertex program 0x… needs subgroup operations, which the device does not support in vertex shaders` followed by the constructs that are left and their addresses; include that line when reporting the title, see [USAGE.md](USAGE.md#devices-without-subgroup-operations-in-vertex-shaders).

## Limitations

- MoltenVK has no subgroup operations in vertex and tessellation-evaluation shaders; the recompiler lowers what it can exactly and stops the title on the rest, naming the constructs (see above).
- A GPU fill of host-imported memory is not visible to the CPU under MoltenVK (the import probe at device creation checks it and logs `a GPU fill of a host import is not visible to the CPU`), so DCC keys are stored from the CPU there, as `APS5_CPU_DCC_KEYS=1` does elsewhere. Without this every present showed the display buffer's DCC clear colour: a black screen with the title running.
- A rect-list draw whose vertex count is not a multiple of 3 is drawn as a patch list; Vulkan drops the trailing 1 or 2 vertices, but MoltenVK tessellates them as a patch with zeroed control points, which the rect-list hull shader then rejects (`rect-list requires finite nondegenerate axis-aligned positions`, every frame, with a black screen). The driver therefore leaves those vertices out of the draw itself.
- Metal has no 64-bit floats and no 64-bit atomics, and MoltenVK has no `VK_KHR_shader_clock`. Shaders that need them fail; the affected tests are listed in [technical debt](../dev/TechnicalDebt.md).
- An f16 multiply that should give -0.0 gives +0.0 under SPIRV-Cross; see [technical debt](../dev/TechnicalDebt.md).
- `sceKernelGetModuleInfoFromAddr`, `sceKernelGetModuleList` and `sceKernelGetModuleInfo` are not implemented on macOS.
- Every guest module keeps its own TLS block; initial-exec TLS access across modules fails the relink.
- Everything runs translated by Rosetta. Apple has said that Rosetta stays generally available through macOS 27 and is then cut back to a subset for older games; macOS 26.4 and newer show a notice when a Rosetta app starts. A native arm64 path does not exist yet (upstream pull request [#1835](https://github.com/boykopovar/AnyPS5/pull/1835) is an experimental start).
- Run the GPU tests one at a time with a timeout and without the flip and display tests, as [BUILD.md](../dev/BUILD.md) describes: a wedged GPU can stop WindowServer and restart the Mac.

## Keeping the fork current

- `main` is upstream `main` plus the macOS port and the fixes needed to build upstream's newer code on macOS.
- [Sync upstream](../../.github/workflows/sync-upstream.yml) merges upstream `main` and `macos-port` every 6 hours and starts the macOS build. A conflict, or a push GitHub refuses, opens an issue labeled `upstream-sync`; with issues turned off (as they are on a new fork) the sync run fails and its summary says why.
- GitHub does not let the workflow's built-in token push changes to workflow files, which upstream makes often. Create a fine-grained personal access token for this repository with Contents and Workflows read and write access, and save it as the `SYNC_TOKEN` repository secret. Without it, such updates wait for **Sync fork** on the repository page.
- A failing macOS build on `main` opens an issue labeled `macos-build` when issues are turned on; the next passing build closes it.
- The tests that measure time (`guest_avplayer`, `guest_audio3d_port`, `guest_raise_exception`) run in their own step. Timers on GitHub's hosted macOS runners end 8 ms or more late, more than these tests allow, so there a failure only adds a warning; on a self-hosted Mac it fails the build.
- Upstream maintenance workflows (labels, pull request intake, progress pages, conventions) run only in boykopovar/AnyPS5. `build.yml` runs here as upstream configured it.

## Credits

The macOS port is the work of mugurc and the contributors to the `macos-port` branch; AnyPS5 is the work of its upstream contributors. Licensed under the GNU General Public License version 2 only, as the project.
