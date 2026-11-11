#!/usr/bin/env bash
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Generate an Xcode project for the iOS and iPadOS build in build-ios-xcode
# (never committed), for running on a device with Xcode's signing and for
# debugging. The everyday build is scripts/build.sh (Ninja and ccache), which
# Xcode's own build cannot share a compiler cache with.
set -euo pipefail

ios_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
repo_dir="$(cd -- "$ios_dir/../.." && pwd)"
sdk=iphoneos
team=""
build_dir="$repo_dir/build-ios-xcode"
open_project=0
multicast=0

usage() {
    cat <<'USAGE'
Usage: platforms/ios/scripts/xcode.sh [--device | --simulator] [--team ID]
                                      [--multicast-entitlement]
                                      [--build-dir DIR] [--open]

Generates build-ios-xcode/open_annihilation_ios.xcodeproj for devices
(--device, the default) or the simulator (--simulator), building the pinned
SDL and FreeType for that system image first when they are missing. Choose the
oa-game scheme and the Check configuration in Xcode. With --team, Xcode signs
the game automatically with that Apple developer team's identity and
provisioning profile.

  --device            Link against the device libraries (local/deps/ios-iphoneos)
  --simulator         Link against the simulator libraries (local/deps/ios-iphonesimulator)
  --team ID           The Apple developer team that signs the game
  --multicast-entitlement
                      Sign device builds with the multicast networking
                      entitlement LAN games need to find each other; the team
                      must have been granted it by Apple
  --build-dir DIR     Generate here instead of build-ios-xcode
  --open              Open the project in Xcode afterwards
  --help              Show this help
USAGE
}

require_value() {
    if [[ $# -lt 2 || -z "$2" ]]; then
        printf 'xcode.sh: %s requires a value\n' "$1" >&2
        exit 2
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help) usage; exit 0 ;;
        --device) sdk=iphoneos; shift ;;
        --simulator) sdk=iphonesimulator; shift ;;
        --team) require_value "$@"; team="$2"; shift 2 ;;
        --build-dir) require_value "$@"; build_dir="$2"; shift 2 ;;
        --open) open_project=1; shift ;;
        --multicast-entitlement) multicast=1; shift ;;
        *) printf 'xcode.sh: unknown option %s (see --help)\n' "$1" >&2; exit 2 ;;
    esac
done
case "$build_dir" in /*) ;; *) build_dir="$PWD/$build_dir" ;; esac

python3 "$ios_dir/tools/bootstrap_ios_deps.py" --sdk "$sdk"
python3 "$repo_dir/tools/bootstrap_accesskit.py"
configure_args=(
    -S "$ios_dir" -B "$build_dir" -G Xcode
    "-DCMAKE_TOOLCHAIN_FILE=$ios_dir/cmake/OaIosToolchain.cmake"
    "-DOA_IOS_SDK=$sdk"
)
if [[ -n "$team" ]]; then
    configure_args+=(
        "-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=$team"
        -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic
        "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY=Apple Development"
    )
fi
if [[ "$multicast" == 1 ]]; then
    configure_args+=(-DOA_IOS_MULTICAST_ENTITLEMENT=ON)
else
    configure_args+=(-DOA_IOS_MULTICAST_ENTITLEMENT=OFF)
fi
cmake "${configure_args[@]}"
project="$build_dir/open_annihilation_ios.xcodeproj"
printf 'Xcode project: %s\n' "$project"
if [[ "$open_project" == 1 ]]; then
    open "$project"
fi
