#!/usr/bin/env python3
"""Guarded native CMake/CTest entrypoint; Linux/Windows x86_64 belongs on pk4."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('build_paths', Path(__file__).with_name('build-paths.py'))
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path)
parser.add_argument('--configuration', default='RelWithDebInfo', choices=['Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel'])
parser.add_argument('--test', action='store_true')
args = parser.parse_args()
values = api.paths()
build = api.validate(args.build or values['work'] / 'cmake.noindex', app=sys.platform == 'darwin')
api.environment(values)
subprocess.run(['cmake', '-S', str(api.REPO), '-B', str(build), '-DCMAKE_BUILD_TYPE=' + args.configuration], check=True)
subprocess.run(['cmake', '--build', str(build)], check=True)
if args.test:
    values['tests'].mkdir(parents=True, exist_ok=True)
    subprocess.run(['ctest', '--test-dir', str(build), '--output-on-failure', '--output-log', str(values['tests'] / 'ctest.log')], check=True)
