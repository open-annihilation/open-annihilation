# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# SDL3 for a Windows build of the engine.
#
# nixpkgs' own SDL3 is not used, unlike zlib and FreeType, because this tree
# needs a static library with its own patch set applied. Everything else about
# the build is the cross set's: the compiler and the C run-time library come
# from the set the caller passes, so what this links is what the game links.
#
# The source is nixpkgs', so the version follows the flake's nixpkgs pin.
# tools/sdl-patches/ is keyed to that version, and a pin that moves past it
# fails at patch time rather than at run time.
#
# @param pname the derivation's name, which says which Windows it is for
# @param description the same, in words
# @param targetFlags the instruction set and code generation the engine is
#        built with for this target, so that the library and the engine agree
{ lib, stdenv, cmake, ninja, sdl3, pname, description, targetFlags }:
stdenv.mkDerivation {
  inherit pname;

  # The source is nixpkgs' SDL3, the same one the other systems build, so the
  # version follows the flake's nixpkgs pin rather than a pin of its own.
  inherit (sdl3) version src;

  patches = [
    ../tools/sdl-patches/3.4.16/0001-steam-deck-trackpad-haptics.patch
  ];

  nativeBuildInputs = [
    cmake
    ninja
  ];

  # A compiler for another system cannot link a program for this one, so cmake
  # has to be told which system it is building for rather than left to try
  # running what it builds.
  cmakeFlags = [
    "-DCMAKE_SYSTEM_NAME=Windows"
    "-DSDL_SHARED=OFF"
    "-DSDL_STATIC=ON"
    "-DSDL_TEST_LIBRARY=OFF"
    "-DSDL_TESTS=OFF"
    "-DSDL_EXAMPLES=OFF"
  ];

  CFLAGS = lib.concatStringsSep " " targetFlags;
  CXXFLAGS = lib.concatStringsSep " " targetFlags;

  # Nothing here is a program for the build machine, so nothing here is run on
  # it.
  doCheck = false;

  # nixpkgs compares these against the system the derivation is built *for*,
  # which is Windows: this is a cross build whose host platform is the target,
  # however much the build machine is a Linux one.
  meta = {
    inherit description;
    license = lib.licenses.zlib;
    platforms = lib.platforms.windows;
  };
}
