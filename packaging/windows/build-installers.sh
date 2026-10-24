#!/usr/bin/env bash
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Build the four Windows installers with makensis, from package folders laid
# out the way the suite lays them out, and check the installers it wrote.
#
# Homebrew: brew install makensis (the formula is nsis; the program is
# makensis). Debian and Nix ship the same program as the nsis package.
# This script uses /opt/homebrew/bin/makensis when it is there, otherwise
# makensis on PATH. MAKENSIS overrides both.
#
# The folders hold the tree's LICENSE, ATTRIBUTIONS.md, licenses/,
# languages/interface.tdf, registries/ and a stand-in brains/classic.oabrain,
# plus stand-in executables and one stand-in font. makensis only packs those
# files; it does not run them.
# Pass a scratch directory to keep (it must contain G82). The default is
# $TMPDIR/G82-installers, and the script replaces that directory.
#
#   packaging/windows/build-installers.sh
#   packaging/windows/build-installers.sh /tmp/G82-installers
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
out=${1:-${TMPDIR:-/tmp}/G82-installers}
case "$out" in
    *G82*) ;;
    *) echo "G82: refusing to replace a directory whose path does not contain G82" >&2; exit 2 ;;
esac

if [[ -n "${MAKENSIS:-}" ]]; then
    makensis=$MAKENSIS
elif [[ -x /opt/homebrew/bin/makensis ]]; then
    makensis=/opt/homebrew/bin/makensis
elif command -v makensis >/dev/null 2>&1; then
    makensis=$(command -v makensis)
else
    echo "G82: makensis not found. Install it with: brew install makensis" >&2
    exit 1
fi

version=$(sed -n 's/^project(open_annihilation VERSION \([0-9.][0-9.]*\).*/\1/p' "$repo/CMakeLists.txt")
if [[ -z "$version" ]]; then
    echo "G82: no project version in CMakeLists.txt" >&2
    exit 1
fi

rm -rf "$out"
mkdir -p "$out"

fail() {
    echo "G82: $*" >&2
    exit 1
}

lay_out() {
    local dir=$1
    local with_intro=$2
    mkdir -p "$dir/licenses" "$dir/fonts" "$dir/languages" "$dir/registries" "$dir/brains"
    cp -p "$repo/LICENSE" "$repo/ATTRIBUTIONS.md" "$dir/"
    cp -p "$repo/licenses/"* "$dir/licenses/"
    cp -p "$repo/languages/interface.tdf" "$dir/languages/"
    cp -p "$repo/registries/"*.yaml "$dir/registries/"
    # The installer requires the file. The bytes are a stand-in; a real
    # package carries the build's classic.oabrain.
    printf 'oabrain' > "$dir/brains/classic.oabrain"
    printf 'MZ' > "$dir/fonts/DejaVuSans.ttf"
    printf 'MZ' > "$dir/open-annihilation.exe"
    printf 'MZ' > "$dir/oa-tool.exe"
    if [[ "$with_intro" == yes ]]; then
        printf 'MZ' > "$dir/oa-intro.exe"
    fi
}

compile_log() {
    local package=$1
    local dir=$2
    local log=$3
    shift 3
    # Extra makensis flags follow the log path.
    "$makensis" -WX "$@" \
        -DOA_PACKAGE="$package" \
        -DOA_PACKAGE_DIR="$dir" \
        -DOA_VERSION="$version" \
        "$repo/packaging/windows/open-annihilation.nsi" >"$log" 2>&1
}

expect_installer() {
    local package=$1
    local dir=$2
    local target=$3
    local log=$4
    local setup=$dir-setup.exe
    local base
    base=$(basename -- "$dir")
    [[ -f "$setup" ]] || fail "$package: missing $setup"
    [[ $(basename -- "$setup") == "open-annihilation-$version-$package-setup.exe" ]] \
        || fail "$package: setup name is $(basename -- "$setup")"
    [[ "$base" == "open-annihilation-$version-$package" ]] || fail "$package: folder name is $base"
    [[ ! -e "$dir/$(basename -- "$setup")" ]] || fail "$package: setup landed inside the package folder"
    local kind
    kind=$(file -b "$setup")
    [[ "$kind" == *PE32\ * ]] || fail "$package: not a PE32 executable ($kind)"
    [[ "$kind" != *PE32+* ]] || fail "$package: PE32+ executable ($kind)"
    grep -q "writing output ($target)" "$log" || fail "$package: log has no $target"
}

# One verbose compile shows the paths stored in the installer.
x64_dir=$out/open-annihilation-$version-windows-x64
lay_out "$x64_dir" yes
x64_log=$out/windows-x64.log
if ! compile_log windows-x64 "$x64_dir" "$x64_log" -V4; then
    cat "$x64_log" >&2
    fail "makensis failed for windows-x64"
fi
expect_installer windows-x64 "$x64_dir" x86-unicode "$x64_log"
# File /r stores each file's own name under the SetOutPath, so a license
# lands in $INSTDIR\licenses and not in $INSTDIR\licenses\licenses.
grep -q 'SetOutPath: "\$INSTDIR\\licenses"' "$x64_log" \
    || fail "windows-x64: licenses were not installed into the licenses folder"
grep -q 'File: "zlib-LICENSE.txt"' "$x64_log" \
    || fail "windows-x64: license file was not stored"
if grep -q 'SetOutPath: "\$INSTDIR\\licenses\\licenses"' "$x64_log"; then
    fail "windows-x64: licenses path is nested twice"
fi
grep -q 'SetOutPath: "\$INSTDIR\\languages"' "$x64_log" \
    || fail "windows-x64: languages folder was not set"
grep -q 'File: "interface.tdf"' "$x64_log" || fail "windows-x64: interface.tdf was not stored"
grep -q 'SetOutPath: "\$INSTDIR\\brains"' "$x64_log" \
    || fail "windows-x64: brains folder was not set"
grep -q 'File: "classic.oabrain"' "$x64_log" || fail "windows-x64: classic.oabrain was not stored"

build_one() {
    local package=$1
    local target=$2
    local dir=$out/open-annihilation-$version-$package
    local log=$out/$package.log
    lay_out "$dir" yes
    if ! compile_log "$package" "$dir" "$log"; then
        cat "$log" >&2
        fail "makensis failed for $package"
    fi
    expect_installer "$package" "$dir" "$target" "$log"
}

build_one windows-x86 x86-unicode
build_one windows-arm64 x86-unicode
build_one windows-95 x86-ansi

# A package that is not one of the four is refused before it reads files.
bad_log=$out/bad-package.log
if compile_log windows-ia64 "$x64_dir" "$bad_log"; then
    fail "unknown package was accepted"
fi
grep -q "OA_PACKAGE must be" "$bad_log" || fail "unknown package failed for another reason"

# A folder missing a notice is refused.
thin=$out/open-annihilation-$version-windows-x64-thin
lay_out "$thin" yes
rm -f "$thin/LICENSE"
thin_log=$out/thin.log
if compile_log windows-x64 "$thin" "$thin_log"; then
    fail "package without LICENSE was accepted"
fi
grep -q "missing LICENSE" "$thin_log" || fail "missing LICENSE failed for another reason"

# A folder missing Classic is refused.
nobrain=$out/open-annihilation-$version-windows-x64-nobrain
lay_out "$nobrain" yes
rm -f "$nobrain/brains/classic.oabrain"
nobrain_log=$out/nobrain.log
if compile_log windows-x64 "$nobrain" "$nobrain_log"; then
    fail "package without brains/classic.oabrain was accepted"
fi
grep -q "missing brains/classic.oabrain" "$nobrain_log" \
    || fail "missing Classic failed for another reason"

echo "G82: four installers built with $("$makensis" -VERSION) for $version"
echo "G82: $out"
