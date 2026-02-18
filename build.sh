#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

usage() {
  cat <<'EOF'
Usage:
  ./build.sh --ndk <android_ndk_dir> [--cmake <cmake_bin>] [--build-type Release|Debug]
             [--gadget-fetch true|false] [--gadget-repo <owner/repo>] [--gadget-version <ver>] [--gadget-prefix <name>]

What it does (no Gradle / no Java):
  - Builds native outputs via CMake + NDK toolchain for 4 ABIs:
      - Zygisk module shared library: lib<module>.so
      - Tool executable: <tool>
  - Stages a Magisk module directory from template/magisk_module + compiled outputs
  - Generates module.prop (replaces fields from module.conf)
  - Optionally fetches gadget .so (4 ABIs) into build/gadgets/ and packages them into module root
  - Produces a release zip under out/

Requirements:
  - Android NDK (path provided via --ndk, or env ANDROID_NDK_HOME)
  - A CMake executable in PATH (or pass --cmake)
  - python3 (for module.prop generation + zipping)
EOF
}

die() {
  echo "[!] $*" >&2
  exit 1
}

info() {
  echo "[*] $*"
}

require_cmd() { command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"; }
require_dir() { [[ -d "$1" ]] || die "Missing directory: $1"; }
require_file() { [[ -f "$1" ]] || die "Missing file: $1"; }

parse_args() {
  NDK_DIR="${ANDROID_NDK_HOME:-}"
  CMAKE_BIN="cmake"
  BUILD_TYPE="Release"
  # Gadgets are typically provided manually in template/magisk_module/ as release assets.
  # Enable this only if you want the build to auto-download missing gadget .so files.
  GADGET_FETCH="false"
  GADGET_REPO="hackcatml/ajeossida"
  GADGET_VERSION="16.5.2"
  GADGET_PREFIX="ajeossida-gadget"
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h|--help)
        usage
        exit 0
        ;;
      --ndk)
        [[ $# -ge 2 ]] || die "--ndk requires a value"
        NDK_DIR="$2"
        shift 2
        ;;
      --cmake)
        [[ $# -ge 2 ]] || die "--cmake requires a value"
        CMAKE_BIN="$2"
        shift 2
        ;;
      --build-type)
        [[ $# -ge 2 ]] || die "--build-type requires a value"
        BUILD_TYPE="$2"
        shift 2
        ;;
      --gadget-fetch)
        [[ $# -ge 2 ]] || die "--gadget-fetch requires a value"
        GADGET_FETCH="$2"
        shift 2
        ;;
      --gadget-repo)
        [[ $# -ge 2 ]] || die "--gadget-repo requires a value"
        GADGET_REPO="$2"
        shift 2
        ;;
      --gadget-version)
        [[ $# -ge 2 ]] || die "--gadget-version requires a value"
        GADGET_VERSION="$2"
        shift 2
        ;;
      --gadget-prefix)
        [[ $# -ge 2 ]] || die "--gadget-prefix requires a value"
        GADGET_PREFIX="$2"
        shift 2
        ;;
      *)
        die "Unknown argument: $1 (use --help)"
        ;;
    esac
  done
}

main() {
  parse_args "$@"
  [[ -n "$NDK_DIR" ]] || die "Missing --ndk (or set ANDROID_NDK_HOME)"
  require_dir "$NDK_DIR"
  require_file "$NDK_DIR/build/cmake/android.toolchain.cmake"
  require_cmd python3
  require_cmd "$CMAKE_BIN"

  require_file "$ROOT_DIR/module.conf"
  # Parse module.conf safely (do NOT `source` it, as values may contain spaces).
  local conf_json
  conf_json="$(
    python3 - "$ROOT_DIR/module.conf" <<'PY'
import json, sys
path = sys.argv[1]
data = {}
for raw in open(path, "r", encoding="utf-8"):
    line = raw.strip()
    if not line or line.startswith("#"):
        continue
    if "=" not in line:
        continue
    k, v = line.split("=", 1)
    data[k.strip()] = v.strip()
print(json.dumps(data))
PY
  )"

  local module_lib module_id module_name tool_name author desc ver vercode
  module_lib="$(python3 -c 'import json,sys;print(json.load(sys.stdin).get("MODULE_LIBRARY_NAME","zygiskgadget"))' <<<"$conf_json")"
  module_id="$(python3 -c 'import json,sys;print(json.load(sys.stdin).get("MAGISK_MODULE_ID","zygisk_gadget"))' <<<"$conf_json")"
  module_name="$(python3 -c 'import json,sys;print(json.load(sys.stdin).get("MODULE_NAME","ZygiskGadget"))' <<<"$conf_json")"
  tool_name="$(python3 -c 'import json,sys;print(json.load(sys.stdin).get("TOOL_NAME","zygisk-gadget"))' <<<"$conf_json")"
  author="$(python3 -c 'import json,sys;print(json.load(sys.stdin).get("MODULE_AUTHOR",""))' <<<"$conf_json")"
  desc="$(python3 -c 'import json,sys;print(json.load(sys.stdin).get("MODULE_DESCRIPTION",""))' <<<"$conf_json")"
  ver="$(python3 -c 'import json,sys;print(json.load(sys.stdin).get("MODULE_VERSION","v0.0.0"))' <<<"$conf_json")"
  vercode="$(python3 -c 'import json,sys;print(json.load(sys.stdin).get("MODULE_VERSION_CODE","0"))' <<<"$conf_json")"

  local src_dir="$ROOT_DIR/src"
  require_dir "$src_dir"
  require_dir "$ROOT_DIR/template/magisk_module"

  info "Building ($BUILD_TYPE) with NDK: $NDK_DIR"
  local abis=(armeabi-v7a arm64-v8a x86 x86_64)
  for abi in "${abis[@]}"; do
    info "CMake build ABI=$abi"
    local bdir="$ROOT_DIR/build/ndk/$BUILD_TYPE/$abi"
    local outdir="$bdir/out"
    rm -rf "$bdir"
    mkdir -p "$outdir"

    "$CMAKE_BIN" -S "$ROOT_DIR" -B "$bdir" \
      -DCMAKE_TOOLCHAIN_FILE="$NDK_DIR/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$abi" \
      -DANDROID_PLATFORM=android-23 \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DMODULE_NAME="$module_lib" \
      -DMODULE_DIR="$module_id" \
      -DTOOL_NAME="$tool_name" \
      -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$outdir" \
      -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$outdir"

    "$CMAKE_BIN" --build "$bdir" -- -j"$(nproc || echo 4)"

    [[ -f "$outdir/lib${module_lib}.so" ]] || die "Missing module output: $outdir/lib${module_lib}.so"
    [[ -f "$outdir/${tool_name}" ]] || die "Missing tool output: $outdir/${tool_name}"
  done

  if [[ "$GADGET_FETCH" == "true" ]]; then
    info "Fetching gadget libraries into build/gadgets (repo=$GADGET_REPO version=$GADGET_VERSION)..."
    python3 - <<PY
import urllib.request
from pathlib import Path

root = Path(${ROOT_DIR@Q})
out = root / "build" / "gadgets"
out.mkdir(parents=True, exist_ok=True)

repo = ${GADGET_REPO@Q}
ver = ${GADGET_VERSION@Q}
prefix = ${GADGET_PREFIX@Q}
abis = ["arm","arm64","x86","x86_64"]
tags = [ver, "v"+ver]

def download(url, dest: Path):
    with urllib.request.urlopen(url, timeout=30) as r:
        dest.write_bytes(r.read())
    return dest.exists() and dest.stat().st_size > 0

for abi in abis:
    name = f"{prefix}-{ver}-android-{abi}.so"
    dest = out / name
    if dest.exists() and dest.stat().st_size > 0:
        continue
    ok = False
    for tag in tags:
        url = f"https://github.com/{repo}/releases/download/{tag}/{name}"
        try:
            if download(url, dest):
                ok = True
                break
        except Exception:
            pass
    if not ok:
        raise SystemExit(f"Failed to fetch {name} from {repo} tags {tags}.")
print(out)
PY
  fi

  local stage="$ROOT_DIR/out/magisk_module_release"
  rm -rf "$stage"
  mkdir -p "$stage"
  info "Staging template -> $stage"
  (cd "$ROOT_DIR/template/magisk_module" && tar cf - .) | (cd "$stage" && tar xf -)

  info "Generating module.prop"
  python3 - <<PY
from pathlib import Path
stage = Path(${stage@Q})
p = stage / "module.prop"
txt = p.read_text(encoding="utf-8")
txt = (txt.replace("\${id}", ${module_id@Q})
          .replace("\${name}", ${module_name@Q})
          .replace("\${version}", ${ver@Q})
          .replace("\${versionCode}", str(${vercode@Q}))
          .replace("\${author}", ${author@Q})
          .replace("\${description}", ${desc@Q}))
p.write_text(txt, encoding="utf-8")
PY

  for abi in "${abis[@]}"; do
    local outdir="$ROOT_DIR/build/ndk/$BUILD_TYPE/$abi/out"
    mkdir -p "$stage/tool/$abi"
    install -m 0755 "$outdir/$tool_name" "$stage/tool/$abi/$tool_name"

    mkdir -p "$stage/zygisk"
    local dst="$stage/zygisk/$abi.so"
    # Always take zygisk module .so from the compiled output.
    install -m 0644 "$outdir/lib${module_lib}.so" "$dst"
  done

  # Validate gadget libraries are present in module root (provided by template, or fetched if enabled).
  python3 - <<PY
from pathlib import Path
import urllib.request

stage = Path(${stage@Q})
fetch = ${GADGET_FETCH@Q}.lower() == "true"
repo = ${GADGET_REPO@Q}
ver = ${GADGET_VERSION@Q}
prefix = ${GADGET_PREFIX@Q}
abis = ["arm","arm64","x86","x86_64"]
tags = [ver, "v"+ver]

def has(path: Path) -> bool:
    return path.exists() and path.stat().st_size > 0

missing = []
for abi in abis:
    name = f"{prefix}-{ver}-android-{abi}.so"
    if not has(stage / name):
        missing.append(name)

if missing and fetch:
    cache = Path(${ROOT_DIR@Q}) / "build" / "gadgets"
    cache.mkdir(parents=True, exist_ok=True)

    def download(url, dest: Path):
        with urllib.request.urlopen(url, timeout=30) as r:
            dest.write_bytes(r.read())
        return has(dest)

    for name in missing:
        dest_cache = cache / name
        if not has(dest_cache):
            ok = False
            for tag in tags:
                url = f"https://github.com/{repo}/releases/download/{tag}/{name}"
                try:
                    if download(url, dest_cache):
                        ok = True
                        break
                except Exception:
                    pass
            if not ok:
                raise SystemExit(f"[!] Failed to fetch {name} from {repo} tags {tags}.")

        dest_stage = stage / name
        if not has(dest_stage):
            dest_stage.write_bytes(dest_cache.read_bytes())

    # re-check
    missing = []
    for abi in abis:
        name = f"{prefix}-{ver}-android-{abi}.so"
        if not has(stage / name):
            missing.append(name)

if missing:
    raise SystemExit(
        "[!] Missing gadget libraries in template/magisk_module/ (module root):\n  - "
        + "\n  - ".join(missing)
        + "\nFix: copy these files into template/magisk_module/, or re-run with --gadget-fetch true."
    )
print("[*] Gadget libraries OK")
PY

  chmod 0755 "$stage/service.sh" 2>/dev/null || true

  mkdir -p "$ROOT_DIR/out"
  local zip="$ROOT_DIR/out/${module_id//_/-}-${ver}-release.zip"
  info "Creating zip: $zip"
  python3 - <<PY
import os, stat, time, zipfile
from pathlib import Path

stage = Path(${stage@Q})
zip_path = Path(${zip@Q})

def add_file(zf: zipfile.ZipFile, path: Path, arc: str):
    st = path.stat()
    zi = zipfile.ZipInfo(arc, time.localtime(st.st_mtime)[:6])
    perms = stat.S_IMODE(st.st_mode)
    zi.external_attr = (perms & 0xFFFF) << 16
    with path.open("rb") as f:
        zf.writestr(zi, f.read(), compress_type=zipfile.ZIP_DEFLATED)

with zipfile.ZipFile(zip_path, "w") as zf:
    for p in sorted(stage.rglob("*")):
        arc = str(p.relative_to(stage)).replace(os.sep, "/")
        if p.is_dir():
            if not arc.endswith("/"):
                arc += "/"
            zf.writestr(arc, b"")
        else:
            add_file(zf, p, arc)
print(zip_path)
PY

  info "Done: $zip"
}

main "$@"

