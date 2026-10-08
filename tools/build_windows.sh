#!/usr/bin/env bash
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Cross-build the project for Windows from macOS or Linux with a mingw-w64
# cross-compiler: for x86-64, or for 32-bit x86 with
# OA_MINGW_TRIPLE=i686-w64-mingw32, and with --xp for executables that also
# run on Windows XP. zlib, SDL3 and FreeType are built for the target into
# local/deps/<name> on first use, and the text fonts, the same for every
# target, into local/deps/text-fonts; the tree (or the project that --source
# names, which adds this one) is configured in build-<name> with
# cmake/toolchains/<triple>.cmake and every target is compiled. <name> is
# windows for x86-64 and windows-i686 for 32-bit x86, with -xp added for
# --xp, so that each target keeps its own dependencies and build tree.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
triple="${OA_MINGW_TRIPLE:-x86_64-w64-mingw32}"
deps="${OA_WINDOWS_DEPS:-}"
build_dir="${OA_WINDOWS_BUILD_DIR:-}"
source_dir="$repo_dir"
build_type="${OA_BUILD_TYPE:-Release}"
jobs="${OA_BUILD_JOBS:-6}"
testing=ON
windows_xp=OFF
windows_95=OFF
run_tests=0
targets=()

usage() {
    cat <<'USAGE'
Usage: tools/build_windows.sh [--xp] [--win95] [--no-tests] [--run-tests]
                              [--build-type TYPE] [--jobs N] [--target NAME]...
                              [--source DIR]

Cross-builds the project for Windows with a mingw-w64 cross-compiler: for
x86-64, or for 32-bit x86 with OA_MINGW_TRIPLE=i686-w64-mingw32. Executables
land in build-windows/ (build-windows-i686/ for 32-bit x86, each with -xp
added for --xp or -95 for --win95) as *.exe with the run-time libraries
linked statically. They
run on Windows, or here under Wine with --run-tests (tools/test_windows.sh
provides a container with the x86-64 toolchain and Wine on macOS and Linux).

  --xp              Build executables that also run on Windows XP
                    (OA_WINDOWS_XP): SP3 for 32-bit x86, the 64-bit edition
                    for x86-64
  --win95           Build executables that also run on Windows 95
                    (OA_WINDOWS_95), 32-bit x86 only
  --no-tests        Configure with BUILD_TESTING=OFF
  --run-tests       Run ctest after the build, each test executable through
                    Wine (CMAKE_TEST_LAUNCHER); needs wine on PATH and CMake 3.29+.
                    OA_WINDOWS_CTEST_ARGS adds ctest arguments (e.g. -R or -E).
  --build-type TYPE CMake build type (default: Release)
  --jobs N          Parallel compile jobs (default: 6)
  --target NAME     Build only this target (repeatable)
  --source DIR      Configure DIR, a project that adds this one, instead of this
                    tree; its build tree defaults to DIR/build-windows (with the
                    same additions)
  -h, --help        Show this help

Environment: OA_MINGW_TRIPLE (x86_64-w64-mingw32, the default, or
i686-w64-mingw32), OA_WINDOWS_DEPS (default: local/deps/windows, with the
same additions as the build tree), OA_WINDOWS_BUILD_DIR, OA_BUILD_TYPE,
OA_BUILD_JOBS, OA_WINDOWS_CTEST_ARGS, OA_WINDOWS_CCACHE (0 builds without
ccache). A dependency folder or build tree made for another target is
refused rather than mixed with this one's.
USAGE
}

require_value() {
    if [[ $# -lt 2 || -z "$2" ]]; then
        printf 'build_windows.sh: %s requires a value\n' "$1" >&2
        exit 2
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --build-type) require_value "$@"; build_type="$2"; shift 2 ;;
        --jobs) require_value "$@"; jobs="$2"; shift 2 ;;
        --target) require_value "$@"; targets+=("$2"); shift 2 ;;
        --source) require_value "$@"; source_dir="$2"; shift 2 ;;
        --xp) windows_xp=ON; shift ;;
        --win95) windows_95=ON; shift ;;
        --no-tests) testing=OFF; shift ;;
        --run-tests) run_tests=1; shift ;;
        *) printf 'build_windows.sh: unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

# The targets this script builds, each with a toolchain file of its own.
case "$triple" in
    x86_64-w64-mingw32) name=windows; debian_package=g++-mingw-w64-x86-64 ;;
    i686-w64-mingw32) name=windows-i686; debian_package=g++-mingw-w64-i686 ;;
    *)
        printf 'build_windows.sh: OA_MINGW_TRIPLE=%s is not a target this script builds; ' "$triple" >&2
        printf 'it builds x86_64-w64-mingw32 (the default) and i686-w64-mingw32\n' >&2
        exit 2
        ;;
esac
toolchain="$repo_dir/cmake/toolchains/$triple.cmake"
target="$triple"
if [[ "$windows_xp" == ON ]]; then
    name="$name-xp"
    target="$triple for Windows XP"
elif [[ "$windows_95" == ON ]]; then
    name="$name-95"
    target="$triple for Windows 95"
fi

if ! command -v "$triple-g++" >/dev/null 2>&1; then
    cat >&2 <<EOF
build_windows.sh: $triple-g++ is not installed.
  macOS:          brew install mingw-w64
  Debian/Ubuntu:  sudo apt-get install $debian_package
EOF
    exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
    printf 'build_windows.sh: CMake 3.24+ is required.\n' >&2
    exit 1
fi
# The tests run through Wine, which ctest puts in front of every test
# executable it starts.
launcher_args=()
if [[ "$run_tests" == 1 ]]; then
    if [[ "$testing" == OFF || ${#targets[@]} -gt 0 ]]; then
        printf 'build_windows.sh: --run-tests needs the whole tree with tests; drop --no-tests and --target\n' >&2
        exit 2
    fi
    if ! command -v wine >/dev/null 2>&1; then
        printf 'build_windows.sh: --run-tests needs wine on PATH (tools/test_windows.sh provides it)\n' >&2
        exit 1
    fi
    launcher_args=("-DCMAKE_TEST_LAUNCHER=$(command -v wine)")
fi
case "$source_dir" in /*) ;; *) source_dir="$PWD/$source_dir" ;; esac
case "$build_dir" in "") ;; /*) ;; *) build_dir="$PWD/$build_dir" ;; esac
if [[ -z "$build_dir" ]]; then build_dir="$source_dir/build-$name"; fi
if [[ -z "$deps" ]]; then deps="$repo_dir/local/deps/$name"; fi
case "$deps" in /*) ;; *) deps="$PWD/$deps" ;; esac

# A build tree keeps the compiler and options of its first configure, so one
# made for another target would build the wrong thing.
cache="$build_dir/CMakeCache.txt"
if [[ -f "$cache" ]]; then
    tree_toolchain="$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:[A-Z]*=//p' "$cache")"
    tree_target="$(basename -- "${tree_toolchain:-no toolchain file}" .cmake)"
    case "$(sed -n 's/^OA_WINDOWS_XP:[A-Z]*=//p' "$cache" | tr '[:upper:]' '[:lower:]')" in
        1|on|yes|true|y) tree_target="$tree_target for Windows XP" ;;
    esac
    if [[ "$tree_target" != "$target" ]]; then
        printf 'build_windows.sh: %s was configured for %s, not %s; remove it or name another tree with OA_WINDOWS_BUILD_DIR\n' \
            "$build_dir" "$tree_target" "$target" >&2
        exit 2
    fi
fi
# The dependency folder records the target its libraries were built for. One
# without the record that holds libraries was made when this script built
# for x86-64 alone.
deps_record="$deps/.oa-windows-target"
deps_target="$target"
if [[ -f "$deps_record" ]]; then
    deps_target="$(<"$deps_record")"
elif [[ -d "$deps/zlib" || -d "$deps/sdl" ]]; then
    deps_target=x86_64-w64-mingw32
fi
if [[ "$deps_target" != "$target" ]]; then
    printf 'build_windows.sh: %s holds dependencies built for %s, not %s; remove it or name another folder with OA_WINDOWS_DEPS\n' \
        "$deps" "$deps_target" "$target" >&2
    exit 2
fi

# ccache, when installed, keeps compiled objects between builds (CCACHE_DIR says
# where); OA_WINDOWS_CCACHE=0 turns it off. Ninja, when installed, generates a
# new build tree; an existing tree keeps the generator it was made with.
compiler_launcher=""
if [[ "${OA_WINDOWS_CCACHE:-1}" != 0 ]] && command -v ccache >/dev/null 2>&1; then
    compiler_launcher=ccache
fi
generator_args=()
if [[ ! -f "$cache" ]] && command -v ninja >/dev/null 2>&1; then
    generator_args=(-G Ninja)
fi

bootstrap_args=(--prefix-root "$deps" --toolchain "$toolchain" --jobs "$jobs")
if [[ "$windows_xp" == ON ]]; then bootstrap_args+=(--xp); fi
if [[ "$windows_95" == ON ]]; then bootstrap_args+=(--win95); fi

printf 'Preparing Windows dependencies (%s) under %s...\n' "$target" "$deps"
mkdir -p "$deps"
printf '%s\n' "$target" >"$deps_record"
python3 "$repo_dir/tools/bootstrap_windows_deps.py" "${bootstrap_args[@]}"
python3 "$repo_dir/tools/bootstrap_text_fonts.py" --fonts-only

printf 'Configuring %s (%s, %s)...\n' "$build_dir" "$target" "$build_type"
cmake -S "$source_dir" -B "$build_dir" ${generator_args[@]+"${generator_args[@]}"} -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    "-DCMAKE_C_COMPILER_LAUNCHER=$compiler_launcher" "-DCMAKE_CXX_COMPILER_LAUNCHER=$compiler_launcher" \
    "-DCMAKE_BUILD_TYPE=$build_type" "-DOA_WINDOWS_DEPS=$deps" "-DOA_WINDOWS_XP=$windows_xp" \
    "-DOA_WINDOWS_95=$windows_95" \
    -DOA_BUILD_PLATFORM=ON -DOA_BUILD_INTRO_PLAYER=ON "-DBUILD_TESTING=$testing" \
    ${launcher_args[@]+"${launcher_args[@]}"}

if [[ ${#targets[@]} -gt 0 ]]; then
    for build_target in "${targets[@]}"; do
        cmake --build "$build_dir" --target "$build_target" --parallel "$jobs"
    done
    exit 0
fi
cmake --build "$build_dir" --parallel "$jobs"

count="$(find "$build_dir" -name '*.exe' | wc -l | tr -d ' ')"
printf 'Windows cross-build complete (%s): %s executables under %s\n' "$target" "$count" "$build_dir"
if [[ -f "$build_dir/open-annihilation.exe" ]]; then
    printf 'open-annihilation.exe: %s\n' "$build_dir/open-annihilation.exe"
else
    printf 'open-annihilation.exe was not built; see the build output above.\n'
fi

if [[ "$run_tests" == 1 ]]; then
    # The scripted checks start the executables they drive through the same
    # runner, which ctest's launcher does not reach.
    OA_TEST_RUNNER="$(command -v wine)"
    export OA_TEST_RUNNER
    printf 'Running the Windows tests under %s...\n' "$(wine --version 2>/dev/null || echo wine)"
    ctest_args=()
    if [[ -n "${OA_WINDOWS_CTEST_ARGS:-}" ]]; then
        read -r -a ctest_args <<<"$OA_WINDOWS_CTEST_ARGS"
    fi
    ctest --test-dir "$build_dir" --output-on-failure --parallel "$jobs" \
        ${ctest_args[@]+"${ctest_args[@]}"}
fi
