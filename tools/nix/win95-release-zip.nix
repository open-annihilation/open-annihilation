{ stdenvNoCC, open-annihilation, zip }:
stdenvNoCC.mkDerivation(finalAttrs: let
  baseFolder = "${open-annihilation.pname}-v${finalAttrs.version}";
in {
  inherit (open-annihilation) version;

  pname = "${open-annihilation.pname}-release-zip";

  nativeBuildInputs = [
    zip
  ];

  dontUnpack = true;

  buildPhase = ''
    runHook preBuild

    verdir=${baseFolder}
    cp -R ${open-annihilation}/bin ${baseFolder}
    zip -rq0 ${baseFolder}.zip ${baseFolder}

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    install -Dm644 ${baseFolder}.zip $out/${baseFolder}.zip

    runHook postInstall
  '';
})