#!/usr/bin/env python3
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NDK_ENV = os.environ.get("ANDROID_NDK_HOME")
NDK = Path(NDK_ENV) if NDK_ENV else None


class TestCMakeCSOLoaderPolicy(unittest.TestCase):
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

    def _configure(self, policy_version: str | None) -> subprocess.CompletedProcess[str]:
        if policy_version is None:
            scenario_setup = ""
            scenario_check = textwrap.dedent(
                """
                if (DEFINED CMAKE_POLICY_VERSION_MINIMUM)
                    message(FATAL_ERROR
                        "CMAKE_POLICY_VERSION_MINIMUM became defined: "
                        "${CMAKE_POLICY_VERSION_MINIMUM}")
                endif ()
                """
            )
        else:
            scenario_setup = (
                f'set(CMAKE_POLICY_VERSION_MINIMUM "{policy_version}")'
            )
            scenario_check = textwrap.dedent(
                f"""
                if (NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM
                    OR NOT CMAKE_POLICY_VERSION_MINIMUM STREQUAL "{policy_version}")
                    message(FATAL_ERROR
                        "CMAKE_POLICY_VERSION_MINIMUM changed from {policy_version} to "
                        "${{CMAKE_POLICY_VERSION_MINIMUM}}")
                endif ()
                """
            )

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            source_dir = temp_path / "wrapper"
            build_dir = temp_path / "build"
            source_dir.mkdir()
            wrapper_logcat_template = source_dir / "src/include/logcat.h.in"
            wrapper_logcat_template.parent.mkdir(parents=True)
            shutil.copyfile(
                ROOT / "src/include/logcat.h.in",
                wrapper_logcat_template,
            )
            (source_dir / "CMakeLists.txt").write_text(
                textwrap.dedent(
                    f"""
                    cmake_minimum_required(VERSION 3.18)
                    project(csoloader_policy_wrapper LANGUAGES C CXX)

                    {scenario_setup}
                    set(MODULE_NAME zygiskgadget)
                    set(TOOL_NAME zygisk-gadget)
                    set(MODULE_DIR zygisk_gadget)
                    add_subdirectory("{(ROOT / "src").as_posix()}" module)

                    {scenario_check}
                    """
                ),
                encoding="utf-8",
            )
            return subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(source_dir),
                    "-B",
                    str(build_dir),
                    f"-DCMAKE_TOOLCHAIN_FILE={NDK}/build/cmake/android.toolchain.cmake",
                    "-DANDROID_ABI=arm64-v8a",
                    "-DANDROID_PLATFORM=android-23",
                ],
                capture_output=True,
                text=True,
            )

    def test_undefined_policy_minimum_remains_undefined(self) -> None:
        proc = self._configure(None)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_policy_minimum_3_4_remains_3_4(self) -> None:
        proc = self._configure("3.4")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_policy_minimum_3_10_remains_3_10(self) -> None:
        proc = self._configure("3.10")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)


if __name__ == "__main__":
    unittest.main()
