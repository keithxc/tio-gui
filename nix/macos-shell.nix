# Shared by this project and mynix; all third-party build/runtime inputs are Nix-managed.
{ pkgs, nixpkgsRevision ? null }:
let
  lib = pkgs.lib;
  runtimePackages = with pkgs; [ gtk4 glib gdk-pixbuf json-glib pcre2 libvterm-neovim lrzsz lua5_4 quickjs hicolor-icon-theme ];
  manifest = pkgs.writeText "tio-macos-runtime.json" (builtins.toJSON {
    manager = "nix";
    nixpkgs = toString pkgs.path;
    inherit nixpkgsRevision;
    helpers = { sz = "${pkgs.lrzsz}/bin/sz"; rz = "${pkgs.lrzsz}/bin/rz"; lua = "${pkgs.lua5_4}/bin/lua"; qjs = "${pkgs.quickjs}/bin/qjs"; };
    pixbuf = "${pkgs.gdk-pixbuf}";
    queryLoaders = "${pkgs.gdk-pixbuf.dev}/bin/gdk-pixbuf-query-loaders";
    compileSchemas = "${pkgs.glib.dev}/bin/glib-compile-schemas";
    dataRoots = map toString [ pkgs.gtk4 pkgs.glib.dev pkgs.hicolor-icon-theme ];
    packages = map (p: {
      name = p.pname or p.name;
      version = p.version or "";
      path = toString p;
      source = p.src.url or (p.src.urls or []);
      homepage = p.meta.homepage or "";
      license = map (l: l.spdxId or l.shortName or "unknown") (lib.toList (p.meta.license or []));
    }) runtimePackages;
  });
in pkgs.mkShell {
  packages = with pkgs; [ cmake ninja pkg-config gettext python3 ] ++ runtimePackages;
  TIO_MACOS_RUNTIME = manifest;
  # Only the unbundled Nix Lua interpreter needs these separate read-only roots.
  TIO_PLUGIN_NIX_LIBS = "${lib.getLib pkgs.readline}:${lib.getLib pkgs.ncurses}";
  # Use the native Apple SDK/compiler for the distributable .app and notarization.
  shellHook = ''
    export CC=/usr/bin/clang
    export CXX=/usr/bin/clang++
    export DEVELOPER_DIR="$(env -u DEVELOPER_DIR /usr/bin/xcode-select -p)"
    export SDKROOT="$(env -u SDKROOT /usr/bin/xcrun --sdk macosx --show-sdk-path)"
    export MACOSX_DEPLOYMENT_TARGET=26.0
  '';
}
