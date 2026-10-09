# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The Windows 95 build: the same engine, cross-compiled for that system.
#
# The toolchain and the C libraries both come from the cross set, which
# nix/win95-toolchain.nix rebuilt against the msvcrt20 C run-time library and
# the Win32 thread backend. zlib and FreeType are nixpkgs' own from that set;
# only SDL3 is built here (nix/win95-sdl.nix), for its patch and its static
# library.
#
# cmake/OaWindows95.cmake does the rest — it turns off what Windows 95 has not
# got, pins the headers at 0x0400, links the stand-ins the executables need, and
# uses msvcrt20, the C run-time library that ships on both RTM and OSR2.
#
# Takes the text fonts, because the engine reads them beside the executable the
# same way it does everywhere else.
#
{ lib, stdenv, cmake, ninja, zlib, freetype, sdl3, src, textFonts, version }:
{
  game = stdenv.mkDerivation {
    pname = "open-annihilation-win95";
    inherit version src;

    nativeBuildInputs = [
      cmake
      ninja
    ];

    # The cmake setup hook puts the host inputs' prefixes on CMAKE_PREFIX_PATH,
    # so nothing has to name them on the command line.
    buildInputs = [
      zlib
      freetype
      sdl3
    ];

    cmakeFlags = [
      "-DOA_WINDOWS_95=ON"
      "-DOA_X86_FLOAT=fpu"
      "-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/i686-w64-mingw32.cmake"
      "-DOA_TEXT_FONTS_DIR=${textFonts}"
      # Nothing here runs on the build machine, so no test may be configured.
      "-DBUILD_TESTING=OFF"
    ];

    buildFlags = [
      "oa-game"
      "oa-tool"
    ];

    # There is no CMake install configured for this target either.
    installPhase = ''
      runHook preInstall

      mkdir -p $out/bin/fonts
      install -Dm755 oa-tool.exe $out/bin/oa-tool.exe
      install -Dm755 open-annihilation.exe $out/bin/open-annihilation.exe
      # bundled_font_directory() reads the fonts from the executable's folder
      # (src/platform/text-font/src/font_directory.cpp).
      install -Dm644 -t $out/bin/fonts ${textFonts}/*.ttf ${textFonts}/*.otf

      runHook postInstall
    '';

    meta = with lib; {
      description = "Open Source port of the Total Annihilation & TA: Kingdoms engines, for Windows 95";
      homepage = "https://coreprime.net/";
      # nixpkgs compares this against the system the derivation is built for,
      # which is Windows: this is a cross build whose host platform is the
      # target, however much the build machine is a Linux one.
      platforms = platforms.windows;
      license = licenses.gpl3Plus;
      mainProgram = "open-annihilation.exe";
    };
  };
}
