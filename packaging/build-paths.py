"""Build/test paths only: validate before creating output or starting tools."""
import json
import importlib.util
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import tempfile
import zlib

REPO = Path(__file__).resolve().parents[1]
FALLBACK_CACHE = Path(os.environ.get('MYGIT_ORIGINAL_CACHE_HOME', os.environ.get('XDG_CACHE_HOME', str(Path.home() / '.cache'))))
PROJECT = 'tio-gui'
PURPOSES = ('work', 'cache', 'tests', 'logs', 'dist', 'evidence', 'releases')


def is_mm4():
    return platform.node().split('.')[0].lower() == 'mm4'


def account_home():
    # HOME can legitimately be replaced by a test fixture. Host policy belongs
    # to the actual account, not that fixture or an embedded developer pathname.
    try:
        import pwd
        return Path(pwd.getpwuid(os.getuid()).pw_dir)
    except ImportError:
        return Path.home()


def resolver():
    override = os.environ.get('MYGIT_BUILD_PATHS_RESOLVER')
    canonical = account_home() / 'mygit/tools/build-paths.py'
    if override and is_mm4() and Path(override).resolve() != canonical.resolve():
        raise ValueError('mm4 cannot override the shared build-path resolver')
    candidates = [canonical] if is_mm4() else [REPO.parent / 'tools/build-paths.py', canonical]
    if override and not is_mm4():
        candidates.insert(0, Path(override))
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    if is_mm4():
        raise ValueError('mm4 requires the shared mygit tools/build-paths.py; no internal fallback')
    return None


def shared_api(shared):
    spec = importlib.util.spec_from_file_location('_shared_build_paths', shared)
    module = importlib.util.module_from_spec(spec)
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    return module


def command(shared, task, target=None):
    result = [sys.executable, str(shared), '--project', PROJECT, '--checkout', str(REPO),
              '--task', task]
    if target:
        result += ['--platform', target]
    return result


def structural(path):
    path = Path(path).expanduser().absolute()
    if '..' in path.parts:
        raise ValueError('Output cannot contain parent traversal: ' + str(path))
    for parent in (path, *path.parents):
        if parent.is_symlink() or (parent / '.git').exists() or (parent / '.git').is_symlink():
            raise ValueError('Output crosses a symlink or source repository: ' + str(path))
        if (parent / 'HEAD').is_file() and (parent / 'objects').is_dir():
            raise ValueError('Output is inside a bare Git repository: ' + str(path))
    path = path.resolve()
    if path == REPO or path.is_relative_to(REPO) or path == Path('/') or path == Path.home():
        raise ValueError('Build output must be outside the source checkout: ' + str(path))
    return path


def paths(target=None, task=None):
    task = task or os.environ.get({'tio-gui': 'TIO_BUILD_TASK', 'hh': 'HH_TASK'}.get(PROJECT, PROJECT.upper().replace('-', '_') + '_BUILD_TASK'), 'dev')
    shared = resolver()
    if shared:
        values = json.loads(subprocess.check_output(command(shared, task, target) + ['--json'], text=True))
        return {k: structural(v) for k, v in values.items()}
    # Standalone/CI checkouts keep a portable local cache; no mm4 fallback.
    target = target or (platform.system().lower() + '-' + platform.machine())
    root = FALLBACK_CACHE / PROJECT
    root = root / str(zlib.crc32(os.fsencode(REPO))) / task / target
    return {**{k: structural(root / k) for k in PURPOSES}, 'root': structural(root)}


def validate(path, *, app=False):
    path = structural(path)
    shared = resolver()
    if shared:
        # Resolve the configured SHARED root first. An output descendant is not
        # another shared root: older resolvers prohibit mygit/build/subdir as a
        # --build-root even though it is a perfectly valid output directory.
        values = paths()
        root = values['root'].parent
        api = shared_api(shared)
        workspace = api.workspace_root() if hasattr(api, 'workspace_root') else shared.parent.parent
        if path.is_relative_to(workspace) and not path.is_relative_to(root):
            raise ValueError('mygit output must stay below the configured shared build root')
        if hasattr(api, 'verify_external_root') and hasattr(api, 'host_policy'):
            api.verify_external_root(path, api.host_policy(workspace))
        elif is_mm4():
            raise ValueError('Shared resolver lacks exact-output volume verification; update it before building')
    if app and not any(part.endswith('.noindex') for part in path.parts):
        raise ValueError('App build/staging output needs a .noindex ancestor: ' + str(path))
    return path


def socket_tmp(values):
    import hashlib
    shared_root = values['root'].parent
    identity = str(REPO) + ':' + str(values['cache']) + ':' + str(os.getuid())
    key = hashlib.sha256(identity.encode()).hexdigest()[:10]
    tmp = validate(shared_root / 'tio' / key)
    # Leave room for tempfile prefixes and a 24-byte socket basename on Darwin.
    if len(os.fsencode(tmp)) + 1 + 30 + 1 + 24 >= 104:
        raise ValueError('Verified external root is too long for Darwin Unix sockets; choose a shorter verified build root')
    return tmp


def environment(values=None):
    values = values or paths()
    if not is_mm4():
        return values
    cache = values['cache']
    tmp = socket_tmp(values)
    defaults = {
        'TMPDIR': tmp, 'TMP': tmp, 'TEMP': tmp,
        'PYTHONPYCACHEPREFIX': cache / 'pycache', 'XDG_CACHE_HOME': cache / 'xdg',
        'CLANG_MODULE_CACHE_PATH': cache / 'clang', 'SWIFT_MODULECACHE_PATH': cache / 'swift',
        'CCACHE_DIR': cache / 'ccache', 'CCACHE_TEMPDIR': cache / 'ccache-tmp', 'SCCACHE_DIR': cache / 'sccache',
        'npm_config_cache': cache / 'npm', 'NPM_CONFIG_CACHE': cache / 'npm',
    }
    # Ambient global caches/tmp are deliberately replaced for this invocation.
    for path in defaults.values():
        structural(path)
    for path in set(defaults.values()):
        path.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault('MYGIT_ORIGINAL_CACHE_HOME', str(FALLBACK_CACHE))
    os.environ.update({key: str(value) for key, value in defaults.items()})
    os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
    sys.dont_write_bytecode = True
    tempfile.tempdir = str(defaults['TMPDIR'])
    return values


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform')
    parser.add_argument('--task')
    parser.add_argument('--purpose', default='work')
    parser.add_argument('--check', type=Path)
    parser.add_argument('--shell', action='store_true')
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--create', action='store_true')
    parser.add_argument('--environment', action='store_true')
    args = parser.parse_args()
    if args.check:
        print(validate(args.check)); return
    values = paths(args.platform, args.task)
    if args.json:
        print(json.dumps({k: str(v) for k, v in values.items()})); return
    if args.environment:
        before = dict(os.environ)
        environment(values)
        for key, value in os.environ.items():
            if before.get(key) != value:
                print('export ' + key + '=' + shlex.quote(value))
    if args.shell or args.environment:
        prefix = 'TIO' if PROJECT == 'tio-gui' else PROJECT.upper().replace('-', '_')
        for name in PURPOSES:
            print('export ' + prefix + '_' + name.upper() + '_DIR=' + shlex.quote(str(values[name])))
        return
    path = values[args.purpose]
    if args.create: path.mkdir(parents=True, exist_ok=True)
    print(path)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
