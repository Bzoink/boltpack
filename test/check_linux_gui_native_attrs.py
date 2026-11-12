#!/usr/bin/env python3
"""Round-trip production Linux FLTK workers with CLI readers and writers."""
import argparse
import os
from pathlib import Path
import stat
import struct
import subprocess
import tempfile


def run(*args, expected=0, cwd=None):
    r = subprocess.run([str(a) for a in args], text=True, capture_output=True, timeout=30, cwd=cwd)
    assert r.returncode == expected, (args, r.returncode, r.stdout, r.stderr)
    return r


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--fixture', required=True, type=Path)
    p.add_argument('--cli', required=True, type=Path)
    p.add_argument('--attr-writer', required=True, type=Path)
    args = p.parse_args()
    fixture, cli, writer = (v.resolve() for v in (args.fixture, args.cli, args.attr_writer))
    with tempfile.TemporaryDirectory(prefix='bp-gui-native-') as tmp:
        root = Path(tmp)
        src = root / 'src'
        (src / 'dir/empty').mkdir(parents=True)
        f = src / 'dir/run'
        f.write_bytes(b'native metadata\n' * 20)
        f.chmod(0o751)
        os.link(f, src / 'alias')
        (src / 'link').symlink_to('dir/run')
        (src / 'dir-link').symlink_to('dir')
        # A link outside the source is recorded, but refused on extraction.
        (src / 'unsafe').symlink_to('../../outside')
        os.mkfifo(src / 'pipe')
        os.setxattr(f, 'user.binary', b'\x00hello\xff')
        os.setxattr(src / 'dir', 'user.folder', b'dir metadata')
        readonly = src / 'readonly'
        readonly.write_bytes(b'read only')
        os.setxattr(readonly, 'user.ro', b'preserved')
        readonly.chmod(0o444)
        def acl(entries):
            return struct.pack('<I', 2) + b''.join(struct.pack('<HHI', *e) for e in entries)
        # The sandbox maps only the current UID; a named entry for it is valid.
        other = os.getuid()
        access = acl([(1, 7, 0xffffffff), (2, 4, other), (4, 5, 0xffffffff),
                      (16, 5, 0xffffffff), (32, 1, 0xffffffff)])
        default = acl([(1, 7, 0xffffffff), (2, 5, other), (4, 5, 0xffffffff),
                       (16, 5, 0xffffffff), (32, 0, 0xffffffff)])
        os.setxattr(f, 'system.posix_acl_access', access)
        os.setxattr(src / 'dir', 'system.posix_acl_default', default)
        (src / 'dir').chmod(0o1750)
        for path in (f, src / 'dir', src / 'link', readonly):
            os.utime(path, (1700000000, 1700000000), follow_symlinks=False)
        archive = root / 'gui.bp'
        r = run(fixture, 'c', archive, src)
        assert 'special file not archived' in r.stderr and 'preservation warnings: 1' in r.stderr
        listing = run(cli, 'l', archive).stdout
        assert 'ATTR' in listing and 'LINK' in listing and 'pipe' not in listing
        run(cli, 't', archive)
        def verify(out):
            tree = out / 'src'
            assert (tree / 'dir/run').read_bytes() == f.read_bytes()
            assert stat.S_IMODE((tree / 'dir/run').stat().st_mode) == stat.S_IMODE(f.stat().st_mode)
            assert stat.S_IMODE((tree / 'dir').stat().st_mode) == 0o1750
            assert (tree / 'dir/run').stat().st_ino == (tree / 'alias').stat().st_ino
            assert (tree / 'dir/empty').is_dir()
            assert os.readlink(tree / 'link') == 'dir/run'
            assert os.readlink(tree / 'dir-link') == 'dir'
            assert not os.path.lexists(tree / 'unsafe')
            for rel in ('dir/run', 'dir', 'readonly'):
                for name in os.listxattr(src / rel):
                    if name.startswith(('user.', 'system.posix_acl')):
                        assert os.getxattr(tree / rel, name) == os.getxattr(src / rel, name), (rel, name)
                assert int((tree / rel).stat().st_mtime) == 1700000000
            assert int((tree / 'link').lstat().st_mtime) == 1700000000
        for reader in (fixture, cli):
            out = root / ('gui-out' if reader == fixture else 'cli-out')
            if reader == fixture:
                run(reader, 'x', archive, out, expected=1)
            else:
                run(reader, 'x', '-o', out, archive, expected=1)
            verify(out)
        # CLI -> GUI, including the same hard links/ACLs.
        cli_archive = root / 'cli.bp'
        run(cli, 'c', '-0', cli_archive, 'src', cwd=root)
        out = root / 'cli-to-gui'
        run(fixture, 'x', cli_archive, out, expected=1)
        verify(out)
        # Selected files keep attributes and never join a pre-existing target.
        out = root / 'selected'
        run(fixture, 'x', archive, out, 'src/readonly')
        assert os.getxattr(out / 'src/readonly', 'user.ro') == b'preserved'
        assert not (out / 'src/dir').exists()
        out = root / 'flat'
        # Include both names of the hard-linked file and an unrelated read-only file.
        run(fixture, 'x', archive, out, 'flat', 'src/alias', 'src/dir/run', 'src/readonly')
        assert (out / 'alias').stat().st_ino == (out / 'run').stat().st_ino
        assert os.getxattr(out / 'run', 'user.binary') == b'\x00hello\xff'
        assert os.getxattr(out / 'readonly', 'user.ro') == b'preserved'
        out = root / 'only-hardlink'
        later = max(('src/alias', 'src/dir/run'), key=listing.index)
        run(fixture, 'x', archive, out, later, expected=1)
        assert (out / later).read_bytes() == f.read_bytes()
        # Interchange archives omit native records.
        profile = root / 'profile.bp'
        run(fixture, 'profile', profile, src)
        assert 'ATTR' not in run(cli, 'l', profile).stdout
        # Synthetic capability entry tests both refusal and explicit opt-in,
        # without requiring root or installing a capability.
        from check_linux_native_attrs import container
        payload = root / 'payload'
        payload.write_bytes(container([(b'security.capability', b'\x01\x00\x00\x02')]))
        security_archive = root / 'security.bp'
        run(writer, security_archive, 'prog', payload, 'file', '0xFD')
        r = run(fixture, 'x', security_archive, root / 'security-off', expected=1)
        assert '--security-attrs' in r.stderr
        r = run(fixture, 'x', security_archive, root / 'security-on', 'security', expected=1)
        assert 'errno' in r.stderr and 'security.capability' in r.stderr
        # A hostile hard-link record must not replace the extracted file with an existing one.
        payload.write_bytes(container([(b'boltpack.hardlink', b'preexisting')]))
        bad = root / 'bad-hardlink.bp'
        run(writer, bad, 'victim', payload, 'file', '0xFD')
        out = root / 'hostile'
        out.mkdir()
        (out / 'preexisting').write_bytes(b'data\n')
        before = (out / 'preexisting').stat().st_ino
        run(fixture, 'x', bad, out, expected=1)
        assert (out / 'preexisting').stat().st_ino == before
        assert (out / 'victim').stat().st_ino != before
    print('PASS: Linux GUI/CLI modes, times, symlinks, hard links, xattrs, ACLs, selection, profiles and security policy')


if __name__ == '__main__':
    main()
