#!/usr/bin/env sh
set -eu
export GTK_A11Y=none
export GTK_IM_MODULE=simple
build=${1:-.cache/build}
unshare -Urn sh -c 'ip link add vcan0 type vcan && ip link set vcan0 up && "$1/can-test" vcan0 && "$1/can-ui-test" vcan0' sh "$build"
