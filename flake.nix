{
  description = "Open Annihilation: the engine, and its Windows 95 build";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/b1b875982b17dabde9b4a37f3e229e74913e6db3";

  outputs = { self, nixpkgs }:
    let
      # The systems this builds on. macOS is to come: the engine expects its
      # files inside an application bundle there, which the install below does
      # not make, and nix refuses a package whose platforms do not name the
      # system it is asked for.
      systems = [ "x86_64-linux" "aarch64-linux" ];

      # The toolchain overlay goes on the Windows 95 set alone. It changes
      # every MinGW set, so a build for a current Windows must not see it: it
      # would get the Windows 95 C run-time library and Win32 version, which
      # is a mistake no compiler would report.
      mkPkgs = system: import nixpkgs { inherit system; };
      mkWin95Pkgs = system:
        import nixpkgs {
          inherit system;
          overlays = [ (import ./nix/win95-toolchain.nix) ];
        };

      # The version the executables carry: the project's own, from the one
      # place it is written down, then the source's date and revision, so that
      # a build says which tree it came from. self.lastModifiedDate is the
      # source's date whether the tree is committed or not, and the dirty
      # revision is used where there is no clean one.
      #
      # builtins.split and not builtins.match: a pattern with .* at either end
      # of a file this size drives Nix's regular expressions past their stack.
      version =
        let
          parts = builtins.split
            "project\\(open_annihilation VERSION ([0-9.]+)"
            (builtins.readFile ./CMakeLists.txt);
          base =
            if builtins.length parts < 3 then "0" else builtins.head (builtins.elemAt parts 1);
          revision = if self ? shortRev then self.shortRev else self.dirtyShortRev;
        in
        "${base}.${self.lastModifiedDate}-g${revision}";

      # The engine as this system builds it. Its source is this tree, so what is
      # built is what is checked out rather than a release fetched from GitHub.
      linux = pkgs: pkgs.callPackage ./nix/linux.nix { src = self; inherit version; };

      # A Windows build of the tree, cross-compiled from a cross set.
      #
      # Everything taken from the set is the same whatever the Windows is:
      # static, because the engine links -static and nixpkgs builds the shared
      # library only, so without an archive to pick the link falls back to the
      # DLL's import library and the executable needs libfreetype-6.dll,
      # zlib1.dll and libgcc_s_dw2-1.dll beside it.
      #
      # nixpkgs' FreeType carries the codecs for compressed font formats, which
      # this engine does not read: it draws TrueType and CFF. Leaving them in
      # means building libpng, brotli, bzip2 and harfbuzz as well.
      windowsBuild =
        pkgs: cross: flags:
        {
          pname,
          windowsName,
          toolchain,
          definitions,
        }:
        let
          zlib = cross.zlib.override { shared = false; };
          freetype = cross.freetype.overrideAttrs (old: {
            propagatedBuildInputs = [ zlib ];
            configureFlags = (old.configureFlags or [ ]) ++ [
              "--disable-shared"
              "--enable-static"
              "--without-bzip2"
              "--without-png"
              "--without-harfbuzz"
              "--without-brotli"
            ];
          });
          sdl3 = cross.callPackage ./nix/windows-sdl.nix {
            pname = "SDL3-${pname}";
            description = "The SDL3 library, built for ${windowsName}";
            targetFlags = flags;
          };
        in
        cross.callPackage ./nix/windows.nix {
          src = self;
          inherit version freetype zlib sdl3 toolchain definitions;
          inherit pname;
          description = "Open Source port of the Total Annihilation & TA: Kingdoms engines, for ${windowsName}";
          textFonts = (linux pkgs).textFonts;
        };

    in
    {
      packages = nixpkgs.lib.genAttrs systems (
        system:
        let
          pkgs = mkPkgs system;

          # The instruction set cmake/toolchains/i686-w64-mingw32.cmake builds
          # the engine with: the i686 instruction set and no SSE2, which a
          # Pentium II has not got either; and without the identical-code
          # folding that can merge a member function with a free function of
          # the same body, so that a call through the merged function passes
          # its arguments where it is not looking for them.
          i686Flags = [ "-march=i686" "-mno-sse2" "-fno-ipa-icf" ];
        in
        {
          # `nix build` with no attribute builds the game for this system.
          default = (linux pkgs).game;
          inherit (linux pkgs) textFonts;

          # The Windows builds, cross-compiled from this system.
          windows = (windowsBuild pkgs pkgs.pkgsCross.mingwW64 [ ] {
            pname = "open-annihilation-windows";
            windowsName = "Windows";
            toolchain = "x86_64-w64-mingw32.cmake";
            definitions = [ ];
          }).game;
          windows-i686 = (windowsBuild pkgs pkgs.pkgsCross.mingw32 i686Flags {
            pname = "open-annihilation-windows-i686";
            windowsName = "32-bit Windows";
            toolchain = "i686-w64-mingw32.cmake";
            definitions = [ ];
          }).game;

          # `nix build .#win95` builds the executables for Windows 95, from the
          # one set whose toolchain nix/win95-toolchain.nix rebuilt.
          win95 =
            let
              win95Pkgs = mkWin95Pkgs system;
            in
            (windowsBuild win95Pkgs win95Pkgs.pkgsCross.mingw32 i686Flags {
              pname = "open-annihilation-win95";
              windowsName = "Windows 95";
              toolchain = "i686-w64-mingw32.cmake";
              definitions = [ "-DOA_WINDOWS_95=ON" "-DOA_X86_FLOAT=fpu" ];
            }).game;
        }
      );
    };
}
