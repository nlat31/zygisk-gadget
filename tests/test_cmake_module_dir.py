#!/usr/bin/env python3
from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NDK_ENV = os.environ.get("ANDROID_NDK_HOME")
NDK = Path(NDK_ENV) if NDK_ENV else None


class TestCMakeModuleDirValidation(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        if not NDK_ENV:
            raise RuntimeError(
                "A valid ANDROID_NDK_HOME is required; the environment variable is not set"
            )
        if not NDK.is_dir():
            raise RuntimeError(
                f"A valid ANDROID_NDK_HOME is required; path does not exist: {NDK}"
            )
        toolchain = NDK / "build/cmake/android.toolchain.cmake"
        if not toolchain.is_file():
            raise RuntimeError(
                "A valid ANDROID_NDK_HOME is required; "
                f"toolchain file is missing: {toolchain}"
            )

    def _configure(self, module_dir: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as build_dir:
            return subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(ROOT),
                    "-B",
                    build_dir,
                    "-DMODULE_NAME=zygiskgadget",
                    "-DTOOL_NAME=zygisk-gadget",
                    f"-DMODULE_DIR={module_dir}",
                    f"-DCMAKE_TOOLCHAIN_FILE={NDK}/build/cmake/android.toolchain.cmake",
                    "-DANDROID_ABI=arm64-v8a",
                    "-DANDROID_PLATFORM=android-23",
                ],
                capture_output=True,
                text=True,
            )

    def test_valid_module_dir_configures(self) -> None:
        proc = self._configure("zygisk_gadget")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_unsafe_module_dirs_fail_configuration(self) -> None:
        for module_dir in (".", "..", "../x", "/abs", "-leading", "x/y", r"x\y", "x..y"):
            with self.subTest(module_dir=module_dir):
                proc = self._configure(module_dir)
                self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertIn("MODULE_DIR", proc.stdout + proc.stderr)


if __name__ == "__main__":
    unittest.main()
