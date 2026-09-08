{
  description = "Development environment for tio-gui";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      version = pkgs.lib.removeSuffix "\n" (builtins.readFile ./VERSION);
    in {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "tio-gui";
        inherit version;
        src = self;

        nativeBuildInputs = with pkgs; [
          cmake
          gettext
          ninja
          pkg-config
          wrapGAppsHook4
        ];

        buildInputs = with pkgs; [
          gtk4
          json-glib
          libsoup_3
          pcre2
          vte-gtk4
        ];

        postFixup = ''
          wrapProgram "$out/bin/tio-gui" \
            --set LOCALE_ARCHIVE ${pkgs.glibcLocalesUtf8}/lib/locale/locale-archive \
            --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.tio pkgs.lrzsz pkgs.bubblewrap pkgs.quickjs pkgs.lua5_4 ]}
        '';
      };

      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          cmake
          gettext
          ninja
          pkg-config
          gcc
          gdb
          clang-tools
          glibcLocales
          gtk4
          json-glib
          libsoup_3
          pcre2
          vte-gtk4
          tio
          lrzsz
          mosquitto
          python3
          xorg-server
          xdotool
          bubblewrap
          quickjs
          lua5_4
        ];

        shellHook = ''
          export LOCALE_ARCHIVE=${pkgs.glibcLocales}/lib/locale/locale-archive
          export LANG=zh_TW.UTF-8
          export LC_ALL=zh_TW.UTF-8
          export LANGUAGE=zh_TW
        '';
      };
    };
}
