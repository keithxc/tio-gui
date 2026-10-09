"""Runtime relocation rules shared by the macOS packager and its regression test."""
import subprocess
from pathlib import Path


def system_library_for(source: Path):
    """Use macOS's Apple iconv, never its build-machine-only Nix copy.

    Nix's Apple libiconv has the system ABI, but loads converter modules and
    charset tables from absolute store paths. GNU libiconv has a different ABI
    and must still be bundled, even though its dylib has the same basename.
    """
    if source.name != "libiconv.2.dylib":
        return None
    data = source.read_bytes()
    if b"/nix/store/" not in data or b"/share/i18n/csmapper" not in data:
        return None
    symbols = {line.split()[-1] for line in subprocess.check_output(
        ["/usr/bin/nm", "-gU", str(source)], text=True).splitlines() if line.split()}
    if "_iconv_open" not in symbols or "_libiconv_open" in symbols:
        raise RuntimeError(f"Unexpected ABI for build-machine Apple iconv: {source}")
    return "/usr/lib/libiconv.2.dylib"
