#!/usr/bin/env python3
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CSOLOADER = ROOT / "src/external/csoloader"
FIXTURES = ROOT / "tests/fixtures"


class TestCSOLoaderLibdlRuntime(unittest.TestCase):
    def test_custom_loaded_library_gets_local_libdl_compatibility(self) -> None:
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.fail("A host C compiler is required for the libdl runtime test")

        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            library = temp / "libcsoloader-libdl-fixture.so"
            runner = temp / "csoloader-libdl-runner"

            fixture_build = subprocess.run(
                [
                    cc,
                    "-std=gnu11",
                    "-fPIC",
                    "-shared",
                    "-Wl,-soname,libcsoloader-compat-soname.so",
                    str(FIXTURES / "libdl_compat_fixture.c"),
                    "-ldl",
                    "-lm",
                    "-pthread",
                    "-o",
                    str(library),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                fixture_build.returncode,
                0,
                fixture_build.stdout + fixture_build.stderr,
            )

            runner_build = subprocess.run(
                [
                    cc,
                    "-std=gnu11",
                    "-D_GNU_SOURCE",
                    "-DCSOLOADER_DEBUG",
                    "-DCSOLOADER_HOOK_LIBDL",
                    "-DDT_RELRSZ=35",
                    "-DDT_RELR=36",
                    "-I",
                    str(CSOLOADER / "include"),
                    str(FIXTURES / "libdl_compat_runner.c"),
                    *(str(path) for path in sorted((CSOLOADER / "src").glob("*.c"))),
                    "-ldl",
                    "-lpthread",
                    "-o",
                    str(runner),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                runner_build.returncode,
                0,
                runner_build.stdout + runner_build.stderr,
            )

            result = subprocess.run(
                [str(runner), str(library)],
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertEqual(
                result.returncode,
                0,
                result.stdout + result.stderr,
            )


if __name__ == "__main__":
    unittest.main()
