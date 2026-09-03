{
  description = "Development environment for tio-gui";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
    in {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "tio-gui";
        version = "0.1.0";
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
          vte-gtk4
        ];

        postFixup = ''
          wrapProgram "$out/bin/tio-gui" \
            --set LOCALE_ARCHIVE ${pkgs.glibcLocalesUtf8}/lib/locale/locale-archive \
            --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.tio ]}
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
          vte-gtk4
          tio
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
