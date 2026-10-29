#!/usr/bin/env python3
"""Host-shim regression for Haiku staged archive and extraction lifecycle."""
import argparse
import os
from pathlib import Path
import subprocess
import stat
import struct
import tempfile

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
    run([args.cc, "-ansi", "-pedantic", "-Wall", "-Wextra",
         "-Wno-unused-function", "-Wno-overlength-strings", "-D_DEFAULT_SOURCE",
         "-I" + str(stub), "-I" + str(ROOT / "src/core"),
         str(ROOT / "src/core/archiver.c"),
         str(ROOT / "src/core/compress.c"),
         str(ROOT / "src/core/boltlite.c"),
         str(ROOT / "src/core/fluff.c"),
         str(ROOT / "src/core/bp_codepage.c"),
         str(ROOT / "src/frontend/haiku/main.c"),
         str(ROOT / "src/core/boltlz.c"), str(stub_impl), "-lm", "-o", str(binary)])
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
    run([str(binary), "c", "-0", "safe-link.blt", "safe-link"], cwd=work)
    link_out = work / "link-output"
    link_out.mkdir()
    run([str(binary), "x", "-o", str(link_out), str(work / "safe-link.blt")])
    if not (link_out / "safe-link").is_symlink() or \
            (link_out / "safe-link").readlink() != Path("source.bin"):
        raise RuntimeError("verified safe symlink was not published correctly")
    if list(link_out.glob(".boltpack-*")):
        raise RuntimeError("successful symlink extraction left a stage")

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
