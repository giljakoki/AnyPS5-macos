"""Packages a title relinked with `relinker --macos` as a macOS application bundle.

    python3 tools/package_macos_app.py --relinked <dir> [--game <dir>] --libs <dir> --vulkan <dir> [--input-config <file>] <Title.app>

--relinked is the relinker's output directory: the executable (eboot) and the app0/sce_module guest
modules it wrote. --game is the title's own folder (sce_sys and its data), copied as app0 under the
relinked modules. --libs is the patched prx directory (build/core/libs/libs) and --vulkan the macOS
directory of a Vulkan SDK, whose loader and MoltenVK go into the bundle.

The bundle runs without the SDK or environment variables:

    Contents/MacOS/launch           enters the run directory and starts eboot
    Contents/MacOS/eboot, libs/     the title, the prx libraries, libvulkan and MoltenVK
    Contents/MacOS/app0             a link to the title's files: the relinker names the guest modules
                                    @executable_path/app0/sce_module/..., and dyld resolves that here
    Contents/Resources/game/app0    the title's files
    Contents/Resources/vulkan/icd.d MoltenVK's driver manifest

On macOS the title's files are cloned into the bundle (APFS copy-on-write, no extra space), and the
quarantine attribute that downloaded files carry is removed, so Gatekeeper does not scan the copied
title before the first start.

AnyPS5 maps /app0 and the mount points under the working directory, so the bundle itself stays read-only: the
run directory is ~/Library/Application Support/AnyPS5/<title id>, where app0 is a link to the bundle's game
folder and download0 exists, and the title's saves are written. The shader cache goes to
~/Library/Caches/<bundle id> and the output to ~/Library/Logs/AnyPS5."""
import argparse
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

LAUNCHER = """#!/bin/sh
# Starts the title from a run directory next to the bundle's read-only game folder.
here="$(cd "$(dirname "$0")" && pwd)"
contents="$(cd "$here/.." && pwd)"
run="$HOME/Library/Application Support/AnyPS5/{title_id}"
export VK_DRIVER_FILES="${{VK_DRIVER_FILES:-$contents/Resources/vulkan/icd.d/MoltenVK_icd.json}}"
export ANYPS5_SHADER_CACHE_DIR="${{ANYPS5_SHADER_CACHE_DIR:-$HOME/Library/Caches/{identifier}/shader_cache}}"
mkdir -p "$ANYPS5_SHADER_CACHE_DIR" "$HOME/Library/Logs/AnyPS5" "$run/download0"
ln -sfn "$contents/Resources/game/app0" "$run/app0"
cd "$run" || exit 1
exec "$here/eboot" "$@" >> "$HOME/Library/Logs/AnyPS5/{title_id}.log" 2>&1
"""


def title_metadata(app0):
    parameters = json.loads((app0 / "sce_sys" / "param.json").read_text(encoding="utf-8"))
    title_id = parameters.get("titleId") or "UNKNOWN"
    localized = parameters.get("localizedParameters", {})
    language = localized.get("defaultLanguage") or parameters.get("defaultLanguage") or "en-US"
    name = (localized.get(language) or localized.get("en-US") or {}).get("titleName") or title_id
    version = parameters.get("contentVersion") or "1.0"
    return title_id, name, version


def make_icon(png, resources):
    """icon0.png as AppIcon.icns, or nothing when the title has none."""
    if not png.is_file():
        return None
    with tempfile.TemporaryDirectory() as scratch:
        iconset = Path(scratch) / "AppIcon.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
                subprocess.run(["sips", "-z", str(size * scale), str(size * scale), str(png), "--out", str(iconset / name)], check=True, capture_output=True)
        subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(resources / "AppIcon.icns")], check=True)
    return "AppIcon"


def package(relinked, game, libs, vulkan, input_config, bundle):
    """Builds the bundle next to its destination and moves it there only when it is complete, so a failure
    leaves no half-built application and keeps an earlier bundle of the same name."""
    executable = relinked / "eboot"
    if not executable.is_file():
        raise RuntimeError(f"No relinked executable: {executable}")
    if bundle.suffix != ".app":
        raise ValueError(f"The bundle name must end in .app: {bundle}")
    folders = [folder for folder in (game, relinked / "app0") if folder is not None]
    if not any((folder / "sce_sys" / "param.json").is_file() for folder in folders):
        raise RuntimeError(f"No sce_sys/param.json in {folders[0]}: the title's id and name come from it, and the title reads it "
                           "while it runs. Copy the complete sce_sys folder of the dump into the game folder.")
    staging = bundle.with_name(bundle.name + ".partial")
    if staging.exists():
        shutil.rmtree(staging)
    try:
        result = fill(executable, relinked, game, libs, vulkan, input_config, staging)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    if sys.platform == "darwin":
        subprocess.run(["xattr", "-dr", "com.apple.quarantine", str(staging)], capture_output=True)
    if bundle.exists():
        shutil.rmtree(bundle)
    staging.rename(bundle)
    return result


def copy_game(game, app0):
    """On macOS the title's files are cloned (APFS copy-on-write), so a title of tens of gigabytes takes
    no extra space and copies at once; elsewhere, or where cloning fails, they are copied."""
    if sys.platform == "darwin" and subprocess.run(["cp", "-cR", f"{game}/.", str(app0)], capture_output=True).returncode == 0:
        return
    shutil.copytree(game, app0, dirs_exist_ok=True)


def cap_unity_video_out(app0, width):
    """Unity titles fix their PS5 output size in boot.config (platform-ps5-video-out-width=3840 renders 4K).
    On a Mac that is the biggest cost there is, so cap it (1920 = 1080p). Returns the files changed.
    The bundle's files are APFS clones, so rewriting one leaves the dump untouched."""
    changed = []
    if width <= 0:
        return changed
    for config in sorted(app0.glob("**/boot.config")):
        lines = config.read_text(encoding="utf-8", errors="surrogateescape").splitlines(keepends=True)
        edited = False
        for index, line in enumerate(lines):
            # platform-ps5-video-out-width and the per-mode keys such as platform-ps5-standardmode-video-out-width.
            key, separator, value = line.partition("=")
            if not separator or not key.startswith("platform-ps5-") or not key.endswith("video-out-width"):
                continue
            value = value.strip()
            if value.isdigit() and int(value) > width:
                lines[index] = f"{key}={width}" + ("\n" if line.endswith("\n") else "")
                edited = True
        if edited:
            config.unlink()
            config.write_text("".join(lines), encoding="utf-8", errors="surrogateescape")
            changed.append(config)
    return changed


def fill(executable, relinked, game, libs, vulkan, input_config, bundle):
    contents = bundle / "Contents"
    macos = contents / "MacOS"
    resources = contents / "Resources"
    app0 = resources / "game" / "app0"
    (macos / "libs").mkdir(parents=True)
    app0.mkdir(parents=True)

    # The title's files first, then the guest modules the relinker wrote over them.
    if game is not None:
        copy_game(game, app0)
    if (relinked / "app0").is_dir():
        shutil.copytree(relinked / "app0", app0, dirs_exist_ok=True)
    for config in cap_unity_video_out(app0, int(os.environ.get("ANYPS5_UNITY_VIDEO_OUT_WIDTH", "1920"))):
        print(f"capped Unity output width in {config.relative_to(app0)}")
    title_id, name, version = title_metadata(app0)
    identifier = "org.anyps5." + "".join(character for character in title_id.lower() if character.isalnum())

    (macos / "app0").symlink_to(Path("..") / "Resources" / "game" / "app0")
    shutil.copy2(executable, macos / "eboot")
    (macos / "eboot").chmod(0o755)
    libraries = sorted(libs.glob("*.prx"))
    if not libraries:
        raise RuntimeError(f"No prx libraries in {libs}")
    for library in libraries:
        shutil.copy2(library, macos / "libs" / library.name)
    # libSceAgcDriver opens libvulkan.1.dylib through its @loader_path rpath, so both go into libs.
    for name_in_sdk in ("libvulkan.1.dylib", "libMoltenVK.dylib"):
        source = (vulkan / "lib" / name_in_sdk).resolve()
        if not source.is_file():
            raise RuntimeError(f"Missing {name_in_sdk} in {vulkan / 'lib'}")
        shutil.copy2(source, macos / "libs" / name_in_sdk)
    manifest = json.loads((vulkan / "share" / "vulkan" / "icd.d" / "MoltenVK_icd.json").read_text())
    manifest["ICD"]["library_path"] = "../../../MacOS/libs/libMoltenVK.dylib"
    (resources / "vulkan" / "icd.d").mkdir(parents=True)
    (resources / "vulkan" / "icd.d" / "MoltenVK_icd.json").write_text(json.dumps(manifest, indent=4) + "\n")
    # SDL looks for anyps5-input.ini in the bundle's Resources.
    if input_config is not None:
        shutil.copy2(input_config, resources / "anyps5-input.ini")

    launcher = macos / "launch"
    launcher.write_text(LAUNCHER.format(identifier=identifier, title_id=title_id))
    launcher.chmod(0o755)
    icon = make_icon(app0 / "sce_sys" / "icon0.png", resources)
    info = {
        "CFBundleName": name,
        "CFBundleDisplayName": name,
        "CFBundleIdentifier": identifier,
        "CFBundleExecutable": "launch",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "LSMinimumSystemVersion": "11.0",
        "LSApplicationCategoryType": "public.app-category.games",
        "NSHighResolutionCapable": True,
    }
    if icon is not None:
        info["CFBundleIconFile"] = icon
    with (contents / "Info.plist").open("wb") as file:
        plistlib.dump(info, file)
    return name, identifier


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--relinked", required=True, type=Path)
    parser.add_argument("--game", type=Path)
    parser.add_argument("--libs", required=True, type=Path)
    parser.add_argument("--vulkan", required=True, type=Path)
    parser.add_argument("--input-config", type=Path)
    parser.add_argument("bundle", type=Path)
    arguments = parser.parse_args()
    try:
        name, identifier = package(arguments.relinked, arguments.game, arguments.libs, arguments.vulkan, arguments.input_config, arguments.bundle)
    except (RuntimeError, ValueError) as error:
        raise SystemExit(f"error: {error}")
    print(f"{arguments.bundle}: {name} ({identifier})")


if __name__ == "__main__":
    main()
