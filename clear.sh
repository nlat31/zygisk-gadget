#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

usage() {
  cat <<'EOF'
Usage:
  ./clear.sh [--dry-run]

What it does:
  - Removes all generated build artifacts and intermediate files produced by this repo's build scripts.
  - Safe: only deletes known paths under the repo root.

Examples:
  ./clear.sh
  ./clear.sh --dry-run
EOF
}

DRY_RUN="false"
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --dry-run) DRY_RUN="true"; shift 1 ;;
    *) echo "[!] Unknown argument: $1 (use --help)" >&2; exit 1 ;;
  esac
done

info() { echo "[*] $*"; }
rm_path() {
  local p="$1"
  [[ -e "$p" ]] || return 0
  if [[ "$DRY_RUN" == "true" ]]; then
    info "Would remove: $p"
  else
    info "Removing: $p"
    rm -rf --one-file-system -- "$p"
  fi
}

# Primary outputs used by build.sh
rm_path "$ROOT_DIR/build"
rm_path "$ROOT_DIR/out"

# Common local CMake build directories users may create manually.
for d in "$ROOT_DIR"/cmake-build-*; do
  [[ -e "$d" ]] || break
  rm_path "$d"
done

# CMake cache/artifacts in repo root (rare, but clean them if present).
rm_path "$ROOT_DIR/CMakeCache.txt"
rm_path "$ROOT_DIR/CMakeFiles"
rm_path "$ROOT_DIR/Makefile"
rm_path "$ROOT_DIR/cmake_install.cmake"
rm_path "$ROOT_DIR/compile_commands.json"

# Android Studio/Gradle leftovers (shouldn't exist anymore, but safe to clean).
rm_path "$ROOT_DIR/.cxx"
rm_path "$ROOT_DIR/.externalNativeBuild"
rm_path "$ROOT_DIR/.gradle"

info "Done."

