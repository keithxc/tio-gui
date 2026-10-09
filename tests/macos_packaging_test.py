#!/usr/bin/env python3
"""Regression: replace Nix Apple iconv, preserve the distinct GNU provider."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "packaging"))
from macos_runtime import system_library_for

scratch = Path(sys.argv[1]).resolve()
scratch.mkdir(parents=True, exist_ok=True)

def run(*args):
    return subprocess.run([str(arg) for arg in args], check=True, capture_output=True, text=True)

with tempfile.TemporaryDirectory(prefix="iconv-regression-", dir=scratch) as temporary:
    root = Path(temporary)
    source = root / "iconv.c"
    library = root / "libiconv.2.dylib"
    source.write_text('''#include <stddef.h>
const char *data = "/nix/store/example-libiconv-113/share/i18n/csmapper";
void *iconv_open(const char *a, const char *b) { (void)a; (void)b; return (void *)-1; }
size_t iconv(void *c, char **i, size_t *il, char **o, size_t *ol) { return (size_t)-1; }
int iconv_close(void *c) { return -1; }
''')
    run("/usr/bin/xcrun", "clang", "-dynamiclib", source, "-Wl,-install_name," + str(library), "-o", library)
    assert system_library_for(library) == "/usr/lib/libiconv.2.dylib"
    probe_source = root / "probe.c"
    probe_source.write_text('''#include <iconv.h>
#include <string.h>
int main(void) {
  iconv_t c = iconv_open("UTF-8", "ISO-8859-1");
  if (c == (iconv_t)-1) return 2;
  char in[] = "\\xe9", out[8] = {0}, *ip = in, *op = out;
  size_t il = 1, ol = sizeof out;
  if (iconv(c, &ip, &il, &op, &ol) == (size_t)-1) return 3;
  iconv_close(c);
  return il != 0 || memcmp(out, "\\xc3\\xa9", 2) != 0;
}
''')
    probe = root / "probe"
    run("/usr/bin/xcrun", "clang", probe_source, library, "-o", probe)
    assert subprocess.run([str(probe)]).returncode == 2
    run("/usr/bin/install_name_tool", "-change", library, system_library_for(library), probe)
    run("/usr/bin/codesign", "--force", "--sign", "-", probe)
    run(probe)
    source.write_text('void *libiconv_open(const char *a, const char *b) { return 0; }\n')
    run("/usr/bin/xcrun", "clang", "-dynamiclib", source, "-o", library)
    assert system_library_for(library) is None
print("PASS Apple iconv relocation converts text; GNU iconv remains bundled")
