#!/usr/bin/env sh
set -eu
export GTK_A11Y=none
export GTK_IM_MODULE=simple
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
cd "$repo"
exports=$(python3 packaging/build-paths.py --environment)
eval "$exports"
default_work="$TIO_WORK_DIR"
build=${1:-$default_work/cmake}
build=$(python3 packaging/build-paths.py --check "$build")
unshare -Urn sh -c 'ip link add vcan0 type vcan && ip link set vcan0 up && "$1/can-test" vcan0 && "$1/can-ui-test" vcan0' sh "$build"
