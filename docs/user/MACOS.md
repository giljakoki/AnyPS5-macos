# macOS on Apple silicon

This fork is [boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5) with the macOS runtime port from [mugurc/AnyPS5 `macos-port`](https://github.com/mugurc/AnyPS5/tree/macos-port) merged in, kept current with both by [Sync upstream](../../.github/workflows/sync-upstream.yml).

## How it runs

The relinker converts the title to an x86-64 Mach-O executable. The system libraries are built as x86-64 Mach-O libraries. Apple silicon (M1 to M5) runs both through Rosetta 2; GPU work goes through Vulkan and MoltenVK to Metal. Nothing is native arm64.

## Requirements

- An Apple silicon Mac. Prebuilt packages from CI need macOS 15 or newer.
- Rosetta 2: `softwareupdate --install-rosetta --agree-to-license`.
- The [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home#mac) for macOS. Its loader and MoltenVK are universal binaries. Homebrew's `vulkan-loader` and `molten-vk` are arm64 only and cannot be loaded by an x86-64 process.
- To build: Xcode command-line tools (`xcode-select --install`), CMake 3.22.1 or newer, Ninja and Python 3 (`brew install cmake ninja python`).

## Getting the binaries

### Prebuilt

Every push to `main` runs the [macOS workflow](../../.github/workflows/macos.yml). Open Actions > macOS > the latest successful run and download `anyps5-macos-x86_64`. It holds `relinker`, `libs/*.prx`, `package_macos_app.py` and these documents.

```sh
tar -xzf anyps5-macos-x86_64.tar.gz
xattr -dr com.apple.quarantine anyps5-macos-x86_64
```

The second command removes the download quarantine; without it macOS refuses to start the unsigned `relinker`.

### From source

```sh
git clone --recursive https://github.com/giljakoki/AnyPS5-macos
cd AnyPS5-macos
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --build build --target libs --parallel
```

The full build selects x86-64 itself; do not set `CMAKE_OSX_ARCHITECTURES`. The relinker is `build/core/relinker/relinker` and the libraries are in `build/core/libs/libs`. See [BUILD.md](../dev/BUILD.md) for the other options.

## Converting and running a title

The input is the same as on Linux and Windows: a decrypted ELF executable with its bundled modules, see [USAGE.md](USAGE.md#input-and-conversion).

```sh
relinker --macos --to-rosetta source/eboot.elf out/eboot
python3 package_macos_app.py --relinked out --game source --libs libs --vulkan ~/VulkanSDK/<version>/macOS Title.app
open Title.app
```

From a source build, the packager is `tools/package_macos_app.py` and `--libs` is `build/core/libs/libs`. `--to-rosetta` lowers instructions Rosetta does not run (RDSEED, CLWB, RDPID); titles for Apple silicon always need it.

The bundle contains the title, the libraries, the Vulkan loader and MoltenVK, and starts without the SDK. The title runs in `~/Library/Application Support/AnyPS5/<title id>`, where its saves are written. Shaders are cached in `~/Library/Caches/<bundle id>`; the first start of a title compiles them and is slower. Output goes to `~/Library/Logs/AnyPS5/<title id>.log`. [Input mapping](INPUT_MAPPING.md) applies unchanged; pass `--input-config anyps5-input.ini` to the packager to put a mapping file in the bundle.

## Limitations

- Metal has no 64-bit floats and no 64-bit atomics, and MoltenVK has no `VK_KHR_shader_clock`. Shaders that need them fail; the affected tests are listed in [technical debt](../dev/TechnicalDebt.md).
- An f16 multiply that should give -0.0 gives +0.0 under SPIRV-Cross; see [technical debt](../dev/TechnicalDebt.md).
- `sceKernelGetModuleInfoFromAddr`, `sceKernelGetModuleList` and `sceKernelGetModuleInfo` are not implemented on macOS.
- Every guest module keeps its own TLS block; initial-exec TLS access across modules fails the relink.
- Everything runs translated by Rosetta. Apple has said that Rosetta stays generally available through macOS 27 and is then cut back to a subset for older games; macOS 26.4 and newer show a notice when a Rosetta app starts. A native arm64 path does not exist yet (upstream pull request [#1835](https://github.com/boykopovar/AnyPS5/pull/1835) is an experimental start).
- Run the GPU tests one at a time with a timeout and without the flip and display tests, as [BUILD.md](../dev/BUILD.md) describes: a wedged GPU can stop WindowServer and restart the Mac.

## Keeping the fork current

- `main` is upstream `main` plus the macOS port and the fixes needed to build upstream's newer code on macOS.
- [Sync upstream](../../.github/workflows/sync-upstream.yml) merges upstream `main` and `macos-port` every 6 hours and starts the macOS build. A conflict, or a push GitHub refuses, opens an issue labeled `upstream-sync`.
- GitHub does not let the workflow's built-in token push changes to workflow files, which upstream makes often. Create a fine-grained personal access token for this repository with Contents and Workflows read and write access, and save it as the `SYNC_TOKEN` repository secret. Without it, such updates wait for **Sync fork** on the repository page.
- A failing macOS build on `main` opens an issue labeled `macos-build`; the next passing build closes it.
- Upstream maintenance workflows (labels, pull request intake, progress pages, conventions) run only in boykopovar/AnyPS5. `build.yml` runs here as upstream configured it.

## Credits

The macOS port is the work of mugurc and the contributors to the `macos-port` branch; AnyPS5 is the work of its upstream contributors. Licensed under the GNU General Public License version 2 only, as the project.
