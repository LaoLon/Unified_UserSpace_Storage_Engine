#!/bin/bash
# Build-time fixes for the 3FS build on the UUSE lab toolchain:
#  - CMake 4.x rejects old cmake_minimum_required in Arrow's fetched bundled
#    deps; bump every version < 3.5 to 3.5.
#  - Arrow's thrift ExternalProject tarball download can fail through the lab
#    proxy; place the verified thrift-0.16.0 tarball manually.
#
# Run after a build failure, then restart the build.
set -u
ROOT="${1:-/build/3fs}"

echo "== fix A: Arrow bundled cmake_minimum_required < 3.5 =="
find "$ROOT/build/src/apache-arrow-cpp" -name CMakeLists.txt -print0 2>/dev/null | while IFS= read -r -d '' f; do
  python3 - "$f" <<'PY'
import re, sys
p = sys.argv[1]
try:
    t = open(p, encoding='utf-8', errors='replace').read()
except OSError:
    raise SystemExit(0)
def repl(m):
    return 'cmake_minimum_required(VERSION 3.5)' if (int(m.group(1)), int(m.group(2))) < (3, 5) else m.group(0)
new = re.sub(r'cmake_minimum_required\(VERSION\s+([0-9]+)\.([0-9]+)[^)\n]*', repl, t)
if new != t:
    open(p, 'w', encoding='utf-8', errors='replace').write(new)
    print('bumped', p)
PY
done

echo "== fix B: thrift 0.16.0 tarball =="
DEST="$ROOT/build/src/apache-arrow-cpp/cpp/thrift_ep-prefix/src/thrift-0.16.0.tar.gz"
SHA=f460b5c1ca30d8918ff95ea3eb6291b3951cf518553566088f3f2be8981f6209
if [ -d "$(dirname "$DEST")" ]; then
  if echo "$SHA  $DEST" | sha256sum -c - >/dev/null 2>&1; then
    echo "thrift tarball ok"
  else
    rm -f "$DEST"
    curl -L --max-time 300 -sS -o "$DEST" https://archive.apache.org/dist/thrift/0.16.0/thrift-0.16.0.tar.gz
    echo "$SHA  $DEST" | sha256sum -c -
  fi
else
  echo "no thrift_ep dir yet"
fi
echo "BUILD_FIXES_DONE"
