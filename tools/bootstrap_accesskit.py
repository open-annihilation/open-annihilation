#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Fetch the pinned AccessKit C bindings and lay out the libraries.

The release archive is checked against its SHA-256 and unpacked into
local/deps/accesskit-c-0.23.2. On macOS the two static libraries are joined
with lipo into one universal library. Another host does not run lipo and
does not require that joined file. The iOS device and simulator libraries,
the Linux static libraries and the Windows mingw DLLs are the copies the
archive already holds. A Linux arm64 or armhf host has no library in the
release; the command prints the --from-source line for that host and does
not run cargo.

--from-source --target <triple> [--linker <cc>] builds a target from the
pinned source with cargo. rustup target add is used only for
aarch64-unknown-linux-gnu, armv7-unknown-linux-gnueabihf and
aarch64-pc-windows-gnullvm.

--self-test checks the pin, the command line, the hash refusal, the
archive member check and the settings record. It downloads nothing.

Exit status: 0 when the libraries are ready (or the self-test passed);
1 when a check fails; 2 for a command line it cannot read.
"""

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

VERSION = "0.23.2"
ZIP_URL = "https://github.com/AccessKit/accesskit-c/releases/download/0.23.2/accesskit-c-0.23.2.zip"
ZIP_SHA256 = "c7fa84f8f34fca909f662375737a54f98b17cf9eae51fffd94ab3d9f662e3681"
SOURCE_URL = "https://github.com/AccessKit/accesskit-c/archive/refs/tags/0.23.2.tar.gz"
SOURCE_SHA256 = "dca8250dad6b2d21b6bf9bf6bc9e7fa6ede0ff21c0858e952b912d9ddf50ffc8"
# Larger than the release archive (68,334,175 bytes).
DOWNLOAD_LIMIT = 80 * 1024 * 1024
DOWNLOAD_ATTEMPTS = 3
DOWNLOAD_WAITS = (2, 8)
ROOT = Path(__file__).resolve().parents[1]
SETTINGS_NAME = ".oa-accesskit-settings.json"
TOP_DIRECTORY = f"accesskit-c-{VERSION}"
# rustup target add is allowed only for these three. Any other triple must
# already be installed.
RUSTUP_TARGETS = (
    "aarch64-unknown-linux-gnu",
    "armv7-unknown-linux-gnueabihf",
    "aarch64-pc-windows-gnullvm",
)
MACOS_ARM64 = "lib/macos/arm64/static/libaccesskit.a"
MACOS_X86_64 = "lib/macos/x86_64/static/libaccesskit.a"
MACOS_UNIVERSAL = "lib/macos/universal/static/libaccesskit.a"
WINDOWS_DLLS = (
    "lib/windows/x86_64/mingw/shared/accesskit.dll",
    "lib/windows/x86/mingw/shared/accesskit.dll",
)
REQUIRED_FILES = (
    "include/accesskit.h",
    MACOS_ARM64,
    MACOS_X86_64,
    MACOS_UNIVERSAL,
    "lib/ios/arm64/static/libaccesskit.a",
    "lib/ios-simulator/arm64/static/libaccesskit.a",
    "lib/ios-simulator/x86_64/static/libaccesskit.a",
    "lib/linux/x86_64/static/libaccesskit.a",
    "lib/linux/x86/static/libaccesskit.a",
    *WINDOWS_DLLS,
)
# platform.machine() values whose release has no static library.
LINUX_ARM_TRIPLES = {
    "aarch64": "aarch64-unknown-linux-gnu",
    "arm64": "aarch64-unknown-linux-gnu",
    "armv7l": "armv7-unknown-linux-gnueabihf",
    "armv7": "armv7-unknown-linux-gnueabihf",
}


def host_joins_macos(host):
    """Whether this host joins the macOS static libraries with lipo.

    @param host a sys.platform value
    @return true only on macOS
    """
    return host == "darwin"


def required_release_files(host):
    """The release files this host must have before the tree is reused.

    The universal macOS library is joined with lipo, which a Mac has. Another
    host keeps the two slices and does not require the joined file.

    @param host a sys.platform value
    @return paths relative to the unpacked release
    """
    return tuple(
        relative for relative in REQUIRED_FILES
        if relative != MACOS_UNIVERSAL or host_joins_macos(host)
    )


def host_source_triple(host, machine):
    """The cargo triple a host must build because the release has no library for it.

    @param host a sys.platform value
    @param machine platform.machine()
    @return the triple, or None when the release already has this host's library
    """
    if host != "linux":
        return None
    return LINUX_ARM_TRIPLES.get(machine)


def host_source_command(host, machine):
    """The --from-source command a host without a release library has to run.

    @param host a sys.platform value
    @param machine platform.machine()
    @return the command, or None
    """
    triple = host_source_triple(host, machine)
    if triple is None:
        return None
    return f"python3 tools/bootstrap_accesskit.py --from-source --target {triple}"


def prepare_steps(host):
    """The extra steps prepare_release runs after unpacking.

    @param host a sys.platform value
    @return "join" on macOS, and nothing on any other host
    """
    if host_joins_macos(host):
        return ("join",)
    return ()


class BootstrapError(RuntimeError):
    """A pin, archive or build check failed."""


def sha256_file(path):
    """The SHA-256 of a file, as 64 hexadecimal digits.

    @param path the file
    @return the digest
    """
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_hash(path, expected):
    """Stop when a file's SHA-256 is not the pin.

    @param path the file
    @param expected the pin, 64 hexadecimal digits
    """
    found = sha256_file(path)
    if found != expected:
        raise BootstrapError(f"{path.name} SHA-256 {found} does not match the pin {expected}")


def reject_member(name):
    """Stop when an archive member is absolute or climbs out of the tree.

    @param name the member's name
    """
    if name.startswith("/") or name.startswith("\\"):
        raise BootstrapError(f"archive member {name} is absolute")
    parts = Path(name).parts
    if ".." in parts:
        raise BootstrapError(f"archive member {name} leaves the tree")


def download(url):
    """Download url, refusing a body larger than DOWNLOAD_LIMIT.

    @param url the address
    @return the body
    """
    request = urllib.request.Request(url, headers={"User-Agent": "open-annihilation-bootstrap-accesskit"})
    for attempt in range(DOWNLOAD_ATTEMPTS):
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                chunks = []
                total = 0
                while True:
                    chunk = response.read(1 << 20)
                    if not chunk:
                        break
                    total += len(chunk)
                    if total > DOWNLOAD_LIMIT:
                        raise BootstrapError(f"{url} exceeds the download limit")
                    chunks.append(chunk)
                return b"".join(chunks)
        except urllib.error.HTTPError as error:
            if attempt + 1 == DOWNLOAD_ATTEMPTS or (error.code != 429 and error.code < 500):
                raise BootstrapError(f"{url} could not be downloaded (HTTP {error.code})") from error
            time.sleep(DOWNLOAD_WAITS[attempt])
        except (urllib.error.URLError, TimeoutError, ConnectionError) as error:
            if attempt + 1 == DOWNLOAD_ATTEMPTS:
                raise BootstrapError(f"{url} could not be downloaded ({error})") from error
            time.sleep(DOWNLOAD_WAITS[attempt])
    raise BootstrapError(f"{url} could not be downloaded")


def fetch_archive(archive, url, expected):
    """Download archive unless it is already the pinned file.

    @param archive where the bytes are kept
    @param url the address
    @param expected the SHA-256 pin
    """
    if archive.exists() and sha256_file(archive) == expected:
        return
    if archive.exists():
        archive.unlink()
    print(f"Downloading {url}", flush=True)
    archive.write_bytes(download(url))
    require_hash(archive, expected)


def extract_zip(archive, destination):
    """Unpack the release so destination is its top directory.

    @param archive the zip
    @param destination local/deps/accesskit-c-<version>
    """
    staging = destination.parent / f"{destination.name}-partial"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    with zipfile.ZipFile(archive) as bundle:
        for info in bundle.infolist():
            reject_member(info.filename)
        bundle.extractall(staging)
    unpacked = staging / TOP_DIRECTORY
    if not unpacked.is_dir():
        shutil.rmtree(staging)
        raise BootstrapError(f"{archive.name} has no {TOP_DIRECTORY} directory")
    if destination.exists():
        shutil.rmtree(destination)
    unpacked.rename(destination)
    shutil.rmtree(staging)


def extract_source(archive, destination):
    """Unpack the tag's source, which cargo builds from.

    @param archive the gzipped tar
    @param destination the source directory
    """
    staging = destination.parent / f"{destination.name}-partial"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    with tarfile.open(archive, "r:gz") as bundle:
        members = bundle.getmembers()
        for member in members:
            reject_member(member.name)
        if hasattr(tarfile, "data_filter"):
            bundle.extractall(staging, filter="data")
        else:
            bundle.extractall(staging)
    unpacked = staging / TOP_DIRECTORY
    if not (unpacked / "Cargo.toml").is_file():
        shutil.rmtree(staging)
        raise BootstrapError(f"{archive.name} has no Cargo.toml")
    if destination.exists():
        shutil.rmtree(destination)
    unpacked.rename(destination)
    shutil.rmtree(staging)


def lipo_command(arm64, intel, universal):
    """The lipo command that joins the two macOS static libraries.

    @param arm64 the arm64 archive
    @param intel the x86_64 archive
    @param universal where the universal archive is written
    @return the command
    """
    return ["lipo", "-create", str(arm64), str(intel), "-output", str(universal)]


def join_macos(root):
    """Join the macOS static libraries into one universal library.

    @param root the unpacked release
    """
    arm64 = root / MACOS_ARM64
    intel = root / MACOS_X86_64
    universal = root / MACOS_UNIVERSAL
    if not arm64.is_file() or not intel.is_file():
        raise BootstrapError("the release has no macOS static library to join")
    universal.parent.mkdir(parents=True, exist_ok=True)
    command = lipo_command(arm64, intel, universal)
    print("Joining the macOS static libraries", flush=True)
    subprocess.run(command, check=True)


def needs_rustup(triple, installed):
    """Whether rustup target add may fetch this triple.

    @param triple the cargo target
    @param installed the triples rustup already has
    @return true only for a named triple that is not installed
    """
    return triple in RUSTUP_TARGETS and triple not in installed


def cargo_command(source, triple):
    """The cargo command that builds one target from the pinned source.

    @param source the unpacked tag
    @param triple the cargo target
    @return the command
    """
    return [
        "cargo", "build", "--release", "--locked",
        "--manifest-path", str(source / "Cargo.toml"),
        "--target", triple,
    ]


def linker_variable(triple):
    """The environment variable cargo reads a target's linker from.

    @param triple the cargo target
    @return the variable's name
    """
    return "CARGO_TARGET_" + triple.upper().replace("-", "_") + "_LINKER"


def release_ready(root, host=None):
    """Whether this release is already unpacked, joined and recorded.

    @param root the unpacked release
    @param host a sys.platform value; this host when omitted
    @return true when the settings match the pin and every library is present
    """
    if host is None:
        host = sys.platform
    record = root / SETTINGS_NAME
    if not record.is_file():
        return False
    try:
        settings = json.loads(record.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return False
    if settings.get("version") != VERSION or settings.get("zip_sha256") != ZIP_SHA256:
        return False
    if settings.get("source_sha256") != SOURCE_SHA256:
        return False
    return all((root / relative).is_file() for relative in required_release_files(host))


def write_settings(root, built):
    """Record the pin and any source builds.

    @param root the unpacked release
    @param built the source builds already recorded, then this one
    """
    settings = {
        "version": VERSION,
        "zip_sha256": ZIP_SHA256,
        "source_sha256": SOURCE_SHA256,
        "from_source": built,
    }
    (root / SETTINGS_NAME).write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")


def read_settings(root):
    """Read the settings record, or an empty one.

    @param root the unpacked release
    @return the record's object
    """
    record = root / SETTINGS_NAME
    if not record.is_file():
        return {"from_source": []}
    try:
        return json.loads(record.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {"from_source": []}


def build_from_source(deps, triple, linker):
    """Build one target with cargo and copy its library into the release tree.

    @param deps the dependency folder
    @param triple the cargo target
    @param linker a linker to use, or None
    """
    archive = deps / f"accesskit-c-{VERSION}.tar.gz"
    source = deps / f"accesskit-c-{VERSION}-source"
    fetch_archive(archive, SOURCE_URL, SOURCE_SHA256)
    if not (source / "Cargo.toml").is_file():
        extract_source(archive, source)
    else:
        require_hash(archive, SOURCE_SHA256)
    if needs_rustup(triple, installed_rust_targets()):
        print(f"Adding the Rust target {triple}", flush=True)
        subprocess.run(["rustup", "target", "add", triple], check=True)
    command = cargo_command(source, triple)
    env = None
    if linker:
        env = os.environ.copy()
        env[linker_variable(triple)] = linker
    print(f"Building AccessKit for {triple}", flush=True)
    subprocess.run(command, check=True, env=env)
    built = source / "target" / triple / "release"
    destination = deps / TOP_DIRECTORY / "from-source" / triple
    destination.mkdir(parents=True, exist_ok=True)
    copied = False
    for name in ("libaccesskit.a", "libaccesskit.rlib", "accesskit.dll", "libaccesskit.so", "libaccesskit.dylib"):
        library = built / name
        if library.is_file():
            shutil.copy2(library, destination / name)
            copied = True
    if not copied:
        raise BootstrapError(f"cargo built no library for {triple}")
    root = deps / TOP_DIRECTORY
    previous = read_settings(root).get("from_source", [])
    kept = [item for item in previous if item.get("target") != triple]
    kept.append({"target": triple, "linker": linker or ""})
    write_settings(root, kept)


def installed_rust_targets():
    """The triples rustup has installed.

    @return the triple names
    """
    listed = subprocess.run(
        ["rustup", "target", "list", "--installed"], check=True, capture_output=True, text=True)
    return set(listed.stdout.split())


def prepare_release(deps):
    """Fetch, check and unpack the release, and on macOS join the libraries.

    @param deps the dependency folder
    @return the unpacked release
    """
    root = deps / TOP_DIRECTORY
    if release_ready(root):
        print(f"AccessKit {VERSION} is already prepared under {root}", flush=True)
        note_host_source_build(root)
        return root
    archive = deps / f"accesskit-c-{VERSION}.zip"
    deps.mkdir(parents=True, exist_ok=True)
    fetch_archive(archive, ZIP_URL, ZIP_SHA256)
    # The universal library is joined on macOS; every other required file
    # comes from the archive. A tree missing one of those is unpacked again.
    needed = required_release_files(sys.platform)
    archived = [relative for relative in needed if relative != MACOS_UNIVERSAL]
    if any(not (root / relative).is_file() for relative in archived):
        extract_zip(archive, root)
    else:
        require_hash(archive, ZIP_SHA256)
    if "join" in prepare_steps(sys.platform):
        join_macos(root)
    missing = [relative for relative in needed if not (root / relative).is_file()]
    if missing:
        raise BootstrapError("the release is missing " + ", ".join(missing))
    for relative in WINDOWS_DLLS:
        if "mingw/shared/" not in relative:
            raise BootstrapError(f"{relative} is not a mingw DLL")
    previous = read_settings(root).get("from_source", [])
    write_settings(root, previous)
    print(f"AccessKit {VERSION} is ready under {root}", flush=True)
    note_host_source_build(root)
    return root


def note_host_source_build(root):
    """Name the source build a Linux arm host still has to run.

    The release has no library for that host. This does not run cargo.

    @param root the unpacked release
    """
    triple = host_source_triple(sys.platform, platform.machine())
    if triple is None:
        return
    library = root / "from-source" / triple / "libaccesskit.a"
    if library.is_file():
        return
    print(
        "This host's library is not in the release. Run "
        + host_source_command(sys.platform, platform.machine()),
        flush=True,
    )


def read_options(argv):
    """Read the command line.

    @param argv the arguments, without the program name
    @return (options, error). error is set when the command line cannot be read
    """
    parser = argparse.ArgumentParser(description="Fetch the pinned AccessKit C bindings.")
    parser.add_argument("--deps", type=Path, default=ROOT / "local" / "deps",
                        help="where the release is unpacked (default: local/deps)")
    parser.add_argument("--from-source", action="store_true",
                        help="also build --target from the pinned source with cargo")
    parser.add_argument("--target", help="the cargo triple --from-source builds")
    parser.add_argument("--linker", help="the linker cargo uses for --target")
    parser.add_argument("--self-test", action="store_true",
                        help="check the pin and the command line; download nothing")
    args = parser.parse_args(argv)
    if args.linker and not args.from_source:
        return None, "--linker needs --from-source"
    if args.from_source and not args.target:
        return None, "--from-source needs --target"
    if args.target is not None and not args.target.replace("-", "").replace("_", "").isalnum():
        return None, "--target takes a cargo triple"
    return args, None


def run_self_test():
    """Check the pin, the refusals and the layout helpers. Downloads nothing.

    @return 0 when every check holds, 1 otherwise
    """
    failures = []

    def check(condition, message):
        if not condition:
            failures.append(message)

    check(len(ZIP_SHA256) == 64 and all(digit in "0123456789abcdef" for digit in ZIP_SHA256), "zip pin")
    check(len(SOURCE_SHA256) == 64 and all(digit in "0123456789abcdef" for digit in SOURCE_SHA256), "source pin")
    check(DOWNLOAD_LIMIT > 68334175, "download limit covers the release archive")
    check(all("mingw/shared/" in path for path in WINDOWS_DLLS), "Windows DLLs are the mingw shared ones")
    check(all("meson" not in path and "COPYING" not in path for path in REQUIRED_FILES),
          "the layout does not name Meson or the LGPL text")
    check(RUSTUP_TARGETS == (
        "aarch64-unknown-linux-gnu",
        "armv7-unknown-linux-gnueabihf",
        "aarch64-pc-windows-gnullvm",
    ), "rustup targets")
    check(needs_rustup("aarch64-unknown-linux-gnu", set()), "a missing named target is added")
    check(not needs_rustup("aarch64-unknown-linux-gnu", {"aarch64-unknown-linux-gnu"}),
          "an installed named target is not added")
    check(not needs_rustup("x86_64-unknown-linux-gnu", set()), "a release target is not a rustup download")
    command = cargo_command(Path("/src"), "aarch64-unknown-linux-gnu")
    check(command[:4] == ["cargo", "build", "--release", "--locked"], "cargo builds locked and release")
    check("--target" in command and command[-1] == "aarch64-unknown-linux-gnu", "cargo names the triple")
    check(linker_variable("aarch64-unknown-linux-gnu") == "CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER",
          "linker variable")
    lipo = lipo_command(Path("arm64.a"), Path("x86_64.a"), Path("universal.a"))
    check(lipo[:2] == ["lipo", "-create"] and lipo[-2] == "-output", "lipo command")
    check(prepare_steps("linux") == (), "a Linux host does not run lipo")
    check(prepare_steps("win32") == (), "a Windows host does not run lipo")
    check(prepare_steps("darwin") == ("join",), "a macOS host joins the libraries")
    check(MACOS_UNIVERSAL not in required_release_files("linux"),
          "a Linux host does not require the universal library")
    check(MACOS_UNIVERSAL in required_release_files("darwin"),
          "a macOS host requires the universal library")
    check(host_source_triple("linux", "aarch64") == "aarch64-unknown-linux-gnu",
          "linux arm64 names its triple")
    check(host_source_triple("linux", "arm64") == "aarch64-unknown-linux-gnu", "linux arm64 alias")
    check(host_source_triple("linux", "armv7l") == "armv7-unknown-linux-gnueabihf",
          "linux armhf names its triple")
    check(host_source_triple("linux", "x86_64") is None, "linux x86_64 uses the release")
    check(host_source_triple("darwin", "arm64") is None, "macOS does not take a source triple")
    check(host_source_command("linux", "aarch64")
          == "python3 tools/bootstrap_accesskit.py --from-source --target aarch64-unknown-linux-gnu",
          "linux arm64 names the source command")
    options, error = read_options([])
    check(error is None and options is not None and not options.from_source, "the default command prepares the release")
    _, error = read_options(["--from-source"])
    check(error == "--from-source needs --target", "from-source needs a target")
    _, error = read_options(["--linker", "cc"])
    check(error == "--linker needs --from-source", "linker needs from-source")
    options, error = read_options(
        ["--from-source", "--target", "armv7-unknown-linux-gnueabihf", "--linker", "arm-linux-gnueabihf-gcc"])
    check(error is None and options.target == "armv7-unknown-linux-gnueabihf"
          and options.linker == "arm-linux-gnueabihf-gcc", "from-source command")
    with tempfile.TemporaryDirectory(prefix="A09-accesskit-") as temporary:
        folder = Path(temporary)
        wrong = folder / "archive.bin"
        wrong.write_bytes(b"not the release")
        try:
            require_hash(wrong, ZIP_SHA256)
            check(False, "a mismatched hash was accepted")
        except BootstrapError as mismatch:
            check("does not match the pin" in str(mismatch), "a mismatched hash is refused")
        evil = folder / "evil.zip"
        with zipfile.ZipFile(evil, "w") as bundle:
            bundle.writestr("../outside.txt", b"no")
        try:
            extract_zip(evil, folder / "out")
            check(False, "a member that climbs out was unpacked")
        except BootstrapError as climbed:
            check("leaves the tree" in str(climbed), "a member that climbs out is refused")
        good = folder / "good.zip"
        with zipfile.ZipFile(good, "w") as bundle:
            bundle.writestr(f"{TOP_DIRECTORY}/include/accesskit.h", b"header")
        destination = folder / TOP_DIRECTORY
        extract_zip(good, destination)
        check((destination / "include" / "accesskit.h").read_bytes() == b"header", "the zip unpacks under its name")
        (destination / SETTINGS_NAME).write_text(json.dumps({
            "version": VERSION, "zip_sha256": ZIP_SHA256, "source_sha256": SOURCE_SHA256, "from_source": [],
        }), encoding="utf-8")
        check(not release_ready(destination), "a tree missing a library is not reused")
        for relative in REQUIRED_FILES:
            path = destination / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"library")
        check(release_ready(destination), "a complete tree with the pin is reused")
        (destination / MACOS_UNIVERSAL).unlink()
        check(release_ready(destination, "linux"),
              "a Linux host reuses a tree without the universal library")
        check(not release_ready(destination, "darwin"),
              "a macOS host does not reuse a tree without the universal library")
        (destination / SETTINGS_NAME).write_text(json.dumps({
            "version": VERSION, "zip_sha256": "0" * 64, "source_sha256": SOURCE_SHA256, "from_source": [],
        }), encoding="utf-8")
        check(not release_ready(destination), "a different archive hash is not reused")
    if failures:
        print("bootstrap_accesskit self-test: " + "; ".join(failures), file=sys.stderr)
        return 1
    print("bootstrap_accesskit self-test: pin, hash refusal, mingw DLLs, cargo command, "
          "settings reuse, non-Mac host")
    return 0


def main(argv=None):
    """Prepare AccessKit, or run the self-test.

    @param argv the arguments, without the program name
    @return the exit status
    """
    options, error = read_options(sys.argv[1:] if argv is None else argv)
    if error is not None:
        print(f"bootstrap_accesskit.py: {error}", file=sys.stderr)
        return 2
    if options.self_test:
        return run_self_test()
    try:
        prepare_release(options.deps)
        if options.from_source:
            build_from_source(options.deps, options.target, options.linker)
    except BootstrapError as failure:
        print(f"bootstrap_accesskit.py: {failure}", file=sys.stderr)
        return 1
    except (subprocess.CalledProcessError, OSError) as failure:
        print(f"bootstrap_accesskit.py: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
