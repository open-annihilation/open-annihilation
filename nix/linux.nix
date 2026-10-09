# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The engine as it is built on this system: the game, the tool beside it, the
# four fonts cmake/OaTextFonts.cmake copies next to them, and a desktop entry.
#
# This is the flake's "default" output. It builds the tree it is in, rather
# than a release fetched from elsewhere, which is the one thing about it a
# packaging of the project would do differently.
#
{ pkgs, src, version }:
let
  # The four fonts, three of which come from projects that publish them.
  # NotoSansCJKsc-Bold.otf is published nowhere, being the Simplified Chinese
  # face (index 2) of Noto Sans CJK's Bold collection; upstream's
  # tools/bootstrap_text_fonts.py extracts it with fonttools, and the cut it
  # then makes is only a size saving, so the whole face travels instead.
  textFonts = pkgs.stdenvNoCC.mkDerivation {
    pname = "open-annihilation-text-fonts";
    version = "2.004";

    nativeBuildInputs = [ (pkgs.python3.withPackages (ps: [ ps.fonttools ])) ];

    dontUnpack = true;
    dontConfigure = true;
    dontBuild = true;

    installPhase = ''
      runHook preInstall

      mkdir -p $out
      install -m644 ${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSans.ttf $out/
      install -m644 ${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSans-Bold.ttf $out/
      install -m644 ${pkgs.noto-fonts-monochrome-emoji}/share/fonts/noto/NotoEmoji.ttf $out/
      python3 - "$out" <<'PY'
      import sys
      from fontTools.ttLib import TTFont
      TTFont(
          "${pkgs.noto-fonts-cjk-sans-static}/share/fonts/opentype/noto-cjk/NotoSansCJK-Bold.ttc",
          fontNumber=2,
          recalcTimestamp=False,
      ).save(sys.argv[1] + "/NotoSansCJKsc-Bold.otf")
      PY

      runHook postInstall
    '';
  };
in
{
  inherit textFonts;

  game = pkgs.stdenv.mkDerivation {
    pname = "open-annihilation";
    inherit version src;

    nativeBuildInputs = [
      pkgs.cmake
      pkgs.copyDesktopItems
    ];

    buildInputs = [
      pkgs.zlib
      pkgs.sdl3
      pkgs.ffmpeg-headless
      pkgs.freetype
    ];

    # The fonts cmake/OaTextFonts.cmake copies beside the game.
    cmakeFlags = [
      "-DOA_TEXT_FONTS_DIR=${textFonts}"
    ];

    buildFlags = [
      "oa-game"
      "oa-tool"
    ];

    # There is no CMake install configured.
    installPhase = ''
      runHook preInstall

      install -Dm755 oa-tool $out/bin/oa-tool
      install -Dm755 open-annihilation $out/bin/open-annihilation
      install -Dm644 ${src}/branding/open-annihilation-256.png \
        $out/share/icons/hicolor/256x256/apps/open-annihilation.png
      # bundled_font_directory() reads the fonts from the executable's folder
      # (src/platform/text-font/src/font_directory.cpp).
      install -Dm644 -t $out/bin/fonts ${textFonts}/*.ttf ${textFonts}/*.otf

      runHook postInstall
    '';

    desktopItems = [
      (pkgs.makeDesktopItem {
        name = "open-annihilation";
        exec = "open-annihilation";
        icon = "open-annihilation";
        desktopName = "Open Annihilation";
        categories = [ "Game" "StrategyGame" ];
        comment = "Open Source port of the Total Annihilation & TA: Kingdoms engines";
      })
    ];

    meta = with pkgs.lib; {
      description = "Open Source port of the Total Annihilation & TA: Kingdoms engines";
      homepage = "https://coreprime.net/";
      platforms = platforms.linux;
      license = licenses.gpl3Plus;
      mainProgram = "open-annihilation";
    };
  };
}
