#!/bin/bash
# Source-level compatibility patches for building 3FS commit 22fca04 on the
# UUSE lab toolchain (Ubuntu 26.04, clang-17 + gcc-12, Boost 1.90).
#
# Usage: run inside the 3FS checkout root after upstream patches/apply.sh.
set -eu

ROOT="${1:-/build/3fs}"
cd "$ROOT"

python3 - "$ROOT" <<'PY'
import sys

root = sys.argv[1]

def read(path):
    return open(path, encoding='utf-8', errors='replace').read()

def write(path, text):
    open(path, 'w', encoding='utf-8', errors='replace').write(text)

# 1. clang-17 needs no -fcoroutines-ts and uses the default linker.
p = root + '/CMakeLists.txt'
lines = read(p).splitlines(True)
out = [l for l in lines if '-fcoroutines-ts' not in l and '-fuse-ld=lld' not in l]
if len(out) != len(lines):
    write(p, ''.join(out))
    print('CMakeLists clang flags patched')
else:
    print('CMakeLists clang flags already clean')

# 2. Boost 1.90 dropped the compiled Boost.System stub.
t = read(p)
if 'COMPONENTS filesystem system program_options' in t:
    write(p, t.replace('COMPONENTS filesystem system program_options',
                       'COMPONENTS filesystem program_options'))
    print('CMakeLists boost components patched')

p = root + '/third_party/folly/CMake/folly-deps.cmake'
lines = read(p).splitlines(True)
out = [l for l in lines if l.strip() != 'system']
if len(out) != len(lines):
    write(p, ''.join(out))
    print('folly-deps system line removed')

# 3. Boost 1.90 uuid is not trivial but is trivially copyable.
p = root + '/src/common/utils/SerDeser.h'
t = read(p)
n = t.count('std::is_trivial_v<T>')
if n:
    write(p, t.replace('std::is_trivial_v<T>', 'std::is_trivially_copyable_v<T>'))
print('SerDeser patched occurrences:', n)

# 4. Boost 1.90 uuid API in UsrbIo.
p = root + '/src/lib/api/UsrbIo.cc'
t = read(p)
if 'memcpy(uuid.data, id, sizeof(uuid.data));' in t:
    write(p, t.replace('memcpy(uuid.data, id, sizeof(uuid.data));',
                       'memcpy(uuid.begin(), id, uuid.static_size());'))
    print('UsrbIo uuid patched')
else:
    print('UsrbIo already patched')

# 5. clang-format-14 is absent on newer Ubuntu; accept any clang-format.
p = root + '/cmake/CLangFormat.cmake'
t = read(p)
old = 'set(CLANG_FORMAT "/usr/bin/clang-format-14")'
new = '''# Local UUSE-lab patch: accept any available clang-format when 14 is absent.
find_program(CLANG_FORMAT NAMES clang-format-14 clang-format-17 clang-format-18 clang-format)
if(NOT CLANG_FORMAT)
    set(CLANG_FORMAT "/usr/bin/clang-format-14")
endif()'''
if new in t:
    print('CLangFormat.cmake already patched')
elif old in t:
    write(p, t.replace(old, new, 1))
    print('CLangFormat.cmake patched')
else:
    print('WARN: CLangFormat pattern missing')

# 6. Boost 1.90 uuid API in Uuid.h.
p = root + '/src/common/utils/Uuid.h'
t = read(p)
repl = [
    ('static constexpr Uuid zero() { return {{0}}; }',
     'static Uuid zero() { return Uuid{}; }'),
    ('memset(uuid.data, 0xff, sizeof(uuid.data));',
     'memset(uuid.begin(), 0xff, uuid.static_size());'),
    ('memcpy(uuid.data, &low, sizeof(low));',
     'memcpy(uuid.begin(), &low, sizeof(low));'),
    ('memcpy(uuid.data + sizeof(low), &high, sizeof(high));',
     'memcpy(uuid.begin() + sizeof(low), &high, sizeof(high));'),
    ('std::string_view(reinterpret_cast<const char *>(data), sizeof(data));',
     'std::string_view(reinterpret_cast<const char *>(begin()), static_size());'),
]
n = 0
for old, new in repl:
    if new in t:
        continue
    if old in t:
        t = t.replace(old, new, 1)
        n += 1
if n:
    write(p, t)
print('Uuid.h replacements:', n)

# 7. Remaining Boost 1.90 uuid.data uses.
def patch_file(path, old, new):
    t = read(path)
    if new in t:
        return
    if old in t:
        write(path, t.replace(old, new, 1))
        print('patched:', path)

patch_file(root + '/src/storage/sync/ResyncWorker.cc',
           '*reinterpret_cast<VersionedChainId *>(clientId.uuid.data) = vChainId;',
           '*reinterpret_cast<VersionedChainId *>(clientId.uuid.begin()) = vChainId;')
patch_file(root + '/src/storage/sync/ResyncWorker.cc',
           '*reinterpret_cast<TargetId *>(clientId.uuid.data + sizeof(VersionedChainId)) = targetId;',
           '*reinterpret_cast<TargetId *>(clientId.uuid.begin() + sizeof(VersionedChainId)) = targetId;')
patch_file(root + '/src/lib/py/usrbio_binding.cc',
           'uuid.data, hf3fs_mount_point',
           'uuid.begin(), hf3fs_mount_point')
PY

# 6. liburing compat.h: kernel headers already define struct open_how.
F="$ROOT/third_party/liburing-cmake/liburing/compat.h"
if [ -f "$F" ]; then
  cp -a "$F" "$F.bak-3fs"
  cat > "$F" <<'EOF'
/* SPDX-License-Identifier: MIT */
#ifndef LIBURING_COMPAT_H
#define LIBURING_COMPAT_H

#include <linux/time_types.h>
#define UAPI_LINUX_IO_URING_H_SKIP_LINUX_TIME_TYPES_H 1

#include <inttypes.h>

/* Local UUSE-lab patch: kernel headers already define struct open_how. */
#if defined(__has_include)
# if __has_include(<linux/openat2.h>)
#  include <linux/openat2.h>
# endif
#endif
#ifndef _LINUX_OPENAT2_H
struct open_how {
	uint64_t	flags;
	uint64_t	mode;
	uint64_t	resolve;
};
#endif

#endif
EOF
  echo "liburing compat.h patched"
fi

echo "LAB_COMPAT_DONE"
