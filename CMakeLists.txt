cmake_minimum_required(VERSION 3.18.1)

project(zygisk_gadget LANGUAGES C CXX)

# Android/NDK build expects these to be set by the build script, but provide sane defaults.
set(MODULE_NAME "zygiskgadget" CACHE STRING "Zygisk module library name (without lib prefix)")
set(TOOL_NAME "zygisk-gadget" CACHE STRING "Tool executable name")
set(MODULE_DIR "zygisk_gadget" CACHE STRING "Magisk module directory name (/data/adb/modules/<MODULE_DIR>)")

add_subdirectory(src)

