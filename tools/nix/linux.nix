# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The engine as it is built on this system: the game, the tool beside it, the
# ten fonts cmake/OaTextFonts.cmake copies next to them, and a desktop entry.
#
# This is the flake's "default" output. It builds the tree it is in, rather
# than a release fetched from elsewhere, which is the one thing about it a
# packaging of the project would do differently.
#
{ pkgs, src, version }:
let
  # Barlow and Barlow Condensed 1.422, the modern look's interface faces: the
  # static faces as the project's release tag holds them, each pinned by the
  # SHA-256 tools/bootstrap_text_fonts.py pins it by.
  barlowRelease = "https://github.com/jpt/barlow/raw/1.422/fonts/ttf";
  barlowFaces = [
    { name = "Barlow-Regular.ttf"; sha256 = "a72240db047b468c957834cdbd607f013da32edee62d096f5fe0a4c2dc8433ce"; }
    { name = "Barlow-Medium.ttf"; sha256 = "5a26763b11d11a437ab9f1c46281d3615ab1f6b93c90b8fa181e03978971b9e9"; }
    { name = "Barlow-SemiBold.ttf"; sha256 = "cafab6a24fc131fd2dbbe56b9cf8daa616db3d5929fa89c35acba690e620585e"; }
    { name = "Barlow-Bold.ttf"; sha256 = "b5af7fa4dd961a259b075cd23094cd4e961afb6a0b5ac5cf0e3395fc54b7a4b5"; }
    { name = "BarlowCondensed-SemiBold.ttf"; sha256 = "0dae49e31a5dd598184b8ed18ed877f9280e07c4cc3fa7c2cad25a5e7d8a3ad4"; }
    { name = "BarlowCondensed-Bold.ttf"; sha256 = "e4fd0ebbc8b4346229f5c409f34e052b5e0d7fbcb7a7c4c30f7c5d334b4953b2"; }
  ];
  barlowFiles = map (face: {
    inherit (face) name;
    file = pkgs.fetchurl {
      url = "${barlowRelease}/${face.name}";
      inherit (face) sha256;
    };
  }) barlowFaces;

  # The ten fonts. Nine come from projects that publish them.
  # NotoSansCJKsc-Bold-Endonyms.otf is the Simplified Chinese face of Noto
  # Sans CJK's Bold collection, cut to the languages' own names and the
  # language notice (tools/text-fonts/endonyms.txt). The full face travels
  # with the Simplified Chinese language pack, not with the game. Nix's
  # fontTools need not match the pinned one, so this cut's bytes may differ
  # from the pin, which this build does not check.
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
      python3 ${src}/tools/bootstrap_text_fonts.py --cut-endonym-face \
        ${pkgs.noto-fonts-cjk-sans-static}/share/fonts/opentype/noto-cjk/NotoSansCJK-Bold.ttc \
        ${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSans-Bold.ttf \
        ${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSans.ttf \
        $out/NotoSansCJKsc-Bold-Endonyms.otf
      ${pkgs.lib.concatMapStrings (face: ''
        install -m644 ${face.file} $out/${face.name}
      '') barlowFiles}
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
      "-DOA_SELF_UPDATE=OFF"
      "-DOA_PACKAGER_NAME=Nix"
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
      # The registries the game ships with, read from the executable's folder.
      install -Dm644 -t $out/bin/registries ${src}/registries/*.yaml

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
