#!/usr/bin/env python3
"""Host-shim regression for Haiku staged archive and extraction lifecycle."""
import argparse
import os
from pathlib import Path
import subprocess
import stat
import struct
import tempfile
import zlib

ROOT = Path(__file__).resolve().parent.parent


def run(command, cwd=None, expected=0, env=None):
    result = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    if result.returncode != expected:
        raise RuntimeError("command returned %d, expected %d: %r\n%s" %
                           (result.returncode, expected, command,
                            result.stdout.decode(errors="replace")))
    return result.stdout


def primary_times(path):
    archive = path.read_bytes()
    table_offset = struct.unpack_from("<Q", archive, len(archive) - 20)[0]
    count = struct.unpack_from("<I", archive, table_offset + 4)[0]
    record = table_offset + 16
    entries = {}
    for _ in range(count):
        name_length = struct.unpack_from("<H", archive, record)[0]
        name = archive[record + 2:record + 2 + name_length].decode("utf-8")
        fields = record + 2 + name_length
        entry_type = archive[fields + 16]
        mtime = struct.unpack_from("<Q", archive, fields + 18)[0]
        flags = struct.unpack_from("<H", archive, fields + 41)[0]
        if entry_type in (0, 3, 4):
            entries[name] = (mtime, flags)
        record = fields + 43
    return entries


def haiku_meta_payload(path, subject):
    archive = path.read_bytes()
    offset = 32
    while offset + 4 <= len(archive):
        header_size, name_length = struct.unpack_from("<HH", archive, offset)
        if header_size < 42 + name_length or offset + header_size > len(archive):
            break
        name_start = offset + 4
        name = archive[name_start:name_start + name_length].decode("utf-8")
        fields = name_start + name_length
        original_size = struct.unpack_from("<Q", archive, fields)[0]
        compressed_size = struct.unpack_from("<Q", archive, fields + 8)[0]
        entry_type = archive[fields + 26]
        platform = archive[fields + 27]
        payload_start = offset + header_size
        if name == subject and entry_type == 2 and platform == 0x14:
            if compressed_size != original_size + 12:
                raise RuntimeError("stored Haiku metadata payload has unexpected size")
            block_size, block_original = struct.unpack_from("<II", archive,
                                                              payload_start)
            if block_size != original_size or block_original != original_size:
                raise RuntimeError("Haiku metadata block lengths are inconsistent")
            return archive[payload_start + 12:payload_start + 12 + original_size]
        offset = payload_start + compressed_size
    raise RuntimeError("archive has no Haiku metadata record for " + subject)


def set_first_entry_mode(source, target, mode):
    archive = bytearray(source.read_bytes())
    table_offset, table_size = struct.unpack_from("<QQ", archive, len(archive) - 20)
    record = table_offset + 16
    name_length = struct.unpack_from("<H", archive, record)[0]
    fields = record + 2 + name_length
    entry_offset = struct.unpack_from("<Q", archive, fields + 26)[0]
    flags = (struct.unpack_from("<H", archive, fields + 41)[0] & 0x4000) | 0x8000 | mode
    struct.pack_into("<H", archive, fields + 41, flags)
    struct.pack_into("<H", archive, entry_offset + 4 + name_length + 36, flags)
    struct.pack_into("<I", archive, 24, zlib.crc32(archive[table_offset:table_offset + table_size]))
    struct.pack_into("<I", archive, 28, zlib.crc32(archive[:28]))
    target.write_bytes(archive)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--linux-boltpack", type=Path,
                        default=ROOT / "build/linux/boltpack")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    if args.output:
        work = args.output.resolve()
        work.mkdir(parents=True, exist_ok=False)
    else:
        work = Path(tempfile.mkdtemp(prefix="boltpack-haiku-host-"))
    stub = work / "api-stub"
    stub.mkdir()
    (stub / "fs_attr.h").write_text(
        "#ifndef FS_ATTR_H\n#define FS_ATTR_H\n"
        "#include <stddef.h>\n#include <sys/types.h>\n"
        "#ifndef B_MIME_STRING_TYPE\n#define B_MIME_STRING_TYPE 'MIMS'\n#endif\n"
        "ssize_t fs_write_attr(int, const char *, unsigned long, off_t, const void *, size_t);\n"
        "ssize_t fs_read_attr(int, const char *, unsigned long, off_t, void *, size_t);\n"
        "#endif\n", encoding="ascii")
    stub_impl = work / "fs_attr_stub.c"
    stub_impl.write_text(
        "#include <sys/types.h>\n#include <stddef.h>\n"
        "#include <sys/xattr.h>\n"
        "ssize_t fs_write_attr(int a,const char*b,unsigned long c,off_t d,const void*e,size_t f)"
        "{(void)b;(void)c;if(d!=0||f==0)return -1;"
        "return fsetxattr(a,\"user.haiku.BEOS_TYPE\",e,f,0)==0?(ssize_t)f:-1;}\n"
        "ssize_t fs_read_attr(int a,const char*b,unsigned long c,off_t d,void*e,size_t f)"
        "{(void)b;(void)c;if(d!=0)return -1;"
        "return fgetxattr(a,\"user.haiku.BEOS_TYPE\",e,f);}\n",
        encoding="ascii")
    binary = work / "boltpack-haiku-host"
    host_compile = [args.cc, "-ansi", "-pedantic", "-Wall", "-Wextra",
         "-Wno-unused-function", "-Wno-overlength-strings", "-D_DEFAULT_SOURCE",
         "-I" + str(stub), "-I" + str(ROOT / "src/core"),
         str(ROOT / "src/core/archiver.c"),
         str(ROOT / "src/core/compress.c"),
         str(ROOT / "src/core/boltlite.c"),
         str(ROOT / "src/core/fluff.c"),
         str(ROOT / "src/core/bp_codepage.c"),
         str(ROOT / "src/frontend/haiku/main.c"),
         str(ROOT / "src/core/boltlz.c"), str(stub_impl), "-lm", "-o", str(binary)]
    run(host_compile)
    conflict_fixture = work / "haiku-name-conflict-fixture"
    run([args.cc, "-ansi", "-pedantic", "-Wall", "-Wextra",
         "-D_DEFAULT_SOURCE", "-I" + str(ROOT / "src/core"),
         str(ROOT / "test/haiku_name_conflict_fixture.c"),
         str(ROOT / "src/core/archiver.c"),
         str(ROOT / "src/core/compress.c"),
         str(ROOT / "src/core/boltlite.c"),
         str(ROOT / "src/core/fluff.c"),
         str(ROOT / "src/core/bp_codepage.c"),
         str(ROOT / "src/core/boltlz.c"), "-lm", "-o",
         str(conflict_fixture)])
    rename_interposer = work / "fail_rename.so"
    interposer_source = work / "fail_rename.c"
    interposer_source.write_text(
        "#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <errno.h>\n"
        "int rename(const char *old_path, const char *new_path) {\n"
        "  (void)old_path; (void)new_path; errno = EIO; return -1;\n}\n",
        encoding="ascii")
    run([args.cc, "-shared", "-fPIC", str(interposer_source), "-ldl",
         "-o", str(rename_interposer)])

    data = b"Haiku staged extraction checksum regression\n" * 40
    (work / "source.bin").write_bytes(data)

    # The Haiku metadata format has a one-byte MIME length. Exercise the
    # host fs_attr shim with a boundary-sized BFS attribute and verify both
    # archive encoding and extraction restoration.
    mime_value = b"application/x-" + b"m" * 242
    if len(mime_value) != 256:
        raise RuntimeError("MIME metadata boundary fixture is malformed")
    mime_source = work / "mime-source.bin"
    mime_source.write_bytes(b"native MIME attribute payload\n")
    os.chmod(mime_source, 0o640)
    os.setxattr(mime_source, "user.haiku.BEOS_TYPE", mime_value)
    run([str(binary), "c", "-0", "mime.blt", "mime-source.bin"], cwd=work)
    meta_payload = haiku_meta_payload(work / "mime.blt", "mime-source.bin")
    expected_mime = mime_value[:255]
    if len(meta_payload) != 258 or meta_payload[2] != 255 or \
            meta_payload[3:] != expected_mime:
        raise RuntimeError("Haiku MIME metadata did not honor its 255-byte field")
    mime_output = work / "mime-output"
    mime_output.mkdir()
    run([str(binary), "x", "-o", str(mime_output), str(work / "mime.blt")])
    restored_mime = os.getxattr(mime_output / "mime-source.bin",
                                "user.haiku.BEOS_TYPE")
    if restored_mime != expected_mime + b"\0":
        raise RuntimeError("Haiku MIME metadata did not round-trip via fs_attr")
    precedence_archive = work / "native-precedence.blt"
    set_first_entry_mode(work / "mime.blt", precedence_archive, 0)
    precedence_output = work / "native-precedence-output"
    precedence_output.mkdir()
    run([str(binary), "x", "-o", str(precedence_output), str(precedence_archive)])
    precedence_file = precedence_output / "mime-source.bin"
    if (stat.S_IMODE(precedence_file.stat().st_mode) != 0o640 or
            os.getxattr(precedence_file, "user.haiku.BEOS_TYPE") != expected_mime + b"\0" or
            precedence_file.read_bytes() != b"native MIME attribute payload\n"):
        raise RuntimeError("native file metadata did not override inaccessible portable mode")

    # Replace the published path before its META entry is processed. The
    # operation must report partial and leave both objects' metadata untouched.
    swap_file_source = work / "swap-published-file.c"
    swap_file_source.write_text(
        "#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <stdio.h>\n"
        "#include <stdlib.h>\n#include <string.h>\n#include <unistd.h>\n"
        "int renameat(int fromfd,const char *from,int tofd,const char *to) {\n"
        "  static int (*real_renameat)(int,const char *,int,const char *); static int done;\n"
        "  int rc;\n"
        "  if (!real_renameat) real_renameat = dlsym(RTLD_NEXT, \"renameat\");\n"
        "  rc = real_renameat(fromfd,from,tofd,to); if (rc || done ||\n"
        "      strncmp(from,\".boltpack-\",10) || strcmp(to,\"mime-source.bin\")) return rc;\n"
        "  done=1;\n"
        "  if (real_renameat(tofd,to,tofd,\"mime-source.bin.moved\") ||\n"
        "      symlinkat(getenv(\"HAIKU_FILE_REDIRECT\"),tofd,to)) return -1;\n"
        "  return rc;\n}\n", encoding="ascii")
    swap_file_library = work / "swap-published-file.so"
    run([args.cc, "-shared", "-fPIC", str(swap_file_source), "-ldl",
         "-o", str(swap_file_library)])
    outside_file = work / "outside-file.bin"
    outside_file.write_bytes(b"outside file sentinel\n")
    os.chmod(outside_file, 0o600)
    os.setxattr(outside_file, "user.haiku.BEOS_TYPE", b"application/x-outside-file\0")
    swap_file_output = work / "swap-file-output"
    swap_file_output.mkdir()
    (swap_file_output / "mime-source.bin").write_bytes(b"old destination\n")
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(swap_file_library)
    env["HAIKU_FILE_REDIRECT"] = str(outside_file)
    run([str(binary), "x", "-f", "-o", str(swap_file_output),
         str(precedence_archive)], env=env, expected=1)
    moved_file = swap_file_output / "mime-source.bin.moved"
    if not (swap_file_output / "mime-source.bin").is_symlink() or \
            stat.S_IMODE(moved_file.stat().st_mode) != 0 or \
            "user.haiku.BEOS_TYPE" in os.listxattr(moved_file):
        raise RuntimeError("published-file substitution applied metadata to the displaced file")
    os.chmod(moved_file, 0o600)
    if moved_file.read_bytes() != b"native MIME attribute payload\n":
        raise RuntimeError("published-file substitution lost verified file data")
    if outside_file.read_bytes() != b"outside file sentinel\n" or \
            stat.S_IMODE(outside_file.stat().st_mode) != 0o600 or \
            os.getxattr(outside_file, "user.haiku.BEOS_TYPE") != b"application/x-outside-file\0":
        raise RuntimeError("published-file substitution modified the symlink target")

    # A Linux archive with no Haiku metadata must also detect substitution.
    run([str(args.linux_boltpack.resolve()), "c", "-0", "file-only.blt",
         mime_source.name], cwd=work)
    no_meta_output = work / "swap-file-no-meta-output"
    no_meta_output.mkdir()
    (no_meta_output / "mime-source.bin").write_bytes(b"old destination\n")
    run([str(binary), "x", "-f", "-o", str(no_meta_output),
         str(work / "file-only.blt")], env=env, expected=1)
    no_meta_moved = no_meta_output / "mime-source.bin.moved"
    if not (no_meta_output / "mime-source.bin").is_symlink() or \
            no_meta_moved.read_bytes() != b"native MIME attribute payload\n" or \
            stat.S_IMODE(no_meta_moved.stat().st_mode) != 0o640 or \
            outside_file.read_bytes() != b"outside file sentinel\n" or \
            stat.S_IMODE(outside_file.stat().st_mode) != 0o600 or \
            os.getxattr(outside_file, "user.haiku.BEOS_TYPE") != b"application/x-outside-file\0":
        raise RuntimeError("file substitution without native metadata was not safely detected")

    # Swap after the initial publication check, during native metadata
    # restoration. The final subject check must still report partial.
    late_swap_source = work / "swap-file-during-metadata.c"
    late_swap_source.write_text(
        "#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <stdlib.h>\n"
        "#include <string.h>\n#include <stdio.h>\n#include <unistd.h>\n"
        "#include <sys/types.h>\n#include <sys/stat.h>\n"
        "int fchmod(int fd, mode_t mode) {\n"
        "  static int (*real_fchmod)(int, mode_t); static int done;\n"
        "  char link[64], path[4096], moved[4096]; ssize_t n; int rc;\n"
        "  if(!real_fchmod) real_fchmod=dlsym(RTLD_NEXT,\"fchmod\");\n"
        "  rc=real_fchmod(fd,mode); if(rc || done) return rc;\n"
        "  snprintf(link,sizeof(link),\"/proc/self/fd/%d\",fd);\n"
        "  n=readlink(link,path,sizeof(path)-1); if(n<15) return rc; path[n]=0;\n"
        "  if(strcmp(path+n-15,\"mime-source.bin\")) return rc; done=1;\n"
        "  snprintf(moved,sizeof(moved),\"%s.moved\",path);\n"
        "  if(rename(path,moved) || symlink(getenv(\"HAIKU_FILE_REDIRECT\"),path)) return -1;\n"
        "  return rc;\n}\n", encoding="ascii")
    late_swap_library = work / "swap-file-during-metadata.so"
    run([args.cc, "-shared", "-fPIC", str(late_swap_source), "-ldl",
         "-o", str(late_swap_library)])
    late_output = work / "late-swap-file-output"
    late_output.mkdir()
    (late_output / "mime-source.bin").write_bytes(b"old destination\n")
    env["LD_PRELOAD"] = str(late_swap_library)
    run([str(binary), "x", "-f", "-o", str(late_output), str(precedence_archive)],
        env=env, expected=1)
    late_moved = late_output / "mime-source.bin.moved"
    if not (late_output / "mime-source.bin").is_symlink() or \
            late_moved.read_bytes() != b"native MIME attribute payload\n" or \
            stat.S_IMODE(late_moved.stat().st_mode) != 0o640 or \
            os.getxattr(late_moved, "user.haiku.BEOS_TYPE") != expected_mime + b"\0" or \
            outside_file.read_bytes() != b"outside file sentinel\n" or \
            stat.S_IMODE(outside_file.stat().st_mode) != 0o600 or \
            os.getxattr(outside_file, "user.haiku.BEOS_TYPE") != b"application/x-outside-file\0":
        raise RuntimeError("late publication substitution was not safely detected")
    mime_dir = work / "mime-dir"
    mime_dir.mkdir()
    (mime_dir / "child.bin").write_bytes(b"directory metadata child\n")
    os.setxattr(mime_dir, "user.haiku.BEOS_TYPE", b"application/x-directory\0")
    run([str(binary), "c", "-0", "mime-dir.blt", "mime-dir"], cwd=work)
    if haiku_meta_payload(work / "mime-dir.blt", "mime-dir/")[3:] != \
            b"application/x-directory":
        raise RuntimeError("Haiku directory MIME attribute was not archived")
    mime_dir_output = work / "mime-dir-output"
    mime_dir_output.mkdir()
    run([str(binary), "x", "-o", str(mime_dir_output),
         str(work / "mime-dir.blt")])
    restored_dir_mime = os.getxattr(mime_dir_output / "mime-dir",
                                    "user.haiku.BEOS_TYPE")
    if restored_dir_mime != b"application/x-directory\0":
        raise RuntimeError("Haiku directory MIME attribute was not restored")

    # A failed directory selection must not apply metadata to an existing one.
    fail_mkdir_source = work / "fail-directory.c"
    fail_mkdir_source.write_text(
        "#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <errno.h>\n"
        "#include <string.h>\n#include <stdarg.h>\n#include <fcntl.h>\n"
        "int openat(int d,const char *p,int f,...) {\n"
        "  static int (*real_openat)(int,const char *,int,...); va_list ap; mode_t m=0;\n"
        "  if (strcmp(p,\"mime-dir\") == 0) { errno = EACCES; return -1; }\n"
        "  if (!real_openat) real_openat = dlsym(RTLD_NEXT, \"openat\");\n"
        "  if (f & O_CREAT) { va_start(ap,f); m=va_arg(ap,int); va_end(ap); return real_openat(d,p,f,m); }\n"
        "  return real_openat(d,p,f);\n}\n", encoding="ascii")
    fail_mkdir = work / "fail-directory.so"
    run([args.cc, "-shared", "-fPIC", str(fail_mkdir_source), "-ldl",
         "-o", str(fail_mkdir)])
    failed_output = work / "failed-directory-output"
    failed_directory = failed_output / "mime-dir"
    failed_directory.mkdir(parents=True)
    os.chmod(failed_directory, 0o700)
    original_mime = b"application/x-original\0"
    os.setxattr(failed_directory, "user.haiku.BEOS_TYPE", original_mime)
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(fail_mkdir)
    failure = subprocess.run([str(binary), "x", "-o", str(failed_output),
                              str(work / "mime-dir.blt")], cwd=work,
                             env=env, capture_output=True)
    if failure.returncode == 0:
        raise RuntimeError("failed Haiku directory creation was reported as success")
    if stat.S_IMODE(failed_directory.stat().st_mode) != 0o700 or \
            os.getxattr(failed_directory, "user.haiku.BEOS_TYPE") != original_mime:
        raise RuntimeError("failed Haiku directory selection changed existing metadata")

    # A parent without write permission must not be finalized until its child
    # and grandchild have been extracted. Pin both directory times and MIME.
    ordered = work / "ordered"
    nested = ordered / "nested"
    nested.mkdir(parents=True)
    (nested / "child.bin").write_bytes(b"deferred directory metadata\n")
    os.setxattr(ordered, "user.haiku.BEOS_TYPE", b"application/x-parent\0")
    os.setxattr(nested, "user.haiku.BEOS_TYPE", b"application/x-child\0")
    os.chmod(nested, 0o500)
    os.chmod(ordered, 0o550)
    os.utime(nested, (1_600_000_001, 1_600_000_001))
    os.utime(ordered, (1_600_000_000, 1_600_000_000))
    run([str(binary), "c", "-0", "ordered.blt", "ordered"], cwd=work)
    dry_output = work / "ordered-dry-output"
    run([str(binary), "x", "-n", "-o", str(dry_output),
         str(work / "ordered.blt")])
    if dry_output.exists():
        raise RuntimeError("Haiku dry-run created an output directory")
    ordered_output = work / "ordered-output"
    ordered_output.mkdir()
    run([str(binary), "x", "-o", str(ordered_output), str(work / "ordered.blt")])
    parent_out = ordered_output / "ordered"
    child_out = parent_out / "nested"
    if (child_out / "child.bin").read_bytes() != b"deferred directory metadata\n":
        raise RuntimeError("deferred directory extraction changed child data")
    for path, mode, timestamp, mime in (
            (parent_out, 0o550, 1_600_000_000, b"application/x-parent\0"),
            (child_out, 0o500, 1_600_000_001, b"application/x-child\0")):
        if (stat.S_IMODE(path.stat().st_mode) != mode or
                int(path.stat().st_mtime) != timestamp or
                os.getxattr(path, "user.haiku.BEOS_TYPE") != mime):
            raise RuntimeError("directory metadata was not finalized after children: " + str(path))

    fail_chmod_source = work / "fail-directory-chmod.c"
    fail_chmod_source.write_text(
        "#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <errno.h>\n"
        "#include <string.h>\n#include <stdio.h>\n#include <unistd.h>\n"
        "#include <sys/types.h>\n#include <sys/stat.h>\n"
        "int fchmod(int fd, mode_t mode) {\n"
        "  static int (*real_fchmod)(int, mode_t); char link[64], path[4096]; ssize_t n;\n"
        "  snprintf(link, sizeof(link), \"/proc/self/fd/%d\", fd);\n"
        "  n = readlink(link, path, sizeof(path) - 1); if (n >= 0) path[n] = 0;\n"
        "  if (n >= 8 && strcmp(path + n - 8, \"/ordered\") == 0) "
        "{ errno = EACCES; return -1; }\n"
        "  if (!real_fchmod) real_fchmod = dlsym(RTLD_NEXT, \"fchmod\");\n"
        "  return real_fchmod(fd, mode);\n}\n", encoding="ascii")
    fail_chmod = work / "fail-directory-chmod.so"
    run([args.cc, "-shared", "-fPIC", str(fail_chmod_source), "-ldl",
         "-o", str(fail_chmod)])
    failed_finish = work / "failed-directory-finish"
    failed_finish.mkdir()
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(fail_chmod)
    failure = subprocess.run([str(binary), "x", "-o", str(failed_finish),
                              str(work / "ordered.blt")], cwd=work,
                             env=env, capture_output=True)
    if failure.returncode == 0 or b"cannot restore directory metadata" not in failure.stderr:
        raise RuntimeError("deferred directory metadata failure was not reported")
    if (failed_finish / "ordered/nested/child.bin").read_bytes() != \
            b"deferred directory metadata\n":
        raise RuntimeError("directory metadata failure lost verified child data")
    if list(failed_finish.rglob(".boltpack-*")):
        raise RuntimeError("directory metadata failure left extraction stages")

    fail_time_source = work / "fail-directory-time.c"
    fail_time_source.write_text(
        "#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <errno.h>\n"
        "#include <string.h>\n#include <stdio.h>\n#include <unistd.h>\n"
        "#include <sys/stat.h>\n#include <time.h>\n"
        "int futimens(int fd, const struct timespec times[2]) {\n"
        "  static int (*real_futimens)(int, const struct timespec *);\n"
        "  char link[64], path[4096]; ssize_t n;\n"
        "  snprintf(link, sizeof(link), \"/proc/self/fd/%d\", fd);\n"
        "  n = readlink(link, path, sizeof(path) - 1); if (n >= 0) path[n] = 0;\n"
        "  if (n >= 8 && strcmp(path + n - 8, \"/ordered\") == 0) "
        "{ errno = EIO; return -1; }\n"
        "  if (!real_futimens) real_futimens = dlsym(RTLD_NEXT, \"futimens\");\n"
        "  return real_futimens(fd, times);\n}\n", encoding="ascii")
    fail_time = work / "fail-directory-time.so"
    run([args.cc, "-shared", "-fPIC", str(fail_time_source), "-ldl",
         "-o", str(fail_time)])
    failed_time = work / "failed-directory-time"
    failed_time.mkdir()
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(fail_time)
    failure = subprocess.run([str(binary), "x", "-o", str(failed_time),
                              str(work / "ordered.blt")], cwd=work,
                             env=env, capture_output=True)
    if failure.returncode == 0 or b"cannot restore directory metadata" not in failure.stderr:
        raise RuntimeError("deferred directory timestamp failure was not reported")
    if (failed_time / "ordered/nested/child.bin").read_bytes() != \
            b"deferred directory metadata\n":
        raise RuntimeError("directory timestamp failure lost verified child data")

    # Replace the queued parent with a symlink while its child is finalized.
    # The child's still-open descriptor and the parent's identity check must
    # keep the redirected directory and its metadata untouched.
    swap_source = work / "swap-directory-parent.c"
    swap_source.write_text(
        "#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <stdlib.h>\n"
        "#include <string.h>\n#include <stdio.h>\n#include <unistd.h>\n"
        "#include <sys/types.h>\n#include <sys/stat.h>\n"
        "int fchmod(int fd, mode_t mode) {\n"
        "  static int (*real_fchmod)(int, mode_t); static int done;\n"
        "  char link[64], path[4096], parent[4096], backup[4096]; ssize_t n; int rc;\n"
        "  if (!real_fchmod) real_fchmod = dlsym(RTLD_NEXT, \"fchmod\");\n"
        "  rc = real_fchmod(fd, mode); if (rc || done) return rc;\n"
        "  snprintf(link, sizeof(link), \"/proc/self/fd/%d\", fd);\n"
        "  n = readlink(link, path, sizeof(path)-1); if (n < 15) return rc; path[n]=0;\n"
        "  if (strcmp(path+n-15, \"/ordered/nested\")) return rc; done=1;\n"
        "  path[n-7]=0; strcpy(parent,path); snprintf(backup,sizeof(backup),\"%s.moved\",parent);\n"
        "  if (rename(parent,backup) || symlink(getenv(\"HAIKU_TEST_REDIRECT\"),parent)) return -1;\n"
        "  return rc;\n}\n", encoding="ascii")
    swap_library = work / "swap-directory-parent.so"
    run([args.cc, "-shared", "-fPIC", str(swap_source), "-ldl",
         "-o", str(swap_library)])
    redirected = work / "redirected"
    redirected_nested = redirected / "nested"
    redirected_nested.mkdir(parents=True)
    (redirected_nested / "child.bin").write_bytes(b"outside sentinel\n")
    os.setxattr(redirected, "user.haiku.BEOS_TYPE", b"application/x-outside\0")
    os.setxattr(redirected_nested, "user.haiku.BEOS_TYPE", b"application/x-outside-child\0")
    os.chmod(redirected, 0o710)
    os.chmod(redirected_nested, 0o710)
    os.utime(redirected_nested, (1_550_000_001, 1_550_000_001))
    os.utime(redirected, (1_550_000_000, 1_550_000_000))
    swapped_output = work / "swapped-directory-output"
    swapped_output.mkdir()
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(swap_library)
    env["HAIKU_TEST_REDIRECT"] = str(redirected)
    failure = subprocess.run([str(binary), "x", "-o", str(swapped_output),
                              str(work / "ordered.blt")], cwd=work,
                             env=env, capture_output=True)
    if failure.returncode == 0 or not (swapped_output / "ordered").is_symlink():
        raise RuntimeError("directory substitution did not report partial or was not injected")
    for path, timestamp, mime in (
            (redirected, 1_550_000_000, b"application/x-outside\0"),
            (redirected_nested, 1_550_000_001, b"application/x-outside-child\0")):
        if (stat.S_IMODE(path.stat().st_mode) != 0o710 or
                int(path.stat().st_mtime) != timestamp or
                os.getxattr(path, "user.haiku.BEOS_TYPE") != mime):
            raise RuntimeError("queued metadata reached substituted directory: " + str(path))
    if (redirected_nested / "child.bin").read_bytes() != b"outside sentinel\n" or \
            (swapped_output / "ordered.moved/nested/child.bin").read_bytes() != \
            b"deferred directory metadata\n":
        raise RuntimeError("directory substitution lost or changed file bytes")

    portable_dir = work / "portable-dir"
    portable_dir.mkdir()
    (portable_dir / "child.txt").write_bytes(b"portable directory mode\n")
    os.chmod(portable_dir, 0o550)
    os.utime(portable_dir, (0, 0))
    run([str(args.linux_boltpack.resolve()), "c", "-0", "portable-dir.blt",
         "portable-dir"], cwd=work)
    portable_output = work / "portable-dir-output"
    portable_output.mkdir()
    run([str(binary), "x", "-o", str(portable_output),
         str(work / "portable-dir.blt")])
    portable_restored = portable_output / "portable-dir"
    if (stat.S_IMODE(portable_restored.stat().st_mode) != 0o550 or
            int(portable_restored.stat().st_mtime) != 0 or
            (portable_restored / "child.txt").read_bytes() != b"portable directory mode\n"):
        raise RuntimeError("portable directory mode or epoch-zero timestamp not restored")

    portable_file = work / "portable-mode.txt"
    portable_file.write_bytes(b"portable file mode\n")
    os.chmod(portable_file, 0o751)
    os.utime(portable_file, (0, 0))
    run([str(args.linux_boltpack.resolve()), "c", "-0", "portable-mode.blt",
         portable_file.name], cwd=work)
    portable_file_output = work / "portable-file-output"
    portable_file_output.mkdir()
    portable_target = portable_file_output / portable_file.name
    portable_target.write_bytes(b"replace me\n")
    os.chmod(portable_target, 0o600)
    run([str(binary), "x", "-f", "-o", str(portable_file_output),
         str(work / "portable-mode.blt")])
    if (stat.S_IMODE(portable_target.stat().st_mode) != 0o751 or
            int(portable_target.stat().st_mtime) != 0 or
            portable_target.read_bytes() != b"portable file mode\n"):
        raise RuntimeError("portable file mode or epoch-zero timestamp not restored")
    setid_archive = work / "portable-setid.blt"
    set_first_entry_mode(work / "portable-mode.blt", setid_archive, 0o6751)
    setid_output = work / "portable-setid-output"
    setid_output.mkdir()
    run([str(binary), "x", "-o", str(setid_output), str(setid_archive)], expected=1)
    setid_file = setid_output / portable_file.name
    if stat.S_IMODE(setid_file.stat().st_mode) != 0o751 or \
            setid_file.read_bytes() != b"portable file mode\n":
        raise RuntimeError("set-ID mode was not stripped while preserving verified bytes")
    fail_file_mode_source = work / "fail-file-mode.c"
    fail_file_mode_source.write_text(
        "#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <errno.h>\n"
        "#include <string.h>\n#include <stdio.h>\n#include <unistd.h>\n"
        "#include <sys/types.h>\n#include <sys/stat.h>\n"
        "int fchmod(int fd, mode_t mode) {\n"
        "  static int (*real_fchmod)(int, mode_t); char link[64], path[4096]; ssize_t n;\n"
        "  snprintf(link,sizeof(link),\"/proc/self/fd/%d\",fd);\n"
        "  n=readlink(link,path,sizeof(path)-1); if(n>=0) path[n]=0;\n"
        "  if(n>=0 && strstr(path,\"/.boltpack-\")) {errno=EACCES; return -1;}\n"
        "  if(!real_fchmod) real_fchmod=dlsym(RTLD_NEXT,\"fchmod\");\n"
        "  return real_fchmod(fd,mode);\n}\n", encoding="ascii")
    fail_file_mode = work / "fail-file-mode.so"
    run([args.cc, "-shared", "-fPIC", str(fail_file_mode_source), "-ldl",
         "-o", str(fail_file_mode)])
    file_mode_failure = work / "failed-file-mode-output"
    file_mode_failure.mkdir()
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(fail_file_mode)
    run([str(binary), "x", "-o", str(file_mode_failure),
         str(work / "portable-mode.blt")], expected=1, env=env)
    if (file_mode_failure / portable_file.name).read_bytes() != b"portable file mode\n" or \
            list(file_mode_failure.glob(".boltpack-*")):
        raise RuntimeError("failed portable mode restoration lost data or left a stage")
    fail_dup_source = work / "fail-subject-dup.c"
    fail_dup_source.write_text(
        "#define _GNU_SOURCE\n#include <errno.h>\n#include <dlfcn.h>\n"
        "#include <stdio.h>\n#include <string.h>\n#include <unistd.h>\n"
        "int dup(int fd) { static int (*real_dup)(int); char link[64], path[4096]; ssize_t n;\n"
        "  snprintf(link,sizeof(link),\"/proc/self/fd/%d\",fd); n=readlink(link,path,sizeof(path)-1);\n"
        "  if(n>=0) { path[n]=0; if(strstr(path,\"/.boltpack-\")) {errno=EMFILE; return -1;} }\n"
        "  if(!real_dup) real_dup=dlsym(RTLD_NEXT,\"dup\"); return real_dup(fd); }\n", encoding="ascii")
    fail_dup = work / "fail-subject-dup.so"
    run([args.cc, "-shared", "-fPIC", str(fail_dup_source), "-o", str(fail_dup)])
    failed_pin_output = work / "failed-file-pin-output"
    failed_pin_output.mkdir()
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(fail_dup)
    run([str(binary), "x", "-o", str(failed_pin_output),
         str(work / "portable-mode.blt")], expected=1, env=env)
    if (failed_pin_output / portable_file.name).read_bytes() != b"portable file mode\n" or \
            list(failed_pin_output.glob(".boltpack-*")):
        raise RuntimeError("failed published-file pin lost data or left a stage")

    pre_epoch_data = b"pre-epoch Haiku timestamp regression\n"
    (work / "pre-epoch.bin").write_bytes(pre_epoch_data)
    os.utime(work / "pre-epoch.bin", (-1, -1))
    pre_epoch_dir = work / "pre-epoch-dir"
    pre_epoch_dir.mkdir()
    (pre_epoch_dir / "child.bin").write_bytes(data[:64])
    os.utime(pre_epoch_dir / "child.bin", (-1, -1))
    os.utime(pre_epoch_dir, (-1, -1))
    pre_epoch_archive = work / "pre-epoch.blt"
    run([str(binary), "c", "-0", str(pre_epoch_archive),
         "pre-epoch.bin", "pre-epoch-dir"], cwd=work)
    pre_epoch_entries = primary_times(pre_epoch_archive)
    for expected_name in ("pre-epoch.bin", "pre-epoch-dir/",
                          "pre-epoch-dir/child.bin"):
        if expected_name not in pre_epoch_entries:
            raise RuntimeError("Haiku timestamp fixture omitted " + expected_name)
        mtime, flags = pre_epoch_entries[expected_name]
        if mtime != 0 or flags & 0x4000:
            raise RuntimeError("Haiku encoded a pre-epoch source time as known")
    linux = args.linux_boltpack.resolve()
    run([str(linux), "t", str(pre_epoch_archive)])
    pre_epoch_output = work / "pre-epoch-output"
    run([str(linux), "x", "-o", str(pre_epoch_output),
         str(pre_epoch_archive)], expected=1)
    if (pre_epoch_output / "pre-epoch.bin").read_bytes() != pre_epoch_data or \
            (pre_epoch_output / "pre-epoch-dir" / "child.bin").read_bytes() != data[:64]:
        raise RuntimeError("Linux did not recover Haiku pre-epoch payload bytes")

    # Failed archive edits must leave an existing target untouched and clean
    # their sibling stages. Exercise create, append, and delete separately.
    failed_create = work / "failed-create.blt"
    create_sentinel = b"existing create target must survive\n"
    failed_create.write_bytes(create_sentinel)
    run([str(binary), "c", str(failed_create), "missing-source.bin"],
        cwd=work, expected=1)
    if failed_create.read_bytes() != create_sentinel or \
            list(work.glob(".boltpack-*")):
        raise RuntimeError("failed create changed its target or left a stage")

    (work / "extra.bin").write_bytes(b"profile append payload\n")
    run([str(binary), "c", "-0", "edit.blt", "source.bin"], cwd=work)
    edit_target = work / "edit.blt"
    edit_target.chmod(0o640)
    before_failed_append = edit_target.read_bytes()
    run([str(binary), "a", str(edit_target), "missing-source.bin"],
        cwd=work, expected=1)
    if edit_target.read_bytes() != before_failed_append or \
            list(work.glob(".boltpack-*")):
        raise RuntimeError("failed append changed its target or left a stage")
    injected_env = os.environ.copy()
    injected_env["LD_PRELOAD"] = str(rename_interposer)
    run([str(binary), "a", str(edit_target), "extra.bin"], cwd=work,
        expected=1, env=injected_env)
    if edit_target.read_bytes() != before_failed_append or \
            list(work.glob(".boltpack-*")):
        raise RuntimeError("failed append publication changed its target or left a stage")
    run([str(binary), "d", str(edit_target), "absent.bin"],
        cwd=work, expected=1)
    if edit_target.read_bytes() != before_failed_append or \
            list(work.glob(".boltpack-*")):
        raise RuntimeError("no-match delete changed its target or left a stage")

    run([str(binary), "a", str(edit_target), "extra.bin"], cwd=work)
    if stat.S_IMODE(edit_target.stat().st_mode) != 0o640:
        raise RuntimeError("successful append did not preserve archive mode")
    run([str(args.linux_boltpack.resolve()), "t", str(edit_target)])
    run([str(binary), "d", str(edit_target), "source.bin"], cwd=work)
    if stat.S_IMODE(edit_target.stat().st_mode) != 0o640:
        raise RuntimeError("successful delete did not preserve archive mode")
    listing = run([str(args.linux_boltpack.resolve()), "l", str(edit_target)])
    if b"extra.bin" not in listing or b"source.bin" in listing:
        raise RuntimeError("successful Haiku archive edits produced wrong contents")
    run([str(binary), "c", str(edit_target), "source.bin"], cwd=work)
    if stat.S_IMODE(edit_target.stat().st_mode) != 0o640:
        raise RuntimeError("create-overwrite did not preserve archive mode")
    run([str(args.linux_boltpack.resolve()), "t", str(edit_target)])
    if list(work.glob(".boltpack-*")):
        raise RuntimeError("successful archive edits left a sibling stage")

    conflict_archive = work / "path-conflict.blt"
    run([str(conflict_fixture), str(conflict_archive)])
    conflict_output = work / "conflict-output"
    run([str(binary), "x", "-o", str(conflict_output),
         str(conflict_archive)], expected=1)
    if conflict_output.exists():
        raise RuntimeError("path-conflict preflight created the output directory")

    run([str(binary), "c", "-0", "good.blt", "source.bin"], cwd=work)
    raw = (work / "good.blt").read_bytes()
    offset = raw.find(data)
    if offset < 0 or raw.find(data, offset + 1) >= 0:
        raise RuntimeError("could not locate unique Stored payload in archive")
    damaged = bytearray(raw)
    damaged[offset + len(data) // 2] ^= 0x40
    (work / "damaged.blt").write_bytes(damaged)

    output = work / "output"
    output.mkdir()
    target = output / "source.bin"
    old_data = b"preserve this existing file\n"
    target.write_bytes(old_data)
    run([str(binary), "x", "-f", "-o", str(output), str(work / "damaged.blt")],
        expected=1)
    if target.read_bytes() != old_data:
        raise RuntimeError("checksum failure changed the existing destination")
    if list(output.glob(".boltpack-*")):
        raise RuntimeError("checksum failure left an extraction stage")

    fresh = work / "fresh"
    fresh.mkdir()
    run([str(binary), "x", "-o", str(fresh), str(work / "damaged.blt")],
        expected=1)
    if (fresh / "source.bin").exists() or list(fresh.glob(".boltpack-*")):
        raise RuntimeError("checksum failure published output for a new destination")

    run([str(binary), "x", "-f", "-o", str(output), str(work / "good.blt")])
    if target.read_bytes() != data:
        raise RuntimeError("verified archive did not replace the destination")
    if list(output.glob(".boltpack-*")):
        raise RuntimeError("successful extraction left an extraction stage")

    target.write_bytes(old_data)
    run([str(binary), "x", "-o", str(output), str(work / "good.blt")],
        expected=1)
    if target.read_bytes() != old_data or list(output.glob(".boltpack-*")):
        raise RuntimeError("no-force extraction changed an existing destination")

    (work / "safe-link").symlink_to("source.bin")
    os.utime(work / "safe-link", (1_700_000_123, 1_700_000_123),
             follow_symlinks=False)
    run([str(binary), "c", "-0", "safe-link.blt", "safe-link"], cwd=work)
    link_out = work / "link-output"
    link_out.mkdir()
    link_target = link_out / "source.bin"
    link_target.write_bytes(b"target must not be modified\n")
    os.utime(link_target, (1_600_000_000, 1_600_000_000))
    run([str(binary), "x", "-o", str(link_out), str(work / "safe-link.blt")])
    if not (link_out / "safe-link").is_symlink() or \
            (link_out / "safe-link").readlink() != Path("source.bin"):
        raise RuntimeError("verified safe symlink was not published correctly")
    if int(os.lstat(link_out / "safe-link").st_mtime) != 1_700_000_123 or \
            int(link_target.stat().st_mtime) != 1_600_000_000 or \
            link_target.read_bytes() != b"target must not be modified\n":
        raise RuntimeError("symlink timestamp was lost or its target was modified")
    if list(link_out.glob(".boltpack-*")):
        raise RuntimeError("successful symlink extraction left a stage")

    # Fail the no-follow timestamp syscall and prove publication is withheld.
    fail_link_time_source = work / "fail-link-time.c"
    fail_link_time_source.write_text(
        "#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <errno.h>\n"
        "#include <fcntl.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#include <sys/stat.h>\n#include <time.h>\n#include <unistd.h>\n"
        "int utimensat(int dirfd,const char *path,const struct timespec ts[2],int flags) {\n"
        "  static int (*real_fn)(int,const char *,const struct timespec *,int);\n"
        "  if ((flags & AT_SYMLINK_NOFOLLOW) && strstr(path,\".boltpack-\")) {\n"
        "    const char *marker=getenv(\"BP_TEST_LINK_TIME_MARKER\");\n"
        "    if(marker) {int fd=open(marker,O_WRONLY|O_CREAT|O_TRUNC,0600); if(fd>=0) close(fd);}\n"
        "    errno=EIO; return -1; }\n"
        "  if(!real_fn) real_fn=dlsym(RTLD_NEXT,\"utimensat\");\n"
        "  return real_fn(dirfd,path,ts,flags);\n}\n", encoding="ascii")
    fail_link_time = work / "fail-link-time.so"
    run([args.cc, "-shared", "-fPIC", str(fail_link_time_source), "-ldl",
         "-o", str(fail_link_time)])
    for existing in (False, True):
        failed_time_output = work / ("link-time-failed-existing" if existing else "link-time-failed-new")
        failed_time_output.mkdir()
        old_target = failed_time_output / "old-target"
        old_target.write_bytes(b"old target stays unchanged\n")
        os.utime(old_target, (1_500_000_000, 1_500_000_000))
        old_link = failed_time_output / "safe-link"
        if existing:
            old_link.symlink_to("old-target")
            os.utime(old_link, (1_500_000_001, 1_500_000_001), follow_symlinks=False)
        marker = work / ("link-time-marker-existing" if existing else "link-time-marker-new")
        env = os.environ.copy()
        env["LD_PRELOAD"] = str(fail_link_time)
        env["BP_TEST_LINK_TIME_MARKER"] = str(marker)
        run([str(binary), "x", "-f", "-o", str(failed_time_output),
             str(work / "safe-link.blt")], expected=1, env=env)
        if not marker.exists() or \
                (existing and (not old_link.is_symlink() or
                               old_link.readlink() != Path("old-target") or
                               int(os.lstat(old_link).st_mtime) != 1_500_000_001)) or \
                (not existing and (old_link.exists() or old_link.is_symlink())) or \
                old_target.read_bytes() != b"old target stays unchanged\n" or \
                int(old_target.stat().st_mtime) != 1_500_000_000 or \
                list(failed_time_output.glob(".boltpack-*")):
            raise RuntimeError("failed symlink timestamp restoration modified a destination or target")

    unknown_link = work / "unknown-link"
    unknown_link.symlink_to("source.bin")
    os.utime(unknown_link, (-1, -1), follow_symlinks=False)
    run([str(binary), "c", "-0", "unknown-link.blt", "unknown-link"], cwd=work)
    unknown_time, unknown_flags = primary_times(work / "unknown-link.blt")["unknown-link"]
    if unknown_time != 0 or unknown_flags & 0x4000:
        raise RuntimeError("pre-epoch symlink timestamp was not encoded as unknown")
    unknown_output = work / "unknown-link-output"
    unknown_output.mkdir()
    marker = work / "link-time-marker-unknown"
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(fail_link_time)
    env["BP_TEST_LINK_TIME_MARKER"] = str(marker)
    run([str(binary), "x", "-o", str(unknown_output),
         str(work / "unknown-link.blt")], env=env)
    if marker.exists() or not (unknown_output / "unknown-link").is_symlink():
        raise RuntimeError("unknown symlink time was applied or link was not published")

    fallback_binary = work / "boltpack-haiku-no-link-time"
    fallback_compile = host_compile.copy()
    fallback_compile.insert(1, "-DBP_HAIKU_NO_SYMLINK_TIMES")
    fallback_compile[-1] = str(fallback_binary)
    run(fallback_compile)
    fallback_output = work / "link-time-unavailable"
    fallback_output.mkdir()
    fallback_target = fallback_output / "source.bin"
    fallback_target.write_bytes(b"fallback target unchanged\n")
    os.utime(fallback_target, (1_500_000_000, 1_500_000_000))
    run([str(fallback_binary), "x", "-o", str(fallback_output),
         str(work / "safe-link.blt")], expected=1)
    if not (fallback_output / "safe-link").is_symlink() or \
            (fallback_output / "safe-link").readlink() != Path("source.bin") or \
            int(fallback_target.stat().st_mtime) != 1_500_000_000 or \
            fallback_target.read_bytes() != b"fallback target unchanged\n" or \
            list(fallback_output.glob(".boltpack-*")):
        raise RuntimeError("unavailable symlink timestamp handling modified target or lost link")
    fallback_unknown = work / "link-time-unavailable-unknown"
    fallback_unknown.mkdir()
    run([str(fallback_binary), "x", "-o", str(fallback_unknown),
         str(work / "unknown-link.blt")])
    if not (fallback_unknown / "unknown-link").is_symlink():
        raise RuntimeError("unknown symlink time was not accepted without no-follow timestamp API")

    # A valid archive timestamp outside the host's signed range must report
    # partial, preserve link data and never touch the target.
    huge_link = bytearray((work / "safe-link.blt").read_bytes())
    huge_table, huge_table_size = struct.unpack_from("<QQ", huge_link, len(huge_link) - 20)
    huge_name = struct.unpack_from("<H", huge_link, huge_table + 16)[0]
    huge_fields = huge_table + 18 + huge_name
    huge_inline = 32 + 4 + huge_name
    struct.pack_into("<Q", huge_link, huge_fields + 18, 1 << 63)
    struct.pack_into("<Q", huge_link, huge_inline + 28, 1 << 63)
    struct.pack_into("<I", huge_link, 24,
                     zlib.crc32(huge_link[huge_table:huge_table + huge_table_size]))
    struct.pack_into("<I", huge_link, 28, zlib.crc32(huge_link[:28]))
    huge_archive = work / "huge-link-time.blt"
    huge_archive.write_bytes(huge_link)
    huge_output = work / "huge-link-time-output"
    huge_output.mkdir()
    huge_target = huge_output / "source.bin"
    huge_target.write_bytes(b"huge timestamp target\n")
    os.utime(huge_target, (1_500_000_000, 1_500_000_000))
    run([str(binary), "x", "-o", str(huge_output), str(huge_archive)], expected=1)
    if not (huge_output / "safe-link").is_symlink() or \
            (huge_output / "safe-link").readlink() != Path("source.bin") or \
            huge_target.read_bytes() != b"huge timestamp target\n" or \
            int(huge_target.stat().st_mtime) != 1_500_000_000:
        raise RuntimeError("unrepresentable symlink timestamp modified the target or lost link")

    # A failed read or close while interpreting the verified symlink stage
    # must never publish the temporary regular file over an existing target.
    link_fault_source = work / "fail-link-read.c"
    link_fault_source.write_text(
        "#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <errno.h>\n"
        "#include <fcntl.h>\n#include <stdio.h>\n#include <stdlib.h>\n"
        "#include <string.h>\n#include <unistd.h>\n"
        "static int link_stage(FILE *f) { char ref[64],p[4096]; ssize_t n; int fd=fileno(f);\n"
        "  int flags=fcntl(fd,F_GETFL); if(flags<0 || (flags&O_ACCMODE)!=O_RDONLY) return 0;\n"
        "  snprintf(ref,sizeof(ref),\"/proc/self/fd/%d\",fd); n=readlink(ref,p,sizeof(p)-1);\n"
        "  if(n<0) return 0; p[n]=0; return strstr(p,\"/.boltpack-\")!=NULL; }\n"
        "static int eof_error;\n"
        "size_t fread(void *p,size_t s,size_t n,FILE *f) {\n"
        "  static size_t (*real_fread)(void *,size_t,size_t,FILE *);\n"
        "  if(getenv(\"BP_TEST_LINK_SHORT\") && link_stage(f)) return 0;\n"
        "  if(!real_fread) real_fread=dlsym(RTLD_NEXT,\"fread\"); return real_fread(p,s,n,f); }\n"
        "int fgetc(FILE *f) { static int (*real_fgetc)(FILE *);\n"
        "  if(getenv(\"BP_TEST_LINK_TRAILING\") && link_stage(f)) return 'X';\n"
        "  if(getenv(\"BP_TEST_LINK_EOFERROR\") && link_stage(f)) {eof_error=1; return EOF;}\n"
        "  if(!real_fgetc) real_fgetc=dlsym(RTLD_NEXT,\"fgetc\"); return real_fgetc(f); }\n"
        "int ferror(FILE *f) { static int (*real_ferror)(FILE *);\n"
        "  if(eof_error && link_stage(f)) return 1;\n"
        "  if(!real_ferror) real_ferror=dlsym(RTLD_NEXT,\"ferror\"); return real_ferror(f); }\n"
        "int fclose(FILE *f) { static int (*real_fclose)(FILE *); int fail,rc;\n"
        "  fail=getenv(\"BP_TEST_LINK_CLOSE\") && link_stage(f);\n"
        "  if(!real_fclose) real_fclose=dlsym(RTLD_NEXT,\"fclose\"); rc=real_fclose(f);\n"
        "  if(fail) {errno=EIO; return EOF;} return rc; }\n", encoding="ascii")
    link_fault = work / "fail-link-read.so"
    run([args.cc, "-shared", "-fPIC", str(link_fault_source), "-ldl", "-o", str(link_fault)])
    for fault in ("SHORT", "TRAILING", "EOFERROR", "CLOSE"):
        for existing in (False, True):
            fault_output = work / ("link-failure-" + fault.lower() + ("-existing" if existing else "-new"))
            fault_output.mkdir()
            fault_target = fault_output / "safe-link"
            sentinel = b"preserve original file\n"
            if existing:
                fault_target.write_bytes(sentinel)
            env = os.environ.copy()
            env["LD_PRELOAD"] = str(link_fault)
            env["BP_TEST_LINK_" + fault] = "1"
            run([str(binary), "x", "-f", "-o", str(fault_output),
                 str(work / "safe-link.blt")], env=env, expected=1)
            if ((existing and (fault_target.is_symlink() or fault_target.read_bytes() != sentinel)) or
                    (not existing and (fault_target.exists() or fault_target.is_symlink())) or
                    list(fault_output.glob(".boltpack-*"))):
                raise RuntimeError("symlink %s failure published or left a stage" % fault.lower())

    # Construct a structurally valid link whose target exceeds the Haiku
    # frontend buffer, without relying on the host filesystem's link limit.
    oversized = bytearray((work / "safe-link.blt").read_bytes())
    table_offset, table_size = struct.unpack_from("<QQ", oversized, len(oversized) - 20)
    name_length = struct.unpack_from("<H", oversized, 34)[0]
    inline = 36 + name_length
    header_size = struct.unpack_from("<H", oversized, 32)[0]
    payload_offset = 32 + header_size
    target_bytes = b"a" * 4096
    oversized[payload_offset:table_offset] = target_bytes
    table_offset = payload_offset + len(target_bytes)
    fields = table_offset + 16 + 2 + name_length
    checksum = zlib.crc32(target_bytes)
    struct.pack_into("<QQI", oversized, inline, len(target_bytes), len(target_bytes), checksum)
    struct.pack_into("<QQ", oversized, fields, len(target_bytes), len(target_bytes))
    struct.pack_into("<I", oversized, fields + 37, checksum)
    struct.pack_into("<Q", oversized, len(oversized) - 20, table_offset)
    struct.pack_into("<Q", oversized, 16, len(oversized))
    struct.pack_into("<I", oversized, 24, zlib.crc32(oversized[table_offset:table_offset + table_size]))
    struct.pack_into("<I", oversized, 28, zlib.crc32(oversized[:28]))
    oversized_archive = work / "oversized-link.blt"
    oversized_archive.write_bytes(oversized)
    run([str(args.linux_boltpack.resolve()), "t", str(oversized_archive)])
    oversized_output = work / "oversized-link-output"
    oversized_output.mkdir()
    oversized_target = oversized_output / "safe-link"
    oversized_target.write_bytes(b"original survives oversized link\n")
    run([str(binary), "x", "-f", "-o", str(oversized_output),
         str(oversized_archive)], expected=1)
    if oversized_target.is_symlink() or \
            oversized_target.read_bytes() != b"original survives oversized link\n" or \
            list(oversized_output.glob(".boltpack-*")):
        raise RuntimeError("oversized symlink target changed the destination or left a stage")

    (work / "unsafe-link").symlink_to("../../outside")
    run([str(binary), "c", "-0", "unsafe-link.blt", "unsafe-link"], cwd=work)
    unsafe_out = work / "unsafe-output"
    unsafe_out.mkdir()
    run([str(binary), "x", "-o", str(unsafe_out), str(work / "unsafe-link.blt")],
        expected=1)
    if (unsafe_out / "unsafe-link").exists() or \
            (unsafe_out / "unsafe-link").is_symlink() or \
            list(unsafe_out.glob(".boltpack-*")):
        raise RuntimeError("unsafe symlink target was published or left staged")

    nested_source = work / "tree" / "inner"
    nested_source.mkdir(parents=True)
    (nested_source / "file.bin").write_bytes(b"must remain inside extraction root")
    run([str(binary), "c", "-0", "tree.blt", "tree"], cwd=work)
    outside = work / "outside"
    outside.mkdir()
    tree_out = work / "tree-output"
    tree_out.mkdir()
    (tree_out / "tree").symlink_to(outside, target_is_directory=True)
    run([str(binary), "x", "-o", str(tree_out), str(work / "tree.blt")],
        expected=1)
    if (outside / "inner" / "file.bin").exists() or \
            list(tree_out.glob(".boltpack-*")):
        raise RuntimeError("extraction followed a symlinked destination parent")

    if os.environ.get("BP_TEST_HAIKU_PARENT_RACE") == "1":
        # Swap a checked destination parent immediately after creating its stage.
        # A decoy stage in the outside directory makes path-based publication
        # overwrite an outside sentinel deterministically.
        race_source = work / "swap-parent-after-stage.c"
        race_source.write_text(
            "#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <fcntl.h>\n"
            "#include <stdarg.h>\n#include <stdio.h>\n#include <stdlib.h>\n"
            "#include <string.h>\n#include <unistd.h>\n"
            "static int done;\n"
            "static void swap(const char *leaf) {char decoy[4096]; int fd;\n"
            " const char *parent=getenv(\"HAIKU_RACE_PARENT\"), *park=getenv(\"HAIKU_RACE_PARK\"),\n"
            " *outside=getenv(\"HAIKU_RACE_OUTSIDE\"), *marker=getenv(\"HAIKU_RACE_MARKER\");\n"
            " if(done || !parent || !park || !outside || !marker) return; done=1;\n"
            " if(rename(parent,park) || symlink(outside,parent)) return;\n"
            " snprintf(decoy,sizeof(decoy),\"%s/%s\",outside,leaf);\n"
            " fd=open(decoy,O_WRONLY|O_CREAT|O_EXCL,0600); if(fd<0) return;\n"
            " (void)write(fd,\"decoy\",5); close(fd);\n"
            " fd=open(marker,O_WRONLY|O_CREAT|O_EXCL,0600); if(fd>=0) close(fd); }\n"
            "int mkstemp(char *p) {static int (*real_fn)(char *); int fd; const char *leaf;\n"
            " if(!real_fn) real_fn=dlsym(RTLD_NEXT,\"mkstemp\"); fd=real_fn(p);\n"
            " leaf=strrchr(p,'/'); if(fd>=0 && leaf && strstr(p,\"/inner/.boltpack-\")) swap(leaf+1);\n"
            " return fd;}\n"
            "int openat(int d,const char *p,int f,...) {static int (*real_fn)(int,const char *,int,...);\n"
            " mode_t m=0; int fd; va_list ap; if(!real_fn) real_fn=dlsym(RTLD_NEXT,\"openat\");\n"
            " if(f&O_CREAT) {va_start(ap,f); m=(mode_t)va_arg(ap,int); va_end(ap); fd=real_fn(d,p,f,m);}\n"
            " else fd=real_fn(d,p,f); if(fd>=0 && (f&O_CREAT) && !strncmp(p,\".boltpack-\",10)) swap(p);\n"
            " return fd;}\n", encoding="ascii")
        race_library = work / "swap-parent-after-stage.so"
        run([args.cc, "-shared", "-fPIC", str(race_source), "-ldl", "-o", str(race_library)])
        race_output = work / "parent-race-output"
        race_output.mkdir()
        race_outside = work / "parent-race-outside"
        race_outside.mkdir()
        outside_file = race_outside / "file.bin"
        outside_file.write_bytes(b"outside must not change\n")
        race_marker = work / "parent-race-triggered"
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(race_library),
                   HAIKU_RACE_PARENT=str(race_output / "tree/inner"),
                   HAIKU_RACE_PARK=str(race_output / "tree/inner.moved"),
                   HAIKU_RACE_OUTSIDE=str(race_outside),
                   HAIKU_RACE_MARKER=str(race_marker))
        result = subprocess.run([str(binary), "x", "-f", "-o", str(race_output),
                                 str(work / "tree.blt")], cwd=work, env=env,
                                capture_output=True)
        if not race_marker.exists():
            raise RuntimeError("parent substitution interposer did not trigger")
        if result.returncode == 0 or outside_file.read_bytes() != b"outside must not change\n":
            raise RuntimeError("parent substitution escaped or was not reported")
        if list((race_output / "tree/inner.moved").glob(".boltpack-*")):
            raise RuntimeError("parent substitution left a stage in the held directory")

    # The output root fits below PATH_MAX, but joining the archived member
    # would exceed the frontend's path buffer. Preflight must reject it before
    # mkdirs creates any part of the selected destination tree.
    long_output = work
    current_length = len(str(long_output))
    while current_length + 241 < 4078:
        long_output = long_output / ("p" * 240)
        current_length += 241
    final_component = 4078 - current_length - 1
    if final_component <= 0 or final_component > 252:
        raise RuntimeError("could not construct the long-path fixture")
    long_output = long_output / ("q" * final_component)
    member = "tree/inner/file.bin"
    if len(str(long_output)) >= 4096 or \
            len(str(long_output)) + 1 + len(member) < 4096:
        raise RuntimeError("long-path fixture does not cross the path bound")
    run([str(binary), "x", "-o", str(long_output), str(work / "tree.blt")],
        expected=1)
    if long_output.exists():
        raise RuntimeError("overlong joined path created the destination tree")

    run([str(binary), "c", "--interchange-profile-1", "profile.blt", "source.bin"],
        cwd=work)
    info = run([str(args.linux_boltpack.resolve()), "i", str(work / "profile.blt")])
    if b"BoltLite" not in info:
        raise RuntimeError("Profile 1 create did not select BoltLite")
    run([str(binary), "c", "-F", "--interchange-profile-1", "bad-method.blt",
         "source.bin"], cwd=work, expected=1)
    run([str(binary), "c", "-b", "32k", "--interchange-profile-1",
         "bad-block.blt", "source.bin"], cwd=work, expected=1)
    if (work / "bad-method.blt").exists() or (work / "bad-block.blt").exists():
        raise RuntimeError("incompatible Profile 1 options created archives")
    run([str(binary), "a", "--interchange-profile-1", "profile.blt", "extra.bin"],
        cwd=work)
    run([str(args.linux_boltpack.resolve()), "t", str(work / "profile.blt")])
    profile_bytes = (work / "profile.blt").read_bytes()
    run([str(binary), "a", "-F", "--interchange-profile-1", "profile.blt",
         "extra.bin"], cwd=work, expected=1)
    if (work / "profile.blt").read_bytes() != profile_bytes:
        raise RuntimeError("incompatible Profile 1 append changed archive bytes")

    run([str(binary), "c", "-F", "nonprofile.blt", "source.bin"], cwd=work)
    nonprofile_bytes = (work / "nonprofile.blt").read_bytes()
    run([str(binary), "a", "--interchange-profile-1", "nonprofile.blt", "extra.bin"],
        cwd=work, expected=1)
    if (work / "nonprofile.blt").read_bytes() != nonprofile_bytes:
        raise RuntimeError("Profile 1 refusal changed an incompatible archive")
    print("PASS: Haiku host-shim extraction lifecycle and Profile 1; evidence:", work)


if __name__ == "__main__":
    main()
