#!/usr/bin/env python3
from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LINKER = ROOT / "src/external/csoloader/src/linker.c"
CMAKE = ROOT / "src/CMakeLists.txt"


class TestAnonymousCSOLoader(unittest.TestCase):
    def test_pt_load_segments_are_copied_into_anonymous_memory(self) -> None:
        source = LINKER.read_text(encoding="utf-8")
        match = re.search(
            r"static int _linker_load_one_segment\(.*?\n\}\n",
            source,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(match)
        loader = match.group(0)

        self.assertIn("pread(fd", loader)
        self.assertIn("mprotect(", loader)
        self.assertNotIn("mmap(", loader)
        self.assertNotIn("MAP_FIXED", loader)

    def test_only_selective_dladdr_hook_is_enabled(self) -> None:
        cmake = CMAKE.read_text(encoding="utf-8")

        self.assertRegex(
            cmake,
            r"target_compile_definitions\(csoloader PRIVATE CSOLOADER_HOOK_DLADDR\)",
        )
        self.assertNotRegex(
            cmake,
            r"target_compile_definitions\([^)]*CSOLOADER_MAKE_LINKER_HOOKS",
        )


if __name__ == "__main__":
    unittest.main()
