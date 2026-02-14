#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

usage() {
  cat <<'EOF'
Usage:
  ./build.sh [--java-home <jdk11_dir>] [--sdk <android_sdk_dir>]

What it does:
  - Builds :module release
  - Packages Magisk module zip (zipRelease)
  - Prints the output zip path

Notes:
  - This project uses AGP 4.2.2, which requires Gradle 6.7.1.
  - Gradle 6.7.1 requires Java <= 15 (JDK 11 recommended).
EOF
}

die() {
  echo "[!] $*" >&2
  exit 1
}

info() {
  echo "[*] $*"
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"
}

detect_java_major() {
  # outputs major version number, e.g. 11 / 17 / 21
  local v
  v="$(java -version 2>&1 | head -n 1 || true)"
  # matches:
  #   openjdk version "11.0.22" ...
  #   openjdk version "17.0.10" ...
  #   java version "1.8.0_..."
  if [[ "$v" =~ \"1\.([0-9]+)\. ]]; then
    echo "${BASH_REMATCH[1]}"
    return 0
  fi
  if [[ "$v" =~ \"([0-9]+)\. ]]; then
    echo "${BASH_REMATCH[1]}"
    return 0
  fi
  echo "0"
}

ensure_gradle_compatible_java() {
  require_cmd java

  local major
  major="$(detect_java_major)"
  if [[ "$major" -eq 0 ]]; then
    die "Unable to detect Java version (java -version)."
  fi

  # Gradle 6.7.1 supports up to Java 15.
  if [[ "$major" -le 15 ]]; then
    info "Java major version: $major (OK for Gradle 6.7.1)"
    return 0
  fi

  # Try to auto-pick a system JDK 11 if present.
  local candidates=(
    "/usr/lib/jvm/java-11-openjdk-amd64"
    "/usr/lib/jvm/java-11-openjdk"
    "/usr/lib/jvm/jdk-11"
    "/usr/lib/jvm/adoptopenjdk-11-hotspot-amd64"
    "/usr/lib/jvm/temurin-11-jdk-amd64"
  )

  for c in "${candidates[@]}"; do
    if [[ -x "$c/bin/java" ]]; then
      export JAVA_HOME="$c"
      export PATH="$JAVA_HOME/bin:$PATH"
      major="$(detect_java_major)"
      if [[ "$major" -le 15 ]]; then
        info "Switched to JAVA_HOME=$JAVA_HOME (Java $major)"
        return 0
      fi
    fi
  done

  local java_path
  java_path="$(command -v java || true)"
  die "Incompatible Java $major for Gradle 6.7.1 (current: $java_path). Please use JDK 11 (recommended) or <= 15, e.g. ./build.sh --java-home /path/to/jdk11"
}

ensure_android_sdk() {
  local sdk_dir=""

  if [[ -f "$ROOT_DIR/local.properties" ]]; then
    # parse sdk.dir=...
    sdk_dir="$(grep -E '^sdk\.dir=' "$ROOT_DIR/local.properties" | head -n 1 | cut -d= -f2- || true)"
  fi

  if [[ -z "${sdk_dir}" ]]; then
    sdk_dir="${ANDROID_SDK_ROOT:-}"
  fi

  if [[ -z "${sdk_dir}" ]]; then
    die "Android SDK not configured. Set ANDROID_SDK_ROOT, or create local.properties with sdk.dir=/path/to/Android/Sdk"
  fi

  if [[ ! -d "${sdk_dir}" ]]; then
    die "Android SDK directory not found: ${sdk_dir}"
  fi

  export ANDROID_SDK_ROOT="${sdk_dir}"
  info "ANDROID_SDK_ROOT=$ANDROID_SDK_ROOT"
}

check_sdk_components() {
  local cmake_ver="3.18.1"
  local ndk_ver="25.2.9519653"

  local cmake_bin="$ANDROID_SDK_ROOT/cmake/$cmake_ver/bin/cmake"
  local ndk_dir="$ANDROID_SDK_ROOT/ndk/$ndk_ver"

  if [[ ! -x "$cmake_bin" ]]; then
    die "CMake not found: $cmake_bin (install CMake $cmake_ver via SDK Manager)"
  fi
  if [[ ! -d "$ndk_dir" ]]; then
    die "NDK not found: $ndk_dir (install NDK $ndk_ver via SDK Manager)"
  fi
  info "Found CMake $cmake_ver and NDK $ndk_ver"
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h|--help)
        usage
        exit 0
        ;;
      --java-home)
        [[ $# -ge 2 ]] || die "--java-home requires a value"
        export JAVA_HOME="$2"
        export PATH="$JAVA_HOME/bin:$PATH"
        shift 2
        ;;
      --sdk|--android-sdk|--android-sdk-root)
        [[ $# -ge 2 ]] || die "--sdk requires a value"
        export ANDROID_SDK_ROOT="$2"
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
  ensure_gradle_compatible_java
  ensure_android_sdk
  check_sdk_components

  require_cmd bash
  [[ -x "$ROOT_DIR/gradlew" ]] || die "Missing ./gradlew"

  info "Building release and packaging Magisk module zip..."
  "$ROOT_DIR/gradlew" --no-daemon :module:zipRelease

  local out_dir="$ROOT_DIR/out"
  [[ -d "$out_dir" ]] || die "Expected output directory not found: $out_dir"

  local zip
  zip="$(ls -1t "$out_dir"/*-release.zip 2>/dev/null | head -n 1 || true)"
  if [[ -z "$zip" ]]; then
    die "Release zip not found in $out_dir (expected *-release.zip). Check Gradle output above."
  fi

  info "Done: $zip"
}

main "$@"

