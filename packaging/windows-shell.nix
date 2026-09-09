# Linux host tools for packaging the native Windows serial edition.
let
  flake = builtins.getFlake (toString ../.);
  pkgs = import flake.inputs.nixpkgs { system = "x86_64-linux"; };
  cross = pkgs.pkgsCross.mingwW64;
in pkgs.mkShell {
  packages = [
    cross.stdenv.cc pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.nsis
    pkgs.glib.dev pkgs.python3 pkgs.zstd
  ];
  TIO_WINDOWS_LDFLAGS = "-L${cross.windows.mcfgthreads}/lib";
  TIO_WINDOWS_EXTRA_DLL_DIR = "${cross.windows.mcfgthreads}/bin";
}
