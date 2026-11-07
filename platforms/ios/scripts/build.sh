#!/usr/bin/env bash
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Build Open Annihilation for the iOS simulator (the default) or for iPhone and
# iPad devices: builds the pinned SDL and FreeType for that system image when
# they are missing or out of date (platforms/ios/tools/bootstrap_ios_deps.py),
# configures platforms/ios in build-ios-sim (build-ios-device for devices)
# with Ninja and, when this machine has it, ccache, and builds the game's
# bundle, open-annihilation.app. Stops at the first failure.
set -euo pipefail

ios_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
repo_dir="$(cd -- "$ios_dir/../.." && pwd)"
sdk=iphonesimulator
team=""
build_dir=""
build_type="${OA_BUILD_TYPE:-Check}"
jobs="${OA_BUILD_JOBS:-3}"
print_bundle=0

usage() {
    cat <<'USAGE'
Usage: platforms/ios/scripts/build.sh [--simulator | --device [--team ID]]
                                      [--build-dir DIR] [--build-type TYPE]
                                      [--jobs N] [--print-bundle]

Builds open-annihilation.app for the iOS simulator on this Mac (--simulator,
the default; build tree build-ios-sim) or for iPhone and iPad devices
(--device; build tree build-ios-device). The pinned SDL and FreeType are built
for that system image first when they are missing or out of date, into
local/deps/ios-<sdk>. Stops at the first failure; never falls back to an
older bundle.

  --simulator         Build for the simulator (arm64, iOS 15.0 and later)
  --device            Build for devices (arm64, iOS 15.0 and later)
  --team ID           The Apple developer team that signs device builds made
                      in Xcode (scripts/xcode.sh); recorded in the build tree
  --build-dir DIR     Build here instead of build-ios-sim or build-ios-device
  --build-type TYPE   CMake build type (default: Check, or OA_BUILD_TYPE)
  --jobs N            Parallel build jobs (default: 3, or OA_BUILD_JOBS)
  --print-bundle      Print only the bundle's path when the build succeeds
  --help              Show this help
USAGE
}

require_value() {
    if [[ $# -lt 2 || -z "$2" ]]; then
        printf 'build.sh: %s requires a value\n' "$1" >&2
        exit 2
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help) usage; exit 0 ;;
        --simulator) sdk=iphonesimulator; shift ;;
        --device) sdk=iphoneos; shift ;;
        --team) require_value "$@"; team="$2"; shift 2 ;;
        --build-dir) require_value "$@"; build_dir="$2"; shift 2 ;;
        --build-type) require_value "$@"; build_type="$2"; shift 2 ;;
        --jobs) require_value "$@"; jobs="$2"; shift 2 ;;
        --print-bundle) print_bundle=1; shift ;;
        *) printf 'build.sh: unknown option %s (see --help)\n' "$1" >&2; exit 2 ;;
    esac
done
case "$jobs" in ''|*[!0-9]*|0) printf 'build.sh: --jobs must be a positive number\n' >&2; exit 2 ;; esac
if [[ -z "$build_dir" ]]; then
    if [[ "$sdk" == iphoneos ]]; then build_dir="$repo_dir/build-ios-device"; else build_dir="$repo_dir/build-ios-sim"; fi
fi
case "$build_dir" in /*) ;; *) build_dir="$PWD/$build_dir" ;; esac

# Messages go to the error stream when --print-bundle keeps the output for the path.
say() {
    if [[ "$print_bundle" == 1 ]]; then printf '%s\n' "$*" >&2; else printf '%s\n' "$*"; fi
}

# Every build tree of the checkout shares one compiler cache.
export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$repo_dir}"
export CCACHE_NOHASHDIR="${CCACHE_NOHASHDIR:-1}"

if [[ "$print_bundle" == 1 ]]; then
    python3 "$ios_dir/tools/bootstrap_ios_deps.py" --sdk "$sdk" --jobs "$jobs" >&2
    python3 "$repo_dir/tools/bootstrap_accesskit.py" >&2
else
    python3 "$ios_dir/tools/bootstrap_ios_deps.py" --sdk "$sdk" --jobs "$jobs"
    python3 "$repo_dir/tools/bootstrap_accesskit.py"
fi

configure_args=(
    -S "$ios_dir" -B "$build_dir" -G Ninja
    "-DCMAKE_BUILD_TYPE=$build_type"
    "-DCMAKE_TOOLCHAIN_FILE=$ios_dir/cmake/OaIosToolchain.cmake"
    "-DOA_IOS_SDK=$sdk"
)
if command -v ccache >/dev/null 2>&1; then
    configure_args+=(
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
        -DCMAKE_OBJC_COMPILER_LAUNCHER=ccache -DCMAKE_OBJCXX_COMPILER_LAUNCHER=ccache
    )
fi
if [[ -n "$team" ]]; then
    configure_args+=("-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=$team")
fi
# A tree is configured on first use, and again when a choice of this run
# differs from the one it holds; otherwise the build itself configures again
# whenever a build file changed.
configured_sdk=""
configured_type=""
if [[ -f "$build_dir/CMakeCache.txt" ]]; then
    configured_sdk="$(sed -n 's/^OA_IOS_SDK:[A-Z]*=//p' "$build_dir/CMakeCache.txt")"
    configured_type="$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$build_dir/CMakeCache.txt")"
fi
if [[ ! -f "$build_dir/build.ninja" || "$configured_sdk" != "$sdk" || "$configured_type" != "$build_type" \
      || -n "$team" ]]; then
    say "Configuring $build_dir for $sdk ($build_type)"
    if [[ "$print_bundle" == 1 ]]; then cmake "${configure_args[@]}" >&2; else cmake "${configure_args[@]}"; fi
fi

start=$SECONDS
say "Building open-annihilation.app for $sdk"
if [[ "$print_bundle" == 1 ]]; then
    cmake --build "$build_dir" --target oa-game --parallel "$jobs" >&2
else
    cmake --build "$build_dir" --target oa-game --parallel "$jobs"
fi
bundle="$build_dir/engine/open-annihilation.app"
if [[ ! -x "$bundle/open-annihilation" || ! -f "$bundle/Info.plist" ]]; then
    printf 'build.sh: the build made no complete bundle at %s\n' "$bundle" >&2
    exit 1
fi
if [[ "$print_bundle" == 1 ]]; then
    printf '%s\n' "$bundle"
else
    printf 'Built %s in %d s\n' "$bundle" "$((SECONDS - start))"
fi
