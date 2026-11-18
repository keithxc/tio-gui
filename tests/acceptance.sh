#!/usr/bin/env sh
# Run inside `nix develop --command sh tests/acceptance.sh` with a GTK display.
# Uses isolated PTYs, localhost servers and an isolated vcan namespace.
set -eu
export LC_ALL=C GTK_IM_MODULE=simple GTK_A11Y=none
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
cd "$repo"
exports=$(python3 packaging/build-paths.py --environment)
eval "$exports"
build=${1:-$TIO_WORK_DIR/cmake}
build=$(python3 packaging/build-paths.py --check "$build")
export TMPDIR="$TIO_TESTS_DIR/tmp" PYTHONPYCACHEPREFIX="$TIO_CACHE_DIR/pycache"
mkdir -p "$TMPDIR" "$TIO_LOGS_DIR"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build" -j 4
ctest --test-dir "$build" --output-on-failure
for test in quick-editor-test analyzer-test plugin-test plugin-ui-test scroll-test ble-test; do
    "$build/$test"
done
python3 tests/keyboard_acceptance.py "$build/keyboard-test"
python3 tests/socket_acceptance.py "$build/quick-editor-test"
python3 tests/serial_line_acceptance.py "$build"
python3 tests/reconnect_acceptance.py "$build"
python3 tests/transfer_acceptance.py "$build/transfer-test"
for test in network-test network-ui-test; do
    python3 tests/network_acceptance.py "$build/$test"
done
for test in modbus-test modbus-ui-test; do
    python3 tests/modbus_acceptance.py "$build/$test"
done
python3 tests/modbus_failure_acceptance.py "$build/modbus-test"
for test in mqtt-test mqtt-ui-test; do
    python3 tests/mqtt_acceptance.py "$build/$test"
done
sh tests/can_acceptance.sh "$build"
for language in zh_CN zh_TW ja de; do
    msgfmt --check --statistics -o /dev/null "po/$language.po"
done
"$build/tio-gui" --version
"$build/tio-gui" --help > /dev/null
printf '%s\n' 'PASS: complete software acceptance suite'
