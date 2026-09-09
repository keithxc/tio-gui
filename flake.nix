{
  description = "Development environment for tio-gui";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  inputs.nix-appimage = {
    url = "github:ralismark/nix-appimage/7946addbc0d97e358a6d7aefe5e82310f0fe6b18";
    inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = { self, nixpkgs, nix-appimage }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      version = pkgs.lib.removeSuffix "\n" (builtins.readFile ./VERSION);
    in {
      packages.${system} = {
        default = pkgs.stdenv.mkDerivation {
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

        appimage =
          let
            fonts = pkgs.makeFontsConf {
              fontDirectories = [ pkgs.dejavu_fonts pkgs.noto-fonts-cjk-sans ];
            };
            # Software rendering avoids depending on a host-specific OpenGL stack.
            launcher = pkgs.writeShellScriptBin "tio-gui" ''
              export GSK_RENDERER="''${GSK_RENDERER:-cairo}"
              export FONTCONFIG_FILE=${fonts}
              export FONTCONFIG_PATH=${pkgs.fontconfig.out}/etc/fonts
              exec ${self.packages.${system}.default}/bin/tio-gui "$@"
            '';
            entry = pkgs.runCommand "tio-gui-appimage-entry" { } ''
              mkdir -p $out/bin
              ln -s ${launcher}/bin/tio-gui $out/bin/tio-gui
              ln -s ${self.packages.${system}.default}/share $out/share
            '';
          in nix-appimage.lib.${system}.mkAppImage {
            program = "${entry}/bin/tio-gui";
            name = "tio-gui-${version}-linux-x86_64.AppImage";
            squashfsArgs = [ "-comp" "xz" ];
          };
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
