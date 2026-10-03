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
      serialAgent = hostSystem:
        let hostPkgs = import nixpkgs { system = hostSystem; };
        in hostPkgs.stdenv.mkDerivation {
          pname = "tio-serial-agent";
          inherit version;
          src = self;
          nativeBuildInputs = with hostPkgs; [ cmake ninja pkg-config python3 makeWrapper ];
          nativeCheckInputs = [ hostPkgs.openssl ];
          buildInputs = with hostPkgs; [ glib json-glib ];
          cmakeFlags = [ "-DTIO_GUI_BUILD_DESKTOP=OFF" "-DTIO_GUI_BUILD_AGENT=ON" ];
          doCheck = true;
          postInstall = ''
            makeWrapper ${hostPkgs.python3}/bin/python3 "$out/bin/tio-remote" \
              --add-flags "$out/share/tio-gui/remote/gateway.py" \
              --add-flags "--agent $out/bin/tio-serial-agent"
          '';
          meta = with hostPkgs.lib; {
            description = "Headless serial transport for tio-gui remote clients";
            homepage = "https://github.com/keithxc/tio-gui";
            license = licenses.gpl3Only;
            mainProgram = "tio-serial-agent";
            platforms = [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" ];
          };
        };
    in {
      devShells.aarch64-darwin.default = import ./nix/macos-shell.nix {
        pkgs = import nixpkgs { system = "aarch64-darwin"; };
        nixpkgsRevision = nixpkgs.rev or null;
      };

      packages.${system} = {
        serial-agent = serialAgent system;
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
          meta = with pkgs.lib; {
            description = "Serial debugging workspace for embedded developers";
            homepage = "https://github.com/keithxc/tio-gui";
            license = licenses.gpl3Only;
            mainProgram = "tio-gui";
            platforms = [ "x86_64-linux" ];
          };
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

      packages.aarch64-linux.serial-agent = serialAgent "aarch64-linux";
      packages.aarch64-darwin.serial-agent = serialAgent "aarch64-darwin";

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
