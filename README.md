# zygisk-gadget
A zygisk module loads frida-gadget

# Usage
- **Install the release file and reboot**<br>
  `zygisk-gadget` tool will be placed in `/data/local/tmp/`<br>
```shell
/data/local/tmp/zygisk-gadget -h                                                                                       
Usage: ./zygisk-gadget -p <packageName> <option(s)>
 Options:
  -d, --delay <microseconds>             Delay in microseconds before loading frida-gadget
  -c, --config                           Activate config mode (default: false)
  -h, --help                             Show help
```

## Normal mode
Frida-gadget will be loaded when the target package is launched.<br>
Use `-d 0` when hooks must be installed before the first Activity lifecycle callbacks.<br>
e.g., `/data/local/tmp/zygisk-gadget -p com.android.chrome -d 0`

## Config file mode
This module supports a config file mode as described [here](https://frida.re/docs/gadget/)<br>
Create `frida-gadget.config` file in the module directory (`/data/adb/modules/zygisk_gadget`) and then use `zygisk-gadget` tool with the config option<br>
e.g., `/data/local/tmp/zygisk-gadget -p com.android.chrome -d 0 -c`

## Loading mechanism
The existing Zygisk module still owns the orchestration: it selects the target
process, asks its companion to copy the ABI-matched Gadget and optional config
into the app data directory, and starts loading from `postAppSpecialize` (either
immediately or after the configured delay).

Inside the target process, the vendored
[CSOLoader](https://github.com/ThePedroo/CSOLoader) baseline custom-links Frida
Gadget directly. Gadget is not loaded through xDL or the Android system
`dlopen`. Each Gadget `PT_LOAD` segment stays in anonymous memory: CSOLoader
copies file bytes with `pread` instead of creating file-backed mappings, so the
Gadget filename does not appear on those lines in `/proc/<pid>/maps`. This
integration is not ReZygisk's `ptrace`/monitor/`remote_csoloader` architecture;
ReZygisk is used only as a reference source.

The unsafe process-wide libdl/PHDR hook group remains disabled. During
relocation, only Gadget and its custom-linked dependencies have their
`dl_iterate_phdr`, `dladdr`, `dlopen`, `dlsym`, `dlerror`, and `dlclose`
imports redirected to a local compatibility layer. It supplies synthetic PHDR
and address information for anonymous images, stable handles for Gadget and
its custom-linked dependencies, and thread-local error reporting. A `dlopen`
request for any other library is forwarded to the Android system linker, and
system handles remain system handles. The rest of the process is unaffected.

On Android, Frida's internal module enumeration reads the system linker's
private module list and cannot see a custom-linked image. The Gadget-specific
load path therefore validates its final per-ABI constructor wrapper and invokes
the wrapped entry with the anonymous mapping range and config JSON explicitly.
Ordinary libraries loaded through `csoloader_load` keep their normal constructor
behavior; an unknown Gadget wrapper is rejected instead of being called.

CSOLoader's required `__tls_get_addr` hook and its constructor/relocation
handling remain active. The loader remains resident for the life of the process
so its TLS, compatibility handles, and mapping state stay valid.

Loading is strict: if CSOLoader fails, there is no fallback path. The Zygisk
module and CSOLoader state remain resident in the target process. Failed loads
retain the copied temporary files for diagnosis; after a successful load, the
module attempts to delete the copied Gadget and config (when present), and logs
any deletion failure.

# Clone
CSOLoader is vendored in the repository, so a normal clone contains all source
dependencies:

```bash
git clone <repository-url>
```

# Build and Flash
This project is a **pure NDK + CMake** build (no Gradle / no Java).

## Build release zip
- Install Android NDK and CMake.
- Provide gadget libraries in `template/magisk_module/`:
  - `ajeossida-gadget-16.5.2-android-arm.so`
  - `ajeossida-gadget-16.5.2-android-arm64.so`
  - `ajeossida-gadget-16.5.2-android-x86.so`
  - `ajeossida-gadget-16.5.2-android-x86_64.so`

`build.sh` only creates release archives. Before compiling any ABI, it requires
all tracked content to be clean, no non-ignored untracked top-level content,
and the `MODULE_VERSION` tag to exist both locally and at
<https://github.com/nlat31/zygisk-gadget.git>. The local tag, public tag, and
clean `HEAD` must all resolve to the same commit.

After committing the release source, create the matching tag and push both the
commit and tag. Then build:

```bash
./build.sh --ndk /path/to/android-ndk
```

Output:
- `out/*-release.zip`
- Each release zip includes the original MIT `LICENSE`,
  `THIRD_PARTY_LICENSES/AGPL-3.0.txt`,
  `THIRD_PARTY_LICENSES/CSOLoader-NOTICE.md`, and a generated
  `SOURCE_OFFER.txt`.

For development-only four-ABI validation, invoke CMake directly instead of
`build.sh`. For example:

```bash
for abi in armeabi-v7a arm64-v8a x86 x86_64; do
  cmake -S . -B "build/dev/$abi" \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/android-ndk/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-23 \
    -DCMAKE_BUILD_TYPE=Debug
  cmake --build "build/dev/$abi" -- -j"$(nproc)"
done
```

There is no bypass switch for the release-source checks; development builds
must not produce a file named as a release zip.

# License
The original zygisk-gadget source is licensed under the MIT License; see
[`LICENSE`](LICENSE).

This repository vendors
[CSOLoader](https://github.com/ThePedroo/CSOLoader), based on upstream commit
`4cf67b87a8d39e765073a63fea148e6d409e4554` with local integration changes.
CSOLoader is licensed under AGPLv3. See
[`THIRD_PARTY_LICENSES/CSOLoader-NOTICE.md`](THIRD_PARTY_LICENSES/CSOLoader-NOTICE.md)
and the accompanying
[`AGPLv3 text`](THIRD_PARTY_LICENSES/AGPL-3.0.txt).

Distributing a binary that statically links CSOLoader with this project creates
a combined work that must be distributed in compliance with AGPLv3, including
the applicable source-code and licensing requirements. Release archives carry
the license texts, third-party notice, and an immutable-commit source offer.
Creating one requires a clean `HEAD` whose matching local and public version
tags resolve to that same commit and no non-ignored untracked top-level
content.

# Credits
[CSOLoader](https://github.com/ThePedroo/CSOLoader)<br>
[ReZygisk](https://github.com/PerformanC/ReZygisk) (reference source)<br>
[Zygisk-Il2CppDumper](https://github.com/Perfare/Zygisk-Il2CppDumper)<br>
[Ajeossida](https://github.com/hackcatml/ajeossida)<br>
[json](https://github.com/nlohmann/json)
