#!/usr/bin/env bash
# Run on an x86_64 Linux builder; keep all generated files in the chosen root.
set -euo pipefail
source_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
work_root=${1:?Usage: package.sh /absolute/build/root}
case "$work_root" in /*) ;; *) echo 'Build root must be absolute' >&2; exit 2 ;; esac
mkdir -p "$work_root"/{cache,dist,logs,tmp}
name="tio-gui-package-$(id -u)-$$"
trap 'podman stop --time 10 "$name" >/dev/null 2>&1 || true' EXIT INT TERM
podman run --rm --name "$name" --security-opt label=disable \
  -e TIO_GUI_SKIP_FINALIZE="${TIO_GUI_SKIP_FINALIZE:-0}" \
  -v "$source_root:/source:ro" -v "$work_root:/work:rw" \
  "${TIO_GUI_BUILD_IMAGE:-docker.io/library/ubuntu@sha256:224a1869083a311ef3f13648a154ba79832fbef6364d31493642ca03082da254}" \
  bash /source/packaging/linux/package-container.sh
