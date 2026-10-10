#!/usr/bin/env bash
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Build the macOS release of Open Annihilation on a Mac and package it: a zip
# that holds the application and the notices, and an installer package that
# puts the application in /Applications. With a Developer ID and notary
# credentials in the environment (docs/development/releasing.md) both are signed,
# notarized and stapled; without them the application is signed ad hoc and
# the installer package is left unsigned, which is for testing only.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# The oldest macOS release the package runs on and the architectures it runs
# natively on. The dependencies and the game are built for them, and the
# checks hold the build to them.
macos_minimum=11.0
architectures=(arm64 x86_64)
build_dir="$repo_dir/build-release-macos"
deps_dir="$repo_dir/local/deps"
out_dir=""
version=""
jobs=6
app=""
unsigned=0
allow_changes=0
game_dir=""
save_file=""
previous_zip=""

usage() {
    cat <<'USAGE'
Usage: tools/release_macos.sh [--version vX.Y.Z] [--out DIR] [--build-dir DIR]
                              [--deps DIR] [--jobs N] [--allow-local-changes]
                              [--app BUNDLE] [--unsigned]
                              [--game-dir DIR [--save FILE]] [--previous-zip ZIP]

Builds the macOS release of Open Annihilation and writes, in DIR:

  open-annihilation-VERSION-macos-universal.zip  the folder of that name,
      holding Open Annihilation.app, LICENSE, ATTRIBUTIONS.md and licenses/
  open-annihilation-VERSION-macos-universal.pkg  an installer package that
      puts Open Annihilation.app in /Applications
  open-annihilation-VERSION-macos-universal-symbols/open-annihilation  the
      game's executable as the build made it, with the symbols the packaged
      one is stripped of, for reading a crash's addresses

The game is a Release build for arm64 and x86_64 and macOS 11.0 or later,
without tests or the game's self-checks (OA_SELF_CHECKS=OFF), with its
executable stripped of its symbols, with zlib, SDL3 and FreeType linked in from
tools/bootstrap_macos_deps.py, and the text fonts of
tools/bootstrap_text_fonts.py in its Resources/fonts. The application is signed inside-out with the
hardened runtime, notarized and stapled, and so is the installer package;
then both are checked. Nothing is installed.

  --version V            the release's version, vX.Y.Z or X.Y.Z: the
                         project's VERSION (the default), which the
                         application must carry
  --out DIR              where the packages go (default: BUILD_DIR/package);
                         DIR/work keeps the staged files and the notary logs
  --build-dir DIR        the build tree (default: build-release-macos)
  --deps DIR             the dependencies' archive, source and library cache
                         (default: local/deps)
  --jobs N               parallel build jobs (default: 6)
  --allow-local-changes  build a tree with uncommitted changes, for a test:
                         the packages are then never for release
  --app BUNDLE           package this built open-annihilation.app instead of
                         building one, without the release build's checks,
                         for testing the packaging: the packages are then
                         never for release
  --unsigned             sign ad hoc and leave the installer package unsigned,
                         whatever the environment says
  --game-dir DIR         start the packaged game headless over this 3.1c
                         installation, under each architecture this Mac runs
  --save FILE            with --game-dir: load this saved game under each of
                         them, play 1000 ticks and compare the final digests
  --previous-zip ZIP     compare the zip's file list with an earlier release's
  -h, --help             show this help

Environment (see docs/development/releasing.md):
  OA_MACOS_APP_IDENTITY        the Developer ID Application identity to sign
                               the application with; unset, it is signed ad hoc
  OA_MACOS_INSTALLER_IDENTITY  the Developer ID Installer identity to sign the
                               installer package with; unset, it is unsigned
  OA_MACOS_NOTARY_PROFILE      the notarytool keychain profile that
                               'xcrun notarytool store-credentials' saved, or
  OA_MACOS_NOTARY_KEY, OA_MACOS_NOTARY_KEY_ID, OA_MACOS_NOTARY_ISSUER
                               an App Store Connect API key file, its key ID
                               and its issuer ID; with neither, nothing is
                               notarized
USAGE
}

require_value() {
    if [[ $# -lt 2 || -z "$2" ]]; then
        printf 'release_macos.sh: %s requires a value\n' "$1" >&2
        exit 2
    fi
}

absolute() {
    case "$1" in /*) printf '%s' "$1" ;; *) printf '%s/%s' "$PWD" "$1" ;; esac
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --version) require_value "$@"; version="$2"; shift 2 ;;
        --out) require_value "$@"; out_dir="$(absolute "$2")"; shift 2 ;;
        --build-dir) require_value "$@"; build_dir="$(absolute "$2")"; shift 2 ;;
        --deps) require_value "$@"; deps_dir="$(absolute "$2")"; shift 2 ;;
        --jobs) require_value "$@"; jobs="$2"; shift 2 ;;
        --app) require_value "$@"; app="$(absolute "${2%/}")"; shift 2 ;;
        --game-dir) require_value "$@"; game_dir="$(absolute "$2")"; shift 2 ;;
        --save) require_value "$@"; save_file="$(absolute "$2")"; shift 2 ;;
        --previous-zip) require_value "$@"; previous_zip="$(absolute "$2")"; shift 2 ;;
        --unsigned) unsigned=1; shift ;;
        --allow-local-changes) allow_changes=1; shift ;;
        *) printf 'release_macos.sh: unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

step() { printf '== %s\n' "$*"; }
fail() { printf 'release_macos.sh: %s\n' "$*" >&2; exit 1; }
plist_value() { /usr/libexec/PlistBuddy -c "Print :$2" "$1/Contents/Info.plist"; }

# The notices that travel with the game, one path a line: LICENSE,
# ATTRIBUTIONS.md and the files of licenses/ that Git tracks, never an
# ignored or untracked file beside them.
notice_files() { git -C "$repo_dir" ls-files -- LICENSE ATTRIBUTIONS.md licenses | LC_ALL=C sort; }

# Checks that a folder holds the source's notices and nothing else in
# licenses/.
check_notices() {
    local dir="$1" notices path others
    notices="$(notice_files)"
    [[ -n "$notices" ]] || fail "Git tracks no notices in $repo_dir"
    while IFS= read -r path; do
        cmp -s "$repo_dir/$path" "$dir/$path" || fail "$dir/$path differs from the source's"
    done <<<"$notices"
    others="$(comm -13 <(printf '%s\n' "$notices") <(cd "$dir" && find licenses -type f | LC_ALL=C sort))"
    [[ -z "$others" ]] || fail "$dir holds notices Git does not track: $(tr '\n' ' ' <<<"$others")"
}

[[ "$(uname -s)" == Darwin ]] || fail "the macOS release is built on macOS"
[[ -z "$save_file" || -n "$game_dir" ]] || fail "--save needs --game-dir"
if [[ -z "$out_dir" ]]; then out_dir="$build_dir/package"; fi

# The signing and notary settings. This script never prints them, but the
# checks' tools print the name of the certificate that signed a package
# (codesign -dvv, spctl -vvv, productsign and pkgutil --check-signature), as
# anyone can read it from the package; keep the output private all the same.
# The notary credentials are passed to xcrun notarytool alone.
app_identity=""
installer_identity=""
notary_args=()
if [[ "$unsigned" == 0 ]]; then
    app_identity="${OA_MACOS_APP_IDENTITY:-}"
    installer_identity="${OA_MACOS_INSTALLER_IDENTITY:-}"
    if [[ -n "${OA_MACOS_NOTARY_PROFILE:-}" ]]; then
        notary_args=(--keychain-profile "$OA_MACOS_NOTARY_PROFILE")
    elif [[ -n "${OA_MACOS_NOTARY_KEY:-}${OA_MACOS_NOTARY_KEY_ID:-}${OA_MACOS_NOTARY_ISSUER:-}" ]]; then
        if [[ ! -f "${OA_MACOS_NOTARY_KEY:-}" || -z "${OA_MACOS_NOTARY_KEY_ID:-}" \
              || -z "${OA_MACOS_NOTARY_ISSUER:-}" ]]; then
            fail "OA_MACOS_NOTARY_KEY must name the API key file," \
                "with OA_MACOS_NOTARY_KEY_ID and OA_MACOS_NOTARY_ISSUER set"
        fi
        notary_args=(--key "$OA_MACOS_NOTARY_KEY" --key-id "$OA_MACOS_NOTARY_KEY_ID" --issuer "$OA_MACOS_NOTARY_ISSUER")
    fi
fi
if [[ -n "$app_identity" ]]; then
    identities="$(security find-identity -v -p codesigning)"
    grep -qF -- "$app_identity" <<<"$identities" \
        || fail "no valid code-signing identity in the keychains matches OA_MACOS_APP_IDENTITY"
fi
if [[ -n "$installer_identity" ]]; then
    identities="$(security find-identity -v)"
    grep -qF -- "$installer_identity" <<<"$identities" \
        || fail "no valid identity in the keychains matches OA_MACOS_INSTALLER_IDENTITY"
fi
# Apple notarizes only what a Developer ID signed.
notarize_app=0
notarize_pkg=0
if [[ ${#notary_args[@]} -gt 0 ]]; then
    if [[ -n "$app_identity" ]]; then notarize_app=1; fi
    if [[ -n "$installer_identity" ]]; then notarize_pkg=1; fi
fi
# Why a setting is off: --unsigned, or what the environment lacks.
off_because() {
    if [[ "$unsigned" == 1 ]]; then printf '%s' --unsigned; else printf '%s' "$1"; fi
}
step "settings"
if [[ -n "$app_identity" ]]; then
    echo "application: signed with the Developer ID that OA_MACOS_APP_IDENTITY names"
else
    echo "application: signed ad hoc, not with a Developer ID ($(off_because "OA_MACOS_APP_IDENTITY is not set"))"
fi
if [[ -n "$installer_identity" ]]; then
    echo "installer package: signed with the Developer ID that OA_MACOS_INSTALLER_IDENTITY names"
else
    echo "installer package: unsigned ($(off_because "OA_MACOS_INSTALLER_IDENTITY is not set"))"
fi
if [[ ${#notary_args[@]} -eq 0 ]]; then
    echo "notarization: none ($(off_because "no OA_MACOS_NOTARY_PROFILE or OA_MACOS_NOTARY_KEY"))"
elif [[ "$notarize_app" == 0 || "$notarize_pkg" == 0 ]]; then
    echo "notarization: only what a Developer ID signs"
fi

project_version="$(sed -n 's/^project(open_annihilation VERSION \([0-9.]*\).*/\1/p' "$repo_dir/CMakeLists.txt")"
[[ -n "$version" ]] || version="$project_version"
version="v${version#v}"
[[ "$version" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "the version must be vX.Y.Z or X.Y.Z, not ${version#v}"

# The game, built the release's way unless --app names one. What keeps the
# packages from being a release is gathered on the way and printed last.
not_release=()
release_build=0
if [[ -z "$app" ]]; then
    step "sources"
    if [[ -n "$(git -C "$repo_dir" status --porcelain)" ]]; then
        [[ "$allow_changes" == 1 ]] \
            || fail "$repo_dir has uncommitted changes; commit them, or pass --allow-local-changes for a test build"
        echo "warning: building uncommitted changes, which is never a release"
        not_release+=("the tree had uncommitted changes (--allow-local-changes)")
    fi
    echo "engine $(git -C "$repo_dir" rev-parse HEAD) $(git -C "$repo_dir" log -1 --format=%s)"
    if git -C "$repo_dir" tag --points-at HEAD | grep -qxF -- "$version"; then
        echo "tagged $version"
    else
        echo "warning: HEAD is not tagged $version"
        not_release+=("HEAD is not tagged $version")
    fi

    step "dependencies"
    python3 "$repo_dir/tools/bootstrap_macos_deps.py" --deps "$deps_dir" \
        --deployment-target "$macos_minimum" --jobs "$jobs"
    prefix="$deps_dir/macos-$macos_minimum"
    # The text fonts, the same for every architecture; FreeType is the
    # prefix's.
    python3 "$repo_dir/tools/bootstrap_text_fonts.py" --deps "$deps_dir" --fonts-only
    text_fonts="$deps_dir/text-fonts"

    # A Release build without tests, for both architectures and the oldest
    # macOS release, with the static libraries of the prefix only: never
    # Homebrew's, and never with an installation's game data. The build
    # tree's cache is cleared, so every cache variable is the one set here or
    # the project's default, never an earlier configure's; the compiled
    # objects stay, and the build tool recompiles what the new settings
    # change. The bundle is made afresh, so the files the build copies into
    # it after linking (the notices and the icon) are the source's, and
    # nothing is left over from an earlier build.
    step "configure"
    rm -rf "$build_dir/open-annihilation.app"
    generator_args=()
    if [[ -f "$build_dir/CMakeCache.txt" ]]; then
        generator="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$build_dir/CMakeCache.txt")"
        rm -f "$build_dir/CMakeCache.txt"
        if [[ -n "$generator" ]]; then generator_args=(-G "$generator"); fi
    elif command -v ninja >/dev/null 2>&1; then
        generator_args=(-G Ninja)
    fi
    env -u OA_GAME_DIR -u OA_DEMO_INSTALLER -u CMAKE_PREFIX_PATH \
        cmake -S "$repo_dir" -B "$build_dir" ${generator_args[@]+"${generator_args[@]}"} \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DOA_SELF_CHECKS=OFF \
        "-DCMAKE_OSX_ARCHITECTURES=$(IFS=';' && printf '%s' "${architectures[*]}")" \
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=$macos_minimum" \
        "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local" \
        -DOA_BUILD_PLATFORM=ON -DOA_BUILD_INTRO_PLAYER=ON -DOA_REQUIRE_GAME=ON \
        -DOA_GAME_DIR= -DOA_DEMO_INSTALLER= \
        "-DSDL3_DIR=$prefix/sdl/lib/cmake/SDL3" \
        "-Dfreetype_DIR=$prefix/freetype/lib/cmake/freetype" "-DOA_TEXT_FONTS_DIR=$text_fonts" \
        "-DZLIB_INCLUDE_DIR=$prefix/zlib/include" "-DZLIB_LIBRARY=$prefix/zlib/lib/libz.a"

    step "build"
    cmake --build "$build_dir" --parallel "$jobs" --target oa-game
    app="$build_dir/open-annihilation.app"
    release_build=1
else
    not_release+=("--app packaged a bundle that the release build did not make or check")
fi

[[ -f "$app/Contents/Info.plist" ]] || fail "no application bundle at $app"
app_executable="$(plist_value "$app" CFBundleExecutable)"
[[ -x "$app/Contents/MacOS/$app_executable" ]] || fail "no executable in $app"
# The packages name the application as its Info.plist does, Open
# Annihilation.app; the build tree keeps the target's name.
app_name="$(plist_value "$app" CFBundleName)"
bundle_id="$(plist_value "$app" CFBundleIdentifier)"
app_version="$(plist_value "$app" CFBundleShortVersionString)"
[[ "$version" == "v$app_version" ]] || fail "the application's Info.plist says version $app_version, not ${version#v}"

if [[ "$release_build" == 1 ]]; then
    step "check the build"
    binary="$app/Contents/MacOS/$app_executable"
    [[ "$(ls -A "$app/Contents/MacOS")" == "$app_executable" ]] || fail "Contents/MacOS holds more than $app_executable"
    resources="$(ls -A "$app/Contents/Resources" | LC_ALL=C sort | tr '\n' ' ')"
    expected="$(printf '%s\n' ATTRIBUTIONS.md LICENSE fonts languages licenses "$(plist_value "$app" CFBundleIconFile)" \
        | LC_ALL=C sort | tr '\n' ' ')"
    [[ "$resources" == "$expected" ]] || fail "unexpected files in Contents/Resources: $resources"
    # The text fonts, and nothing else, as the bootstrap made them.
    fonts="$(ls -A "$app/Contents/Resources/fonts" | LC_ALL=C sort | tr '\n' ' ')"
    expected_fonts="$(ls -A "$text_fonts" | grep -Ev '^build-settings\.json$' | LC_ALL=C sort | tr '\n' ' ')"
    [[ "$fonts" == "$expected_fonts" ]] || fail "Contents/Resources/fonts holds $fonts, not $expected_fonts"
    for font in "$text_fonts"/*.ttf "$text_fonts"/*.otf; do
        cmp -s "$font" "$app/Contents/Resources/fonts/$(basename "$font")" \
            || fail "Contents/Resources/fonts/$(basename "$font") differs from $font"
    done
    lipo -info "$binary"
    for arch in "${architectures[@]}"; do
        lipo "$binary" -verify_arch "$arch" || fail "the game has no $arch code"
        if otool -arch "$arch" -L "$binary" | tail -n +2 | grep -Ev '^[[:space:]]+(/usr/lib/|/System/Library/)'; then
            fail "the $arch executable needs a library outside the system (above)"
        fi
        minos="$(otool -arch "$arch" -l "$binary" | awk '/LC_BUILD_VERSION/ {found = 1}
            found && $1 == "minos" && minos == "" {minos = $2} END {print minos}')"
        [[ "$minos" == "$macos_minimum" ]] || fail "the $arch game needs macOS $minos, not $macos_minimum"
    done
    [[ "$(plist_value "$app" LSMinimumSystemVersion)" == "$macos_minimum" ]] \
        || fail "the Info.plist's LSMinimumSystemVersion is not $macos_minimum"
    check_notices "$app/Contents/Resources"
    echo "${architectures[*]}, macOS $macos_minimum or later, system libraries only, the source's notices,"
    echo "the text fonts: $fonts"
    echo "$bundle_id $app_version"
fi

package="open-annihilation-$version-macos-universal"
zip_path="$out_dir/$package.zip"
pkg_path="$out_dir/$package.pkg"
symbols_dir="$out_dir/$package-symbols"
work="$out_dir/work"
rm -rf "$work" "$zip_path" "$pkg_path" "$symbols_dir"
mkdir -p "$work"

step "stage"
stage="$work/stage/$package"
staged_app="$stage/$app_name.app"
mkdir -p "$stage"
ditto --norsrc --noextattr --noqtn --noacl "$app" "$staged_app"
while IFS= read -r path; do
    mkdir -p "$(dirname "$stage/$path")"
    cp "$repo_dir/$path" "$stage/$path"
done <<<"$(notice_files)"
find "$stage" \( -name .DS_Store -o -name '._*' \) -print -delete
echo "$staged_app"

# The packaged executable carries no symbols; the build's own, which has
# them, is kept beside the packages for reading a crash's addresses (its
# UUID is the packaged one's).
step "strip"
mkdir -p "$symbols_dir"
cp "$staged_app/Contents/MacOS/$app_executable" "$symbols_dir/$app_executable"
strip "$staged_app/Contents/MacOS/$app_executable"
echo "$symbols_dir/$app_executable: $(wc -c <"$symbols_dir/$app_executable" | tr -d ' ') bytes," \
    "$(wc -c <"$staged_app/Contents/MacOS/$app_executable" | tr -d ' ') stripped"

# Code inside the bundle besides its executable is signed before what holds
# it: every Mach-O file, then every nested bundle, each deepest first, then
# the application. The game has none today, since SDL3, zlib and FreeType
# are linked into its executable, but a library or helper added later is signed
# where it lies. A Developer ID signature carries the hardened runtime and a
# secure timestamp, and no entitlements (docs/development/releasing.md).
step "sign"
sign() {
    if [[ -n "$app_identity" ]]; then
        codesign --force --options runtime --timestamp --sign "$app_identity" "$1"
    else
        codesign --force --sign - "$1"
    fi
}
nested=()
while IFS= read -r -d '' path; do
    if [[ "$path" != "$staged_app/Contents/MacOS/$app_executable" ]]; then
        case "$(file -b "$path")" in Mach-O*) nested+=("$path") ;; esac
    fi
done < <(find "$staged_app/Contents" -depth -type f -print0)
while IFS= read -r -d '' path; do
    nested+=("$path")
done < <(find "$staged_app/Contents" -depth -mindepth 1 -type d \( -name '*.app' -o -name '*.appex' \
    -o -name '*.bundle' -o -name '*.framework' -o -name '*.plugin' -o -name '*.xpc' \) -print0)
for path in ${nested[@]+"${nested[@]}"}; do
    echo "signing ${path#"$staged_app/"}"
    sign "$path"
done
sign "$staged_app"
codesign --verify --deep --strict --verbose=2 "$staged_app"

# Prints the value of a key of the JSON object on standard input, or nothing.
json_value() {
    python3 -c 'import json, sys; print(json.load(sys.stdin).get(sys.argv[1], ""))' "$1" 2>/dev/null || true
}

# Submits a file to Apple's notary service and waits for its verdict; keeps
# the service's log in the work folder and prints the issues it lists.
notarize() {
    local file="$1" what="$2" result id status log="$work/notary-$2.json"
    echo "submitting the $what to Apple's notary service (this needs internet access)"
    result="$(xcrun notarytool submit "$file" "${notary_args[@]}" --wait --output-format json)" || true
    id="$(json_value id <<<"$result")"
    status="$(json_value status <<<"$result")"
    [[ -n "$id" ]] || { printf '%s\n' "$result" >&2; fail "notarytool did not submit the $what"; }
    echo "notarization of the $what: submission $id, $status"
    if xcrun notarytool log "$id" "${notary_args[@]}" "$log" >/dev/null; then
        python3 -c 'import json, sys
issues = json.load(open(sys.argv[1])).get("issues") or []
for issue in issues:
    print("  %s: %s: %s" % (issue.get("severity"), issue.get("path"), issue.get("message")))
print("  %d issue(s); the log is %s" % (len(issues), sys.argv[1]))' "$log"
    else
        echo "  the notary log is not available yet: xcrun notarytool log $id"
    fi
    [[ "$status" == Accepted ]] || fail "Apple did not accept the $what ($status)"
}

if [[ "$notarize_app" == 1 ]]; then
    step "notarize the application"
    ditto -c -k --keepParent "$staged_app" "$work/notary-application.zip"
    notarize "$work/notary-application.zip" application
    xcrun stapler staple "$staged_app"
fi

step "zip"
ditto -c -k --norsrc --noextattr --noqtn --noacl --keepParent "$stage" "$zip_path"

# The installer package holds the application alone and installs it in
# /Applications on the startup disk. Its component is not relocatable, so
# Installer writes to /Applications even when another copy of the
# application, a build tree's say, is elsewhere on the disk.
step "installer package"
mkdir -p "$work/pkg-root" "$work/component"
ditto --norsrc --noextattr --noqtn --noacl "$staged_app" "$work/pkg-root/$app_name.app"
pkgbuild --analyze --root "$work/pkg-root" "$work/components.plist"
/usr/libexec/PlistBuddy -c 'Set :0:BundleIsRelocatable false' "$work/components.plist"
component="$work/component/$app_executable.pkg"
pkgbuild --root "$work/pkg-root" --component-plist "$work/components.plist" --identifier "$bundle_id" \
    --version "$app_version" --install-location /Applications "$component"

# pkgbuild keeps each file's extended attributes as an AppleDouble (._)
# entry beside the file, in the payload and in its bill of materials, and
# Installer sets them on the files it writes. macOS gives every file written
# in some sessions, a terminal inside another application's window for one,
# a com.apple.provenance attribute that nothing removes from the file, so the
# component is rewritten without those entries: the installed application
# then carries no attribute, as the one the zip holds. Every other entry is
# pkgbuild's, byte for byte.
component_dir="$work/component-expanded"
pkgutil --expand "$component" "$component_dir"
lsbom "$component_dir/Bom" | awk -F '\t' '{ n = split($1, part, "/"); if (part[n] !~ /^\._/) print }' \
    >"$work/bom.txt"
mkbom -i "$work/bom.txt" "$component_dir/Bom"
python3 - "$component_dir/Payload" <<'PAYLOAD'
import gzip
import sys

# The payload is a gzip-compressed cpio archive in the portable (odc) format:
# a 76-byte header of octal fields, the NUL-terminated name, then the data.
path = sys.argv[1]
with open(path, "rb") as payload:
    data = payload.read()
if data[:2] != b"\x1f\x8b":
    sys.exit("the component's payload is not gzip-compressed")
data = gzip.decompress(data)
kept = bytearray()
position = dropped = 0
while True:
    header = data[position:position + 76]
    if header[:6] != b"070707":
        sys.exit(f"the component's payload has no cpio header at byte {position}")
    name_size = int(header[59:65], 8)
    file_size = int(header[65:76], 8)
    name = data[position + 76:position + 76 + name_size - 1]
    end = position + 76 + name_size + file_size
    if name.rsplit(b"/", 1)[-1].startswith(b"._"):
        dropped += 1
    else:
        kept += data[position:end]
    position = end
    if name == b"TRAILER!!!":
        break
kept += data[position:]
with open(path, "wb") as payload:
    payload.write(gzip.compress(bytes(kept), compresslevel=9, mtime=0))
print(f"the component's payload: {dropped} AppleDouble entries left out")
PAYLOAD
diff <(lsbom -s "$component_dir/Bom" | LC_ALL=C sort) \
    <(gzip -dc "$component_dir/Payload" | cpio -it 2>/dev/null | LC_ALL=C sort) \
    || fail "the component's bill of materials and payload differ (above)"
sed -i '' -E "s/numberOfFiles=\"[0-9]+\"/numberOfFiles=\"$(wc -l <"$work/bom.txt" | tr -d ' ')\"/" \
    "$component_dir/PackageInfo"
rm "$component"
pkgutil --flatten "$component_dir" "$component"

# The distribution requires the oldest macOS release and the architectures
# of the application it installs, as its Info.plist and executable give them.
pkg_minimum="$(plist_value "$staged_app" LSMinimumSystemVersion)"
read -r -a pkg_architectures <<<"$(lipo -archs "$staged_app/Contents/MacOS/$app_executable")"
requirements=(-c "Add :os array" -c "Add :os:0 string $pkg_minimum" -c "Add :arch array")
for index in "${!pkg_architectures[@]}"; do
    requirements+=(-c "Add :arch:$index string ${pkg_architectures[$index]}")
done
/usr/libexec/PlistBuddy "${requirements[@]}" "$work/requirements.plist" >/dev/null
productbuild --synthesize --product "$work/requirements.plist" --package "$component" "$work/synthesized.xml"
awk -v title="$app_name" '{ print }
    /^<installer-gui-script/ {
        print "    <title>" title "</title>"
        print "    <domains enable_anywhere=\"false\" enable_currentUserHome=\"false\" enable_localSystem=\"true\"/>"
    }' "$work/synthesized.xml" >"$work/distribution.xml"
grep -q '<title>' "$work/distribution.xml" || fail "productbuild wrote an unexpected distribution"
# productbuild writes the product unsigned and productsign signs it, so that
# one tool signs every installer package and the keychain is asked to let
# only that tool use the Developer ID Installer key (docs/development/releasing.md).
if [[ -n "$installer_identity" ]]; then
    productbuild --distribution "$work/distribution.xml" --package-path "$work/component" "$work/unsigned.pkg"
    productsign --sign "$installer_identity" --timestamp "$work/unsigned.pkg" "$pkg_path"
else
    productbuild --distribution "$work/distribution.xml" --package-path "$work/component" "$pkg_path"
fi
if [[ "$notarize_pkg" == 1 ]]; then
    step "notarize the installer package"
    notarize "$pkg_path" installer
    xcrun stapler staple "$pkg_path"
fi

# Checks an application as a player receives it: sealed, and, as far as it
# was signed and notarized, with the hardened runtime, a secure timestamp, a
# stapled ticket and Gatekeeper's approval.
check_application() {
    local bundle="$1" details assessment
    codesign --verify --deep --strict --verbose=2 "$bundle"
    if [[ -n "$app_identity" ]]; then
        details="$(codesign -dvv "$bundle" 2>&1)"
        grep -Eq 'flags=0x[0-9a-f]+\([^)]*runtime' <<<"$details" || fail "$bundle lacks the hardened runtime"
        grep -q '^Authority=Developer ID Application: ' <<<"$details" \
            || fail "$bundle is not signed with a Developer ID"
        grep -q '^Timestamp=' <<<"$details" || fail "$bundle has no secure timestamp"
        grep -Eq '^TeamIdentifier=[0-9A-Z]+$' <<<"$details" || fail "$bundle names no team"
        echo "$bundle: Developer ID, hardened runtime, secure timestamp"
    fi
    if [[ "$notarize_app" == 1 ]]; then
        xcrun stapler validate "$bundle"
        assessment="$(spctl -a -vvv -t exec "$bundle" 2>&1)" \
            || { printf '%s\n' "$assessment"; fail "Gatekeeper rejects $bundle"; }
        printf '%s\n' "$assessment"
        grep -q '^source=Notarized Developer ID' <<<"$assessment" || fail "Gatekeeper does not see $bundle as notarized"
    fi
}

step "check the zip"
unzip -tq "$zip_path"
entries="$(zipinfo -1 "$zip_path")"
if grep -Eq '(^|/)(__MACOSX|\.DS_Store|\._)' <<<"$entries"; then fail "the zip holds junk entries"; fi
[[ "$(cut -d/ -f1 <<<"$entries" | sort -u)" == "$package" ]] || fail "the zip holds more than the folder $package"
if [[ -n "$previous_zip" ]]; then
    previous_package="$(zipinfo -1 "$previous_zip" | sed -n 1p | cut -d/ -f1)"
    if diff <(zipinfo -1 "$previous_zip" | sed "s|^$previous_package|PACKAGE|" | sort) \
            <(sed "s|^$package|PACKAGE|" <<<"$entries" | sort); then
        echo "file list: the same as $(basename "$previous_zip")"
    else
        echo "file list: differs from $(basename "$previous_zip") (above: < previous, > this)"
    fi
fi
echo "$(wc -l <<<"$entries" | tr -d ' ') entries in one folder, $package"
mkdir -p "$work/extracted/ditto" "$work/extracted/unzip"
ditto -x -k "$zip_path" "$work/extracted/ditto"
unzip -q "$zip_path" -d "$work/extracted/unzip"
diff -r "$work/extracted/ditto" "$work/extracted/unzip"
extracted="$work/extracted/ditto/$package"
check_notices "$extracted"
diff -r "$staged_app" "$extracted/$app_name.app" || fail "the zip's $app_name.app differs from the one staged"
check_application "$extracted/$app_name.app"
check_application "$work/extracted/unzip/$package/$app_name.app"

# The payload is checked as Installer writes it. pkgutil --payload-files
# lists the AppleDouble (._) entries that carry extended attributes, which
# pkgutil --expand-full turns back into attributes.
step "check the installer package"
if pkgutil --payload-files "$pkg_path" | grep -E '(^|/)\._'; then
    fail "the installer package's payload holds AppleDouble entries (above)"
fi
pkgutil --expand-full "$pkg_path" "$work/expanded"
payload="$work/expanded/$app_executable.pkg/Payload"
[[ "$(ls -A "$payload")" == "$app_name.app" ]] || fail "the installer package holds more than $app_name.app"
[[ -z "$(find "$payload" -name '._*')" ]] || fail "the installer package holds ._ files"
[[ -z "$(xattr -r "$payload" 2>/dev/null | grep -v 'com\.apple\.provenance')" ]] \
    || fail "the installer package gives its files extended attributes"
diff -r "$staged_app" "$payload/$app_name.app" || fail "the installer package's $app_name.app differs from the zip's"
package_info="$work/expanded/$app_executable.pkg/PackageInfo"
grep -q 'install-location="/Applications"' "$package_info" \
    || fail "the installer package does not install in /Applications"
grep -q 'relocatable="false"' "$package_info" || fail "the installer package's component is relocatable"
check_application "$payload/$app_name.app"
echo "installs $app_name.app in /Applications, macOS $pkg_minimum or later, ${pkg_architectures[*]}"
if [[ -n "$installer_identity" ]]; then
    pkgutil --check-signature "$pkg_path"
fi
if [[ "$notarize_pkg" == 1 ]]; then
    xcrun stapler validate "$pkg_path"
    assessment="$(spctl -a -vvv -t install "$pkg_path" 2>&1)" \
        || { printf '%s\n' "$assessment"; fail "Gatekeeper rejects the installer package"; }
    printf '%s\n' "$assessment"
    grep -q '^source=Notarized Developer ID' <<<"$assessment" \
        || fail "Gatekeeper does not see the installer package as notarized"
fi

# Headless starts of the game from the zip as ditto and as unzip extract it,
# under each architecture of the application that this Mac runs (x86_64
# needs Rosetta on Apple silicon).
if [[ -n "$game_dir" ]]; then
    step "headless start"
    game="$extracted/$app_name.app/Contents/MacOS/$app_executable"
    started=()
    for arch in $(lipo -archs "$game"); do
        if ! arch "-$arch" /usr/bin/true 2>/dev/null; then
            echo "this Mac does not run $arch code; not started under $arch"
            continue
        fi
        for copy in ditto unzip; do
            log="$work/headless-$copy-$arch.log"
            arch "-$arch" env SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
                "$work/extracted/$copy/$package/$app_name.app/Contents/MacOS/$app_executable" \
                --game-dir "$game_dir" --skip-intro --headless-check --frames 60 \
                --preferences-file "$work/headless-$copy-$arch.conf" >"$log" 2>&1 \
                || { cat "$log"; fail "the $copy copy of the game did not start under $arch"; }
            grep 'native check:' "$log" | sed "s/^/$copy $arch: /" || true
            [[ "$(grep -c 'native check:' "$log")" -ge 3 ]] \
                || { cat "$log"; fail "the checks of the $copy copy under $arch did not all run"; }
        done
        started+=("$arch")
    done
    if [[ -n "$save_file" ]]; then
        step "saved game"
        digests=()
        for arch in ${started[@]+"${started[@]}"}; do
            log="$work/load-$arch.log"
            arch "-$arch" env SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy "$game" --game-dir "$game_dir" \
                --headless-check --mute --load "$save_file" --match-ticks 1000 \
                --preferences-file "$work/load-$arch.conf" >"$log" 2>&1 \
                || { cat "$log"; fail "the saved game did not load under $arch"; }
            grep '^saveload:' "$log" | sed "s/^/$arch: /" || true
            if grep -q 'simulation error' "$log"; then cat "$log"; fail "a simulation error under $arch"; fi
            digest="$({ grep '^saveload: tick' "$log" || true; } | tail -1 | awk '{print $NF}')"
            [[ -n "$digest" ]] || { cat "$log"; fail "no final digest under $arch"; }
            digests+=("$digest")
        done
        [[ "$(printf '%s\n' ${digests[@]+"${digests[@]}"} | sort -u | wc -l | tr -d ' ')" -le 1 ]] \
            || fail "the final digests differ: ${digests[*]}"
        echo "final digests: ${digests[*]-none}"
    fi
fi

step "packages"
shasum -a 256 "$zip_path" "$pkg_path"
ls -l "$zip_path" "$pkg_path"
if [[ "$notarize_app" == 0 || "$notarize_pkg" == 0 ]]; then
    not_release+=("not signed with a Developer ID and notarized throughout (see the settings above)")
fi
if [[ -z "$game_dir" ]]; then
    echo "note: the game was not started (--game-dir)"
fi
if [[ ${#not_release[@]} -eq 0 ]]; then
    echo "release $version: built from the tagged tree, signed with a Developer ID, notarized and stapled"
else
    echo "NOT FOR RELEASE:"
    printf '  %s\n' "${not_release[@]}"
fi
