#!/usr/bin/env python3
from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LINKER = ROOT / "src/external/csoloader/src/linker.c"
BACKTRACE_SUPPORT = (
    ROOT / "src/external/csoloader/src/backtrace-support.c"
)
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

    def test_only_local_libdl_compatibility_is_enabled(self) -> None:
        cmake = CMAKE.read_text(encoding="utf-8")

        self.assertRegex(
            cmake,
            r"target_compile_definitions\(csoloader PRIVATE CSOLOADER_HOOK_LIBDL\)",
        )
        self.assertNotRegex(
            cmake,
            r"target_compile_definitions\([^)]*CSOLOADER_MAKE_LINKER_HOOKS",
        )

    def test_all_gadget_libdl_imports_are_redirected(self) -> None:
        source = LINKER.read_text(encoding="utf-8")

        for name in (
            "dl_iterate_phdr",
            "dladdr",
            "dlopen",
            "dlsym",
            "dlerror",
            "dlclose",
        ):
            self.assertIn(f'if (strcmp(sym_name, "{name}") == 0)', source)
            self.assertIn(f"(void *)custom_{name}", source)

    def test_phdr_callbacks_run_outside_the_registry_lock(self) -> None:
        source = BACKTRACE_SUPPORT.read_text(encoding="utf-8")
        match = re.search(
            r"int custom_dl_iterate_phdr\(.*?\n\}\n",
            source,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(match)
        implementation = match.group(0)

        unlock = implementation.index(
            "pthread_mutex_unlock(&g_custom_libs_mutex)"
        )
        custom_callback = implementation.index(
            "result = callback(&snapshots[i].info"
        )
        self.assertLess(unlock, custom_callback)
        self.assertIn("snapshot->phdr", implementation)
        self.assertIn("snapshot->name", implementation)

    def test_custom_handles_and_thread_local_dlerror_are_present(self) -> None:
        source = BACKTRACE_SUPPORT.read_text(encoding="utf-8")

        self.assertIn("CUSTOM_HANDLE_MAGIC", source)
        self.assertIn("struct custom_lib_handle", source)
        self.assertIn("unsigned int refs", source)
        self.assertIn("scope_index", source)
        self.assertIn("active_refs", source)
        self.assertIn("static __thread bool g_custom_dlerror_pending", source)
        self.assertIn("custom_image_soname", source)
        self.assertIn("g_original_libdl_once", source)


if __name__ == "__main__":
    unittest.main()
