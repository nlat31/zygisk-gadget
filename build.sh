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
  - Git, with a clean release commit whose local and public version tags match
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

release_fail() {
  die "$*
Release source validation failed. Clean all non-ignored changes, create tag '$RELEASE_VERSION' at HEAD, and push that tag to https://github.com/nlat31/zygisk-gadget.git before building a release zip."
}

validate_release_source() {
  RELEASE_VERSION="$1"
  local public_repo="https://github.com/nlat31/zygisk-gadget.git"
  local tag_ref="refs/tags/${RELEASE_VERSION}"
  local head_commit tag_commit top_status remote_tags
  local remote_direct="" remote_peeled="" remote_commit="" oid ref

  command -v git >/dev/null 2>&1 || release_fail "Git is required."
  git rev-parse --is-inside-work-tree >/dev/null 2>&1 \
    || release_fail "The source directory is not a Git worktree."

  top_status="$(git status --porcelain=v1 --untracked-files=normal)" \
    || release_fail "Could not inspect the top-level worktree."
  while IFS= read -r ref; do
    [[ "${ref:0:3}" == "?? " ]] || continue
    release_fail "Non-ignored untracked top-level content is present: ${ref:3}
Untracked source or package inputs can make the binary differ from the tagged source."
  done <<<"$top_status"

  git diff --quiet -- \
    || release_fail "Tracked unstaged changes are present."
  git diff --cached --quiet -- \
    || release_fail "Tracked staged changes are present."

  git show-ref --verify --quiet "$tag_ref" \
    || release_fail "Missing local tag '$RELEASE_VERSION' ($tag_ref)."
  tag_commit="$(git rev-parse "${tag_ref}^{commit}" 2>/dev/null)" \
    || release_fail "Local tag '$RELEASE_VERSION' does not resolve to a commit."
  head_commit="$(git rev-parse HEAD 2>/dev/null)" \
    || release_fail "HEAD does not resolve to a commit."
  [[ "$tag_commit" == "$head_commit" ]] \
    || release_fail "Local tag '$RELEASE_VERSION' resolves to $tag_commit, but HEAD is $head_commit."

  if ! remote_tags="$(git ls-remote --tags "$public_repo" \
      "$tag_ref" "${tag_ref}^{}" 2>&1)"; then
    release_fail "Could not query public tag '$RELEASE_VERSION' at $public_repo: $remote_tags"
  fi
  while IFS=$'\t ' read -r oid ref; do
    case "$ref" in
      "$tag_ref") remote_direct="$oid" ;;
      "${tag_ref}^{}") remote_peeled="$oid" ;;
    esac
  done <<<"$remote_tags"
  remote_commit="${remote_peeled:-$remote_direct}"
  [[ -n "$remote_commit" ]] \
    || release_fail "Public tag '$RELEASE_VERSION' was not found at $public_repo."
  [[ "$remote_commit" == "$head_commit" ]] \
    || release_fail "Public tag '$RELEASE_VERSION' resolves to $remote_commit, but HEAD is $head_commit."

  RELEASE_SOURCE_COMMIT="$head_commit"
  info "Release source verified: $RELEASE_VERSION at $RELEASE_SOURCE_COMMIT"
}

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

  [[ "$GADGET_REPO" == "hackcatml/ajeossida" ]] \
    || die "Release Gadget repository is pinned to hackcatml/ajeossida"
  [[ "$GADGET_VERSION" == "16.5.2" ]] \
    || die "Release Gadget version is pinned to 16.5.2"
  [[ "$GADGET_PREFIX" == "ajeossida-gadget" ]] \
    || die "Release Gadget prefix is pinned to ajeossida-gadget"
}

main() {
  parse_args "$@"
  [[ -n "$NDK_DIR" ]] || die "Missing --ndk (or set ANDROID_NDK_HOME)"
  require_dir "$NDK_DIR"
  require_file "$NDK_DIR/build/cmake/android.toolchain.cmake"
  require_cmd python3
  require_cmd "$CMAKE_BIN"

  require_file "$ROOT_DIR/module.conf"
  require_file "$ROOT_DIR/LICENSE"
  require_file "$ROOT_DIR/THIRD_PARTY_LICENSES/AGPL-3.0.txt"
  require_file "$ROOT_DIR/THIRD_PARTY_LICENSES/CSOLoader-NOTICE.md"
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

  validate_release_source "$ver"
  local source_commit="$RELEASE_SOURCE_COMMIT"
  local source_date_epoch
  source_date_epoch="$(git show -s --format=%ct "$source_commit")"

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
import hashlib
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
expected = {
    "arm": "d439c3086861a9bc05c7da4682526a2d5ba035354a4d932a7434b0459e5b4f53",
    "arm64": "2fcf74d2f7f40864dc7f2a4ec340f850317584f0df3051da3b96889ff82ddeb3",
    "x86": "b83b23c17d9a5bda614ca09157fd9d30daeb53793a7f9cbc00d8fa9ac30f5c4f",
    "x86_64": "6bac6c04884d5702e60025dede8b539ad73205e35b1ba8ad195b356ffb6620bc",
}

def valid(path: Path, digest: str) -> bool:
    return (path.exists()
            and hashlib.sha256(path.read_bytes()).hexdigest() == digest)

def download(url, dest: Path, digest: str):
    with urllib.request.urlopen(url, timeout=30) as r:
        data = r.read()
    if hashlib.sha256(data).hexdigest() != digest:
        return False
    dest.write_bytes(data)
    return True

for abi in abis:
    name = f"{prefix}-{ver}-android-{abi}.so"
    dest = out / name
    digest = expected[abi]
    if valid(dest, digest):
        continue
    ok = False
    for tag in tags:
        url = f"https://github.com/{repo}/releases/download/{tag}/{name}"
        try:
            if download(url, dest, digest):
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

  info "Staging licenses and source offer"
  install -m 0644 "$ROOT_DIR/LICENSE" "$stage/LICENSE"
  mkdir -p "$stage/THIRD_PARTY_LICENSES"
  install -m 0644 \
    "$ROOT_DIR/THIRD_PARTY_LICENSES/AGPL-3.0.txt" \
    "$stage/THIRD_PARTY_LICENSES/AGPL-3.0.txt"
  install -m 0644 \
    "$ROOT_DIR/THIRD_PARTY_LICENSES/CSOLoader-NOTICE.md" \
    "$stage/THIRD_PARTY_LICENSES/CSOLoader-NOTICE.md"
  cat >"$stage/SOURCE_OFFER.txt" <<EOF
SOURCE OFFER FOR ${ver}

The zygisk-gadget module binary in this archive combines the original
MIT-licensed zygisk-gadget code with statically linked CSOLoader code and is
distributed under the GNU Affero General Public License, version 3 (AGPLv3).

The complete Corresponding Source is the immutable zygisk-gadget commit:
  ${source_commit}

That commit is published under tag ${ver} at:
https://github.com/nlat31/zygisk-gadget.git

Retrieve it with:

  git clone https://github.com/nlat31/zygisk-gadget.git
  cd zygisk-gadget
  git checkout ${source_commit}

The vendored CSOLoader source is based on upstream commit
4cf67b87a8d39e765073a63fea148e6d409e4554, with this project's anonymous
PT_LOAD mapping, Gadget mapped-range entry, and local six-function libdl
compatibility changes included in the commit above.

The release build verified that local tag ${ver}, the public tag at the URL
above, and the source commit all resolve to ${source_commit}.
EOF

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
import hashlib
from pathlib import Path
import urllib.request

stage = Path(${stage@Q})
fetch = ${GADGET_FETCH@Q}.lower() == "true"
repo = ${GADGET_REPO@Q}
ver = ${GADGET_VERSION@Q}
prefix = ${GADGET_PREFIX@Q}
abis = ["arm","arm64","x86","x86_64"]
tags = [ver, "v"+ver]
expected = {
    "arm": "d439c3086861a9bc05c7da4682526a2d5ba035354a4d932a7434b0459e5b4f53",
    "arm64": "2fcf74d2f7f40864dc7f2a4ec340f850317584f0df3051da3b96889ff82ddeb3",
    "x86": "b83b23c17d9a5bda614ca09157fd9d30daeb53793a7f9cbc00d8fa9ac30f5c4f",
    "x86_64": "6bac6c04884d5702e60025dede8b539ad73205e35b1ba8ad195b356ffb6620bc",
}

def valid(path: Path, digest: str) -> bool:
    return (path.exists()
            and hashlib.sha256(path.read_bytes()).hexdigest() == digest)

missing = []
for abi in abis:
    name = f"{prefix}-{ver}-android-{abi}.so"
    if not valid(stage / name, expected[abi]):
        missing.append(name)

if missing and fetch:
    cache = Path(${ROOT_DIR@Q}) / "build" / "gadgets"
    cache.mkdir(parents=True, exist_ok=True)

    def download(url, dest: Path, digest: str):
        with urllib.request.urlopen(url, timeout=30) as r:
            data = r.read()
        if hashlib.sha256(data).hexdigest() != digest:
            return False
        dest.write_bytes(data)
        return True

    for name in missing:
        abi = name.removeprefix(f"{prefix}-{ver}-android-").removesuffix(".so")
        digest = expected[abi]
        dest_cache = cache / name
        if not valid(dest_cache, digest):
            ok = False
            for tag in tags:
                url = f"https://github.com/{repo}/releases/download/{tag}/{name}"
                try:
                    if download(url, dest_cache, digest):
                        ok = True
                        break
                except Exception:
                    pass
            if not ok:
                raise SystemExit(f"[!] Failed to fetch {name} from {repo} tags {tags}.")

        dest_stage = stage / name
        dest_stage.write_bytes(dest_cache.read_bytes())

    # re-check
    missing = []
    for abi in abis:
        name = f"{prefix}-{ver}-android-{abi}.so"
        if not valid(stage / name, expected[abi]):
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
timestamp = time.gmtime(max(int(${source_date_epoch@Q}), 315532800))[:6]

def add_file(zf: zipfile.ZipFile, path: Path, arc: str):
    st = path.stat()
    zi = zipfile.ZipInfo(arc, timestamp)
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
            zi = zipfile.ZipInfo(arc, timestamp)
            zi.external_attr = (0o755 & 0xFFFF) << 16
            zf.writestr(zi, b"")
        else:
            add_file(zf, p, arc)
print(zip_path)
PY

  info "Done: $zip"
}

main "$@"

