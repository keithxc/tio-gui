"""Initialize isolated build/test output before fixture or child creation."""
import importlib.util
from pathlib import Path
import sys
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('tio_build_paths', Path(__file__).resolve().parents[1] / 'packaging/build-paths.py')
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)


def setup():
    return api.environment()
