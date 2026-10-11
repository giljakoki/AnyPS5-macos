import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_macos_app


def inputs(root):
    relinked = root / "relinked"
    (relinked / "app0" / "sce_module").mkdir(parents=True)
    (relinked / "eboot").write_text('#!/bin/sh\npwd -P > "$HOME/where"\ncat app0/sce_sys/param.json > "$HOME/param"\ntest -d download0 && exit 7\nexit 1\n')
    (relinked / "app0" / "sce_module" / "libc.prx.guest.prx").write_bytes(b"module")
    game = root / "game"
    (game / "sce_sys").mkdir(parents=True)
    (game / "sce_sys" / "param.json").write_text(json.dumps({"titleId": "PPSA00000"}))
    libs = root / "libs"
    libs.mkdir()
    (libs / "libc.prx").write_bytes(b"prx")
    vulkan = root / "vulkan"
    (vulkan / "lib").mkdir(parents=True)
    (vulkan / "lib" / "libvulkan.1.dylib").write_bytes(b"loader")
    (vulkan / "lib" / "libMoltenVK.dylib").write_bytes(b"driver")
    (vulkan / "share" / "vulkan" / "icd.d").mkdir(parents=True)
    (vulkan / "share" / "vulkan" / "icd.d" / "MoltenVK_icd.json").write_text(json.dumps({"ICD": {"library_path": "x"}}))
    return relinked, game, libs, vulkan


def earlier_bundle(bundle):
    (bundle / "Contents").mkdir(parents=True)
    (bundle / "Contents" / "earlier").write_text("kept")


class PackageMacosAppTests(unittest.TestCase):
    def test_missing_param_json_is_reported_before_anything_is_written(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            relinked, game, libs, vulkan = inputs(root)
            (game / "sce_sys" / "param.json").unlink()
            bundle = root / "Title.app"
            earlier_bundle(bundle)

            with self.assertRaisesRegex(RuntimeError, "No sce_sys/param.json in .*game"):
                package_macos_app.package(relinked, game, libs, vulkan, None, bundle)

            self.assertEqual((bundle / "Contents" / "earlier").read_text(), "kept")
            self.assertEqual(sorted(path.name for path in root.iterdir()), ["Title.app", "game", "libs", "relinked", "vulkan"])

    def test_a_failure_while_packaging_leaves_no_half_built_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            relinked, game, libs, vulkan = inputs(root)
            (vulkan / "lib" / "libMoltenVK.dylib").unlink()
            bundle = root / "Title.app"
            earlier_bundle(bundle)

            with self.assertRaisesRegex(RuntimeError, "Missing libMoltenVK.dylib"):
                package_macos_app.package(relinked, game, libs, vulkan, None, bundle)

            self.assertEqual((bundle / "Contents" / "earlier").read_text(), "kept")
            self.assertFalse((root / "Title.app.partial").exists())

            (vulkan / "lib" / "libMoltenVK.dylib").write_bytes(b"driver")
            package_macos_app.package(relinked, game, libs, vulkan, None, bundle)
            self.assertFalse((bundle / "Contents" / "earlier").exists())
            self.assertTrue((bundle / "Contents" / "MacOS" / "launch").is_file())

    def test_guest_modules_resolve_from_the_executable_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            relinked, game, libs, vulkan = inputs(root)
            bundle = root / "Title.app"

            package_macos_app.package(relinked, game, libs, vulkan, None, bundle)

            module = bundle / "Contents" / "MacOS" / "app0" / "sce_module" / "libc.prx.guest.prx"
            self.assertEqual(module.read_bytes(), b"module")
            self.assertEqual(module.resolve(), (bundle / "Contents" / "Resources" / "game" / "app0" / "sce_module" / "libc.prx.guest.prx").resolve())
            self.assertTrue((bundle / "Contents" / "MacOS" / "app0").is_symlink())
            self.assertFalse((bundle / "Contents" / "MacOS" / "app0").readlink().is_absolute())

            home = root / "home"
            home.mkdir()
            launched = subprocess.run([str(bundle / "Contents" / "MacOS" / "launch")], env={**os.environ, "HOME": str(home)}, capture_output=True, timeout=30)
            self.assertEqual(launched.returncode, 7, launched.stderr)
            run = home / "Library" / "Application Support" / "AnyPS5" / "PPSA00000"
            self.assertEqual(Path((home / "where").read_text().strip()), run.resolve())
            self.assertEqual(json.loads((home / "param").read_text()), {"titleId": "PPSA00000"})
            self.assertEqual(len(list((bundle / "Contents" / "Resources" / "game").iterdir())), 1)


if __name__ == "__main__":
    unittest.main()
