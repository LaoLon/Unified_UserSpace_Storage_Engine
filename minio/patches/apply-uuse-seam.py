#!/usr/bin/env python3
# Shard-9.2 patch (A-plan): route the MinIO disk layer through UUSE.
# Usage: python3 apply-uuse-seam.py <minio-src-dir>
#
# Anchored, idempotent replacements against the frozen tree
# RELEASE.2025-10-15T17-29-55Z. Re-running skips applied anchors; a missing
# anchor fails loudly (upstream drift).

import sys
from pathlib import Path

NL = "\n"

# --- cmd/xl-storage.go ------------------------------------------------------
XL_PATCHES = [
    (
        '\t"github.com/minio/minio/internal/disk"' + NL,
        '\t"github.com/minio/minio/internal/disk"' + NL +
        '\tuuseos "github.com/minio/minio/internal/uuseos"' + NL,
        "uuseos import",
    ),
    (
        "func (s *xlStorage) openFileDirect(path string, mode int) (f *os.File, err error) {" + NL +
        "\tw, err := OpenFileDirectIO(path, mode, 0o666)" + NL,
        "func (s *xlStorage) openFileDirect(path string, mode int) (f uuseos.File, err error) {" + NL +
        "\tif uuseos.RegisteredParent(path) {" + NL +
        "\t\tvar handle uuseos.File" + NL +
        "\t\t_, handle, err = uuseos.OpenFile(path, mode, 0o666)" + NL +
        "\t\treturn handle, err" + NL +
        "\t}" + NL +
        "\tw, err := OpenFileDirectIO(path, mode, 0o666)" + NL,
        "openFileDirect routing",
    ),
    (
        "func (s *xlStorage) openFileSync(filePath string, mode int, skipParent string) (f *os.File, err error) {" + NL +
        "\treturn s.openFile(filePath, mode|writeMode, skipParent)" + NL +
        "}",
        "func (s *xlStorage) openFileSync(filePath string, mode int, skipParent string) (f uuseos.File, err error) {" + NL +
        "\treturn s.openFile(filePath, mode|writeMode, skipParent)" + NL +
        "}",
        "openFileSync signature",
    ),
    (
        "func (s *xlStorage) openFile(filePath string, mode int, skipParent string) (f *os.File, err error) {",
        "func (s *xlStorage) openFile(filePath string, mode int, skipParent string) (f uuseos.File, err error) {",
        "openFile signature",
    ),
    (
        "\tw, err := OpenFile(filePath, mode, 0o666)" + NL +
        "\tif err != nil {" + NL,
        "\tvar w uuseos.File" + NL +
        "\tif uuseos.RegisteredParent(filePath) {" + NL +
        "\t\t_, w, err = uuseos.OpenFile(filePath, mode, 0o666)" + NL +
        "\t} else {" + NL +
        "\t\tw, err = OpenFile(filePath, mode, 0o666)" + NL +
        "\t}" + NL +
        "\tif err != nil {" + NL,
        "openFile body routing",
    ),
    (
        "\t// Open the file for reading." + NL +
        "\tfile, err := OpenFile(filePath, readMode, 0o666)" + NL,
        "\t// Open the file for reading." + NL +
        "\tvar file uuseos.File" + NL +
        "\tif uuseos.RegisteredParent(filePath) {" + NL +
        "\t\tfile, err = uuseos.ReadFileUuse(filePath)" + NL +
        "\t} else {" + NL +
        "\t\tvar native *os.File" + NL +
        "\t\tnative, err = OpenFile(filePath, readMode, 0o666)" + NL +
        "\t\tfile = native" + NL +
        "\t}" + NL,
        "ReadFile open routing",
    ),
    (
        "\tfile, err := OpenFile(filePath, readMode, 0o666)" + NL +
        "\tif err != nil {" + NL +
        "\t\tswitch {" + NL +
        "\t\tcase osIsNotExist(err):" + NL +
        "\t\t\tif !skipAccessChecks(volume) {" + NL,
        "\tvar file uuseos.File" + NL +
        "\tif uuseos.RegisteredParent(filePath) {" + NL +
        "\t\tfile, err = uuseos.ReadFileUuse(filePath)" + NL +
        "\t} else {" + NL +
        "\t\tvar native *os.File" + NL +
        "\t\tnative, err = OpenFile(filePath, readMode, 0o666)" + NL +
        "\t\tfile = native" + NL +
        "\t}" + NL +
        "\tif err != nil {" + NL +
        "\t\tswitch {" + NL +
        "\t\tcase osIsNotExist(err):" + NL +
        "\t\t\tif !skipAccessChecks(volume) {" + NL,
        "ReadFileStream open routing",
    ),
    (
        "\tif err = Fdatasync(w); err != nil {" + NL +
        "\t\tw.Close()" + NL +
        "\t\treturn err" + NL +
        "\t}" + NL,
        "\tif ufile, ok := w.(uuseos.File); ok && uuseos.RegisteredParent(filePath) {" + NL +
        "\t\tif err = ufile.Sync(); err != nil {" + NL +
        "\t\t\tw.Close()" + NL +
        "\t\t\treturn err" + NL +
        "\t\t}" + NL +
        "\t} else if err = Fdatasync(w.(*os.File)); err != nil {" + NL +
        "\t\tw.Close()" + NL +
        "\t\treturn err" + NL +
        "\t}" + NL,
        "writeAllDirect sync routing",
    ),
    (
        "\t\twritten, err = xioutil.CopyAligned(diskHealthWriter(ctx, w), r, *bufp, fileSize, w)" + NL,
        "\t\twritten, err = xioutil.CopyAligned(diskHealthWriter(ctx, w), r, *bufp, fileSize, w.(*os.File))" + NL,
        "writeAllDirect CopyAligned native handle",
    ),
    (
        "\tvar written int64" + NL +
        "\tif odirectEnabled {" + NL +
        "\t\twritten, err = xioutil.CopyAligned(diskHealthWriter(ctx, w), r, *bufp, fileSize, w.(*os.File))" + NL,
        "\tvar written int64" + NL +
        "\tif odirectEnabled && !uuseos.RegisteredParent(filePath) {" + NL +
        "\t\twritten, err = xioutil.CopyAligned(diskHealthWriter(ctx, w), r, *bufp, fileSize, w.(*os.File))" + NL,
        "writeAllDirect CopyAligned UUSE guard",
    ),
    (
        "\tvar w *os.File" + NL,
        "\tvar w uuseos.File" + NL,
        "write handle type (Direct/Internal/Append)",
        True,
    ),
    (
        "\tb, err := os.ReadFile(healingFile)" + NL,
        "\tb, err := uuseos.ReadFile(healingFile)" + NL,
        "healing file read",
    ),
    (
        "\tb, err := os.ReadFile(s.formatFile)" + NL,
        "\tb, err := uuseos.ReadFile(s.formatFile)" + NL,
        "format.json read",
    ),
    (
        "// getDiskInfo returns given disk information." + NL +
        "func getDiskInfo(drivePath string) (di disk.Info, rootDrive bool, err error) {" + NL,
        "// getDiskInfo returns given disk information." + NL +
        "func getDiskInfo(drivePath string) (di disk.Info, rootDrive bool, err error) {" + NL +
        "\tif uuseos.RegisteredParent(drivePath) {" + NL +
        "\t\ttotal, free, _, derr := uuseos.DriveStats(drivePath)" + NL +
        "\t\tif derr != nil {" + NL +
        "\t\t\treturn di, false, derr" + NL +
        "\t\t}" + NL +
        "\t\tdi.Total = total" + NL +
        "\t\tdi.Free = free" + NL +
        "\t\tdi.Used = total - free" + NL +
        "\t\tdi.FSType = \"uuse\"" + NL +
        "\t\tdi.Name = drivePath" + NL +
        "\t\trot := false" + NL +
        "\t\tdi.Rotational = &rot" + NL +
        "\t\treturn di, false, nil" + NL +
        "\t}" + NL,
        "getDiskInfo routing",
    ),
    (
        "\ts.drivePath, err = getValidPath(ep.Path)" + NL +
        "\tif err != nil {" + NL +
        "\t\ts.drivePath = ep.Path" + NL +
        "\t\treturn s, err" + NL +
        "\t}" + NL,
        "\ts.drivePath, err = getValidPath(ep.Path)" + NL +
        "\tif err != nil {" + NL +
        "\t\ts.drivePath = ep.Path" + NL +
        "\t\treturn s, err" + NL +
        "\t}" + NL +
        "\t// Mount the UUSE engine for declared drives before any disk access." + NL +
        "\tif uuseos.LookupParent(s.drivePath) {" + NL +
        "\t\tif err = uuseos.MountDrives(s.drivePath); err != nil {" + NL +
        "\t\t\treturn s, err" + NL +
        "\t\t}" + NL +
        "\t}" + NL,
        "engine mount hook (early)",
    ),
    (
        "\tdstBuf, err := xioutil.ReadFile(dstFilePath)" + NL,
        "\tvar dstBuf []byte" + NL +
        "\tif uuseos.RegisteredParent(dstFilePath) {" + NL +
        "\t\tdstBuf, err = uuseos.ReadFile(dstFilePath)" + NL +
        "\t} else {" + NL +
        "\t\tdstBuf, err = xioutil.ReadFile(dstFilePath)" + NL +
        "\t}" + NL,
        "RenameData dst xl.meta read routing",
    ),
    (
        "\t\t\t\tdstBuf, err = xioutil.ReadFile(dstFilePath)" + NL,
        "\t\t\t\tif uuseos.RegisteredParent(dstFilePath) {" + NL +
        "\t\t\t\t\tdstBuf, err = uuseos.ReadFile(dstFilePath)" + NL +
        "\t\t\t\t} else {" + NL +
        "\t\t\t\t\tdstBuf, err = xioutil.ReadFile(dstFilePath)" + NL +
        "\t\t\t\t}" + NL,
        "RenameData legacy dst read routing",
    ),
    (
        "\tf, err := OpenFile(itemPath, readMode, 0o666)" + NL,
        "\tvar f uuseos.File" + NL +
        "\tvar err error" + NL +
        "\tif uuseos.RegisteredParent(itemPath) {" + NL +
        "\t\t_, f, err = uuseos.OpenFile(itemPath, readMode, 0o666)" + NL +
        "\t} else {" + NL +
        "\t\tf, err = OpenFile(itemPath, readMode, 0o666)" + NL +
        "\t}" + NL,
        "readMetadataWithDMTime open routing",
    ),
    (
        "\tf, err := OpenFile(filePath, readMode, 0o666)" + NL +
        "\tif err != nil {" + NL +
        "\t\tswitch {" + NL +
        "\t\tcase osIsNotExist(err):" + NL +
        "\t\t\t// Check if the object doesn't exist because its bucket" + NL,
        "\tvar f uuseos.File" + NL +
        "\tif uuseos.RegisteredParent(filePath) {" + NL +
        "\t\t_, f, err = uuseos.OpenFile(filePath, readMode, 0o666)" + NL +
        "\t} else {" + NL +
        "\t\tf, err = OpenFile(filePath, readMode, 0o666)" + NL +
        "\t}" + NL +
        "\tif err != nil {" + NL +
        "\t\tswitch {" + NL +
        "\t\tcase osIsNotExist(err):" + NL +
        "\t\t\t// Check if the object doesn't exist because its bucket" + NL,
        "readAllDataWithDMTime open routing",
    ),
    (
        "\t// Open the file for reading." + NL +
        "\tfile, err := OpenFile(partPath, readMode, 0o666)" + NL,
        "\t// Open the file for reading." + NL +
        "\tvar file uuseos.File" + NL +
        "\tvar err error" + NL +
        "\tif uuseos.RegisteredParent(partPath) {" + NL +
        "\t\t_, file, err = uuseos.OpenFile(partPath, readMode, 0o666)" + NL +
        "\t} else {" + NL +
        "\t\tfile, err = OpenFile(partPath, readMode, 0o666)" + NL +
        "\t}" + NL,
        "bitrotVerify open routing",
    ),
    (
        "\todirectEnabled := globalAPIConfig.odirectEnabled() && s.oDirect && fileSize > 0" + NL +
        NL +
        "\tvar w uuseos.File" + NL +
        "\tif odirectEnabled {" + NL +
        "\t\tw, err = OpenFileDirectIO(filePath, flags, 0o666)" + NL +
        "\t} else {" + NL +
        "\t\tw, err = OpenFile(filePath, flags, 0o666)" + NL +
        "\t}" + NL,
        "\todirectEnabled := globalAPIConfig.odirectEnabled() && s.oDirect && fileSize > 0" + NL +
        NL +
        "\tvar w uuseos.File" + NL +
        "\tif uuseos.RegisteredParent(filePath) {" + NL +
        "\t\t_, w, err = uuseos.OpenFile(filePath, flags, 0o666)" + NL +
        "\t} else if odirectEnabled {" + NL +
        "\t\tw, err = OpenFileDirectIO(filePath, flags, 0o666)" + NL +
        "\t} else {" + NL +
        "\t\tw, err = OpenFile(filePath, flags, 0o666)" + NL +
        "\t}" + NL,
        "writeAllDirect open routing",
    ),
    (
        "\tdirPathAbs := pathJoin(volumeDir, dirPath)" + NL +
        "\tif count > 0 {" + NL +
        "\t\tentries, err = readDirN(dirPathAbs, count)" + NL +
        "\t} else {" + NL +
        "\t\tentries, err = readDir(dirPathAbs)" + NL +
        "\t}" + NL,
        "\tdirPathAbs := pathJoin(volumeDir, dirPath)" + NL +
        "\tif uuseos.RegisteredParent(dirPathAbs) {" + NL +
        "\t\tlimit := count" + NL +
        "\t\tif limit <= 0 {" + NL +
        "\t\t\tlimit = -1" + NL +
        "\t\t}" + NL +
        "\t\tentries, err = uuseos.ReadDirN(dirPathAbs, limit)" + NL +
        "\t} else if count > 0 {" + NL +
        "\t\tentries, err = readDirN(dirPathAbs, count)" + NL +
        "\t} else {" + NL +
        "\t\tentries, err = readDir(dirPathAbs)" + NL +
        "\t}" + NL,
        "ListDir uuse enumeration",
    ),
]

# --- cmd/os-instrumented.go -------------------------------------------------
OSI_PATCHES = [
    (
        '// RemoveAll captures time taken to call the underlying os.RemoveAll' + NL +
        'func RemoveAll(dirPath string) (err error) {' + NL +
        '\tdefer updateOSMetrics(osMetricRemoveAll, dirPath)(err)' + NL +
        '\treturn os.RemoveAll(dirPath)' + NL +
        '}',
        '// RemoveAll captures time taken to call the underlying os.RemoveAll' + NL +
        'func RemoveAll(dirPath string) (err error) {' + NL +
        '\tdefer updateOSMetrics(osMetricRemoveAll, dirPath)(err)' + NL +
        '\tif uuseos.RegisteredParent(dirPath) {' + NL +
        '\t\treturn uuseos.RemoveAll(dirPath)' + NL +
        '\t}' + NL +
        '\treturn os.RemoveAll(dirPath)' + NL +
        '}',
        "RemoveAll routing",
    ),
    (
        'func MkdirAll(dirPath string, mode os.FileMode, baseDir string) (err error) {' + NL +
        '\tdefer updateOSMetrics(osMetricMkdirAll, dirPath)(err)' + NL +
        '\treturn osMkdirAll(dirPath, mode, baseDir)' + NL +
        '}',
        'func MkdirAll(dirPath string, mode os.FileMode, baseDir string) (err error) {' + NL +
        '\tdefer updateOSMetrics(osMetricMkdirAll, dirPath)(err)' + NL +
        '\tif uuseos.RegisteredParent(dirPath) {' + NL +
        '\t\treturn uuseos.MkdirAll(dirPath, mode)' + NL +
        '\t}' + NL +
        '\treturn osMkdirAll(dirPath, mode, baseDir)' + NL +
        '}',
        "MkdirAll routing (before baseDir shortcut)",
    ),
    (
        'func Rename(src, dst string) (err error) {' + NL +
        '\tdefer updateOSMetrics(osMetricRename, src, dst)(err)' + NL +
        '\treturn RenameSys(src, dst)' + NL +
        '}',
        'func Rename(src, dst string) (err error) {' + NL +
        '\tdefer updateOSMetrics(osMetricRename, src, dst)(err)' + NL +
        '\tif uuseos.RegisteredParent(src) || uuseos.RegisteredParent(dst) {' + NL +
        '\t\treturn uuseos.Rename(src, dst)' + NL +
        '\t}' + NL +
        '\treturn RenameSys(src, dst)' + NL +
        '}',
        "Rename routing",
    ),
    (
        'func Lstat(name string) (info os.FileInfo, err error) {' + NL +
        '\tdefer updateOSMetrics(osMetricLstat, name)(err)' + NL +
        '\treturn os.Lstat(name)' + NL +
        '}',
        'func Lstat(name string) (info os.FileInfo, err error) {' + NL +
        '\tdefer updateOSMetrics(osMetricLstat, name)(err)' + NL +
        '\tif uuseos.RegisteredParent(name) {' + NL +
        '\t\treturn uuseos.Lstat(name)' + NL +
        '\t}' + NL +
        '\treturn os.Lstat(name)' + NL +
        '}',
        "Lstat routing",
    ),
    (
        'func Remove(deletePath string) (err error) {' + NL +
        '\tdefer updateOSMetrics(osMetricRemove, deletePath)(err)' + NL +
        '\treturn os.Remove(deletePath)' + NL +
        '}',
        'func Remove(deletePath string) (err error) {' + NL +
        '\tdefer updateOSMetrics(osMetricRemove, deletePath)(err)' + NL +
        '\tif uuseos.RegisteredParent(deletePath) {' + NL +
        '\t\treturn uuseos.Remove(deletePath)' + NL +
        '\t}' + NL +
        '\treturn os.Remove(deletePath)' + NL +
        '}',
        "Remove routing",
    ),
    (
        'func Stat(name string) (info os.FileInfo, err error) {' + NL +
        '\tdefer updateOSMetrics(osMetricStat, name)(err)' + NL +
        '\treturn os.Stat(name)' + NL +
        '}',
        'func Stat(name string) (info os.FileInfo, err error) {' + NL +
        '\tdefer updateOSMetrics(osMetricStat, name)(err)' + NL +
        '\tif uuseos.RegisteredParent(name) {' + NL +
        '\t\treturn uuseos.Stat(name)' + NL +
        '\t}' + NL +
        '\treturn os.Stat(name)' + NL +
        '}',
        "Stat routing",
    ),
    (
        'func Mkdir(dirPath string, mode os.FileMode) (err error) {' + NL +
        '\tdefer updateOSMetrics(osMetricMkdir, dirPath)(err)' + NL +
        '\treturn os.Mkdir(dirPath, mode)' + NL +
        '}',
        'func Mkdir(dirPath string, mode os.FileMode) (err error) {' + NL +
        '\tdefer updateOSMetrics(osMetricMkdir, dirPath)(err)' + NL +
        '\tif uuseos.RegisteredParent(dirPath) {' + NL +
        '\t\treturn uuseos.MkdirAll(dirPath, mode)' + NL +
        '\t}' + NL +
        '\treturn os.Mkdir(dirPath, mode)' + NL +
        '}',
        "Mkdir routing",
    ),
]

# --- cmd/os_unix.go ---------------------------------------------------------
OSU_PATCHES = [
    (
        'func readDirFn(dirPath string, fn func(name string, typ os.FileMode) error) error {' + NL +
        '\tfd, err := openFileWithFD(dirPath, readMode, 0o666)' + NL,
        'func readDirFn(dirPath string, fn func(name string, typ os.FileMode) error) error {' + NL +
        '\tif uuseos.RegisteredParent(dirPath) {' + NL +
        '\t\treturn uuseos.ReadDirFn(dirPath, fn)' + NL +
        '\t}' + NL +
        '\tfd, err := openFileWithFD(dirPath, readMode, 0o666)' + NL,
        "readDirFn routing",
    ),
    (
        'func readDirWithOpts(dirPath string, opts readDirOpts) (entries []string, err error) {' + NL +
        '\tfd, err := openFileWithFD(dirPath, readMode, 0o666)' + NL,
        'func readDirWithOpts(dirPath string, opts readDirOpts) (entries []string, err error) {' + NL +
        '\tif uuseos.RegisteredParent(dirPath) {' + NL +
        '\t\treturn uuseos.ReadDirN(dirPath, opts.count)' + NL +
        '\t}' + NL +
        '\tfd, err := openFileWithFD(dirPath, readMode, 0o666)' + NL,
        "readDirWithOpts routing",
    ),
    (
        'func osMkdirAll(dirPath string, perm os.FileMode, baseDir string) error {' + NL +
        '\tif baseDir != "" {' + NL,
        'func osMkdirAll(dirPath string, perm os.FileMode, baseDir string) error {' + NL +
        '\tif uuseos.RegisteredParent(dirPath) {' + NL +
        '\t\treturn uuseos.MkdirAll(dirPath, perm)' + NL +
        '\t}' + NL +
        '\tif baseDir != "" {' + NL,
        "osMkdirAll routing",
    ),
    (
        'func access(name string) error {' + NL +
        '\tif err := unix.Access(name, unix.F_OK); err != nil {' + NL +
        '\t\treturn &os.PathError{Op: "lstat", Path: name, Err: err}' + NL +
        '\t}' + NL +
        '\treturn nil' + NL +
        '}',
        'func access(name string) error {' + NL +
        '\tif uuseos.RegisteredParent(name) {' + NL +
        '\t\tif _, err := uuseos.Stat(name); err != nil {' + NL +
        '\t\t\treturn &os.PathError{Op: "access", Path: name, Err: err}' + NL +
        '\t\t}' + NL +
        '\t\treturn nil' + NL +
        '\t}' + NL +
        '\tif err := unix.Access(name, unix.F_OK); err != nil {' + NL +
        '\t\treturn &os.PathError{Op: "lstat", Path: name, Err: err}' + NL +
        '\t}' + NL +
        '\treturn nil' + NL +
        '}',
        "access routing",
    ),
]

# --- cmd/metacache-walk.go --------------------------------------------------
MW_PATCHES = [
    (
        "\tmeta.metadata, err = xioutil.ReadFile(pathJoinBuf(sb, volumeDir, current, entry))" + NL,
        "\tmeta.metadata, err = uuseos.ReadFileIfUuse(pathJoinBuf(sb, volumeDir, current, entry))" + NL,
        "metacache walk entry metadata read routing",
    ),
    (
        "\t\t\t\t\tmeta.metadata, err = xioutil.ReadFile(pathJoinBuf(sb, volumeDir, meta.name, xlStorageFormatFileV1))" + NL,
        "\t\t\t\t\tmeta.metadata, err = uuseos.ReadFileIfUuse(pathJoinBuf(sb, volumeDir, meta.name, xlStorageFormatFileV1))" + NL,
        "metacache walk legacy metadata read routing",
    ),
]

PATCHES = [
    ("cmd/xl-storage.go", XL_PATCHES, 'uuseos "github.com/minio/minio/internal/uuseos"'),
    ("cmd/os-instrumented.go", OSI_PATCHES, 'uuseos "github.com/minio/minio/internal/uuseos"'),
    ("cmd/os_unix.go", OSU_PATCHES, 'uuseos "github.com/minio/minio/internal/uuseos"'),
    ("cmd/metacache-walk.go", MW_PATCHES, 'uuseos "github.com/minio/minio/internal/uuseos"'),
]


def add_import(text: str, marker: str, where: str) -> str:
    if marker in text:
        return text
    anchors = [
        '\t"github.com/minio/minio/internal/ioutil"' + NL,
        '\t"github.com/minio/minio/internal/disk"' + NL,
        '\t"github.com/minio/minio/internal/bpool"' + NL,
        '\tioutilx "github.com/minio/minio/internal/ioutil"' + NL,
        '\txioutil "github.com/minio/minio/internal/ioutil"' + NL,
        '\t"github.com/minio/minio/internal/grid"' + NL,
    ]
    for anchor in anchors:
        if anchor in text:
            return text.replace(anchor, anchor + marker + NL, 1)
    raise SystemExit(f"import anchor not found in {where}")


def apply(root: Path) -> None:
    applied = skipped = 0
    for rel, replacements, importMarker in PATCHES:
        path = root / rel
        text = path.read_text()
        text = add_import(text, importMarker, rel)
        for entry in replacements:
            old, new, desc = entry[0], entry[1], entry[2]
            replace_all = len(entry) > 3 and entry[3]
            if new in text and not replace_all:
                skipped += 1
                continue
            count = text.count(old)
            if count == 0:
                raise SystemExit(
                    f"anchor not found in {rel} ({desc}):\n{old[:200]}"
                )
            if count != 1 and not replace_all:
                raise SystemExit(
                    f"anchor not unique in {rel} ({desc}), count={count}:\n{old[:200]}"
                )
            text = text.replace(old, new)
            applied += 1
        path.write_text(text)
        print(f"patched {rel}")
    print(f"done: applied={applied} skipped={skipped}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: apply-uuse-seam.py <minio-src>")
    apply(Path(sys.argv[1]))
