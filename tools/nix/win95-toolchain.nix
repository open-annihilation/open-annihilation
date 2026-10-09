# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The two things about nixpkgs' mingw that are wrong for Windows 95, as an
# overlay on the package set the Windows 95 build comes out of:
#
#   import nixpkgs { overlays = [ (import ./tools/nix/win95-toolchain.nix) ]; }
#
# Applied at import rather than to one stdenv, so that the whole cross set —
# the compiler, the C run-time library, and every package built from them,
# including every transitive dependency — is built the Windows 95 way. Doing
# it with overrideCC instead would leave each package's own dependencies on
# the unpatched toolchain, and something deep in that graph linking mcfgthread
# would only show up as a load failure on the guest.
#
# Threads. nixpkgs' mingw is built with --enable-threads=mcf, and mcfgthread
# imports ntdll's keyed events (NtCreateSection, NtWaitForKeyedEvent and so
# on). Windows 95 has no ntdll at all, so a program linked against that C++
# run-time library cannot load there whatever else it avoids. nixpkgs keeps
# the choice in a `threads` attribute, which gcc reads as threadsCross; its own
# comment names win32 as the alternative and null as the package for it, since
# Win32 threads come from the system rather than a library.
#
# The C run-time library. Windows 95 has msvcrt20, and the toolchain otherwise
# links the msvcrt that ships with modern Windows. The choice is
# --with-default-msvcrt, which the mingw-w64 headers and the CRT each take as
# their `crt` argument, defaulting to stdenv.hostPlatform.libc. It is set here
# rather than passed as -mcrtdll= at each link because cmake's symbol checks
# *link* a program: a check against the newer library finds functions msvcrt20
# has not got, takes the path that uses them, and the real link fails
# afterwards. With the choice built into the toolchain every check and every
# link agree.
#
# The Windows version the headers declare. mingw-w64's headers default
# _WIN32_WINNT to 0xa00 — Windows 10 — so every declaration guarded by a
# version is present, and anything in the dependency graph is free to reach for
# a call Windows 95 has not got. brotli's command-line tool calling fopen_s and
# _sopen_s is what that looks like in practice; against msvcrt20 they are
# simply undefined and the link fails. Setting the default to 0x0400 removes
# those declarations, so the problem does not arise rather than being found one
# package at a time. The CRT does not take the option: it decides what is
# declared, not what is implemented.
#
# The platform's own libc cannot carry the first of these: libcCross is chosen
# by comparing hostPlatform.libc against a list of known names and throws on
# anything else, so "msvcrt20" is not a value it can take. Overriding the two
# packages whose `crt` decides it reaches the same place, because libcCross
# *is* windows.mingw_w64.
#
final: prev:
let
  # The value is not compared, only the flag's name, so that a change of
  # default upstream fails in effect rather than silently leaving the flag
  # alone and producing a library Windows 95 has not got.
  useMsvcrt20 = flags:
    map
      (flag:
        if builtins.isString flag && builtins.match "--with-default-msvcrt=.*" flag != null
        then "--with-default-msvcrt=msvcrt20"
        else flag)
      flags;
in
# The attributes exist only for a MinGW target; on any other set this overlay
# does nothing, which is what lets it be applied to the whole nixpkgs and reach
# pkgsCross from there.
prev.lib.optionalAttrs prev.stdenv.hostPlatform.isMinGW {
  windows = prev.windows // {
    mingw_w64 = prev.windows.mingw_w64.overrideAttrs (old: {
      configureFlags = useMsvcrt20 old.configureFlags;
    });
    mingw_w64_headers = prev.windows.mingw_w64_headers.overrideAttrs (old: {
      configureFlags = useMsvcrt20 old.configureFlags ++ [
        "--with-default-win32-winnt=0x0400"
      ];
    });
  };

  threads = {
    model = "win32";
    package = null;
  };
}
