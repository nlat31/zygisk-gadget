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
e.g., `/data/local/tmp/zygisk-gadget -p com.android.chrome -d 300000`

## Config file mode
This module supports a config file mode as described [here](https://frida.re/docs/gadget/)<br>
Create `frida-gadget.config` file in the module directory (`/data/adb/modules/zygisk_gadget`) and then use `zygisk-gadget` tool with the config option<br>
e.g., `/data/local/tmp/zygisk-gadget -p com.android.chrome -d 300000 -c`

# Build and Flash
This project is a **pure NDK + CMake** build (no Gradle / no Java).

## Build release zip
- Install Android NDK and CMake.
- Provide gadget libraries in `template/magisk_module/`:
  - `ajeossida-gadget-16.5.2-android-arm.so`
  - `ajeossida-gadget-16.5.2-android-arm64.so`
  - `ajeossida-gadget-16.5.2-android-x86.so`
  - `ajeossida-gadget-16.5.2-android-x86_64.so`

Build:

```bash
./build.sh --ndk /path/to/android-ndk
```

Output:
- `out/*-release.zip`

# Credits
[xDL](https://github.com/hexhacking/xDL)<br>
[Zygisk-Il2CppDumper](https://github.com/Perfare/Zygisk-Il2CppDumper)<br>
[Ajeossida](https://github.com/hackcatml/ajeossida)<br>
[json](https://github.com/nlohmann/json)