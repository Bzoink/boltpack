#!/usr/bin/env python3
"""Acceptance gate: restored applications must run and failures must preserve data."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SPEC = Path(__file__).with_name('reference') / 'usable-roundtrip.json'


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


class Evidence:
    def __init__(self, directory):
        self.directory = directory
        self.commands = []
        self.checks = []

    def run(self, argv, cwd=None, expected=(0,), env=None, timeout=180):
        argv = list(map(str, argv))
        path = self.directory / ('command-%03d.log' % len(self.commands))
        record = {'argv': argv, 'cwd': str(cwd or ROOT), 'log': path.name}
        self.commands.append(record)
        with path.open('wb') as log:
            child = subprocess.Popen(argv, cwd=cwd or ROOT, env=env, stdout=log,
                                     stderr=subprocess.STDOUT, start_new_session=True)
            try:
                rc = child.wait(timeout=timeout)
            except BaseException:
                os.killpg(child.pid, signal.SIGTERM)
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
                raise
        record['returncode'] = rc
        record['expected_returncodes'] = list(expected)
        require(rc in expected, 'Command failed (%d); inspect %s' % (rc, path))
        return path.read_bytes()

    def check(self, name, ok, detail=None, expected_partial=False):
        result = {'name': name, 'status': ('expected_partial' if expected_partial else 'pass') if ok else 'fail'}
        if detail is not None:
            result['detail'] = detail
        self.checks.append(result)
        require(ok, name)


def linux_case(args, evidence, spec):
    out = evidence.directory
    src = out / 'APP'
    (src / 'EMPTY').mkdir(parents=True)
    payload = spec['data'].encode('ascii')
    (src / 'DATA.TXT').write_bytes(payload)
    (src / 'binary.dat').write_bytes(bytes(range(256)) * 32)
    (src / 'café.txt').write_text('Exact UTF-8 name\n', encoding='utf-8')
    script = src / 'START'
    script.write_text('#!/bin/sh\nset -eu\n'
                      'test "$(cat "$(dirname "$0")/DATA.TXT")" = '
                      '"BoltPack usable application data"\n'
                      'printf "BOLTPACK_USABLE_SCRIPT_OK\\n"\n', encoding='ascii')
    script.chmod(0o751)
    os.link(script, src / 'HARDLINK')
    (src / 'LINK').symlink_to('START')
    evidence.run([args.cc, '-O2', ROOT / 'test/usable/probe.c', '-o', src / 'PROBE'])
    os.setxattr(script, 'user.boltpack', b'\x00native metadata\xff')
    os.setxattr(src / 'EMPTY', 'user.boltpack', b'empty directory metadata')
    # A named entry for this UID is valid even in a sandbox with one mapped UID.
    acl = struct.pack('<I', 2) + b''.join(struct.pack('<HHI', *entry) for entry in
        [(1, 7, 0xffffffff), (2, 5, os.getuid()), (4, 5, 0xffffffff),
         (16, 5, 0xffffffff), (32, 1, 0xffffffff)])
    os.setxattr(script, 'system.posix_acl_access', acl)
    for path in list(src.iterdir()) + [src]:
        os.utime(path, (1700000000, 1700000000), follow_symlinks=False)
    for codec in ('-0', '-1', '-2', '-F'):
        archive = out / ('linux%s.bolt' % codec)
        restored = out / ('restore%s' % codec)
        evidence.run([args.linux_cli, 'c', codec, archive, 'APP'], cwd=out)
        evidence.run([args.linux_cli, 't', archive])
        evidence.run([args.linux_cli, 'x', '-o', restored, archive])
        tree = restored / 'APP'
        for name in ('DATA.TXT', 'binary.dat', 'café.txt', 'START', 'PROBE'):
            evidence.check(codec + ': exact bytes/name: ' + name,
                           (tree / name).read_bytes() == (src / name).read_bytes())
            evidence.check(codec + ': modification time: ' + name,
                           int((tree / name).stat().st_mtime) == 1700000000)
            evidence.check(codec + ': permission mode: ' + name,
                           stat.S_IMODE((tree / name).stat().st_mode) ==
                           stat.S_IMODE((src / name).stat().st_mode))
        evidence.check(codec + ': directory metadata',
            (tree / 'EMPTY').is_dir() and int(tree.stat().st_mtime) == 1700000000 and
            os.getxattr(tree / 'EMPTY', 'user.boltpack') == b'empty directory metadata')
        evidence.check(codec + ': hard-link identity',
                       (tree / 'START').stat().st_ino == (tree / 'HARDLINK').stat().st_ino)
        evidence.check(codec + ': symbolic-link identity', os.readlink(tree / 'LINK') == 'START')
        evidence.check(codec + ': binary extended attribute',
                       os.getxattr(tree / 'START', 'user.boltpack') == b'\x00native metadata\xff')
        evidence.check(codec + ': POSIX ACL',
                       os.getxattr(tree / 'START', 'system.posix_acl_access') == acl)
        evidence.check(codec + ': restored script runs with restored data',
                       evidence.run([tree / 'LINK']) == b'BOLTPACK_USABLE_SCRIPT_OK\n')
        evidence.check(codec + ': restored executable runs with restored data',
                       evidence.run([tree / 'PROBE', tree / 'DATA.TXT']).strip() ==
                       spec['program_marker'].encode('ascii'))
    # Corrupt stored entry data while retaining the original recorded checksums.
    keep = out / 'KEEP.TXT'
    keep.write_bytes(payload)
    good = out / 'good.bolt'
    evidence.run([args.linux_cli, 'c', '-0', good, 'KEEP.TXT'], cwd=out)
    bad = out / 'corrupt.bolt'
    raw = bytearray(good.read_bytes())
    location = raw.find(payload)
    require(location >= 0, 'Stored fixture payload was not found')
    raw[location] ^= 1
    bad.write_bytes(raw)
    target = out / 'failure'
    target.mkdir()
    sentinel = target / 'KEEP.TXT'
    sentinel.write_bytes(b'existing destination must survive\n')
    os.setxattr(sentinel, 'user.keep', b'untouched')
    before = digest(sentinel)
    evidence.run([args.linux_cli, 'x', '-f', '-o', target, bad], expected=(1,))
    evidence.check('checksum failure preserves existing bytes and metadata',
                   digest(sentinel) == before and os.getxattr(sentinel, 'user.keep') == b'untouched')
    evidence.check('checksum failure cleans temporary output',
                   sorted(p.name for p in target.iterdir()) == ['KEEP.TXT'])
    previous = good.read_bytes()
    evidence.run([args.linux_cli, 'c', good, 'KEEP.TXT', 'MISSING.TXT'], cwd=out, expected=(1,))
    evidence.check('failed archive replacement preserves previous archive', good.read_bytes() == previous)
    # Unsupported absolute links must be reported while ordinary files survive.
    (src / 'UNSAFE').symlink_to('/outside')
    partial = out / 'partial.bolt'
    evidence.run([args.linux_cli, 'c', '-0', partial, 'APP'], cwd=out)
    result = evidence.run([args.linux_cli, 'x', '-o', out / 'partial', partial], expected=(1,))
    evidence.check('expected partial: unsafe link refused with visible diagnostic',
                   b'unsafe target' in result and not os.path.lexists(out / 'partial/APP/UNSAFE'), expected_partial=True)
    evidence.check('expected partial: usable application retained',
                   evidence.run([out / 'partial/APP/START']) == b'BOLTPACK_USABLE_SCRIPT_OK\n')
    return ['Absolute/escaping links are intentionally refused (partial).',
            'Ownership, set-ID bits, special files and sparse layout are outside this gate.']


def dos_case(args, evidence, spec):
    out = evidence.directory
    image = out / 'dos-fat12.img'
    # DOS .COM: print a marker through int 21h/AH=09, then exit 0 (AH=4C).
    marker = (spec['dos_marker'] + '\r\n').encode('ascii')
    program = bytes.fromhex('ba0c01b409cd21b8004ccd21') + marker + b'$'
    (out / 'RUN.COM').write_bytes(program)
    (out / 'DATA.TXT').write_bytes(spec['data'].encode('ascii'))
    (out / 'KEEP.TXT').write_bytes(spec['data'].encode('ascii'))
    evidence.run([args.linux_cli, 'c', '--target-profile=dos16-cp850-mini', '-0',
                  'GOOD.BLT', 'KEEP.TXT'], cwd=out)
    bad = bytearray((out / 'GOOD.BLT').read_bytes())
    offset = bad.find(spec['data'].encode('ascii'))
    require(offset >= 0, 'Stored DOS corruption fixture payload not found')
    bad[offset] ^= 1
    (out / 'BAD.BLT').write_bytes(bad)
    (out / 'SENTINEL').write_bytes(b'existing DOS destination survives\r\n')
    evidence.run(['mformat', '-i', image, '-C', '-f', '720', '::'])
    for name in ('OUT', 'FAIL'):
        evidence.run(['mmd', '-i', image, '::/' + name])
    for source, dest in ((args.dos_cli, 'BPMINI.EXE'), (out / 'RUN.COM', 'RUN.COM'),
                         (out / 'DATA.TXT', 'DATA.TXT'), (out / 'BAD.BLT', 'BAD.BLT'),
                         (out / 'SENTINEL', 'FAIL/KEEP.TXT')):
        evidence.run(['mcopy', '-m', '-i', image, source, '::/' + dest])
    commands = ['@echo off', 'set BP_TZ_OFFSET=0', 'md SRC', 'md SRC\\EMPTY',
                'copy /b RUN.COM SRC\\RUN.COM >NUL', 'copy /b DATA.TXT SRC\\DATA.TXT >NUL',
                'attrib +r SRC\\RUN.COM', 'attrib +h SRC\\RUN.COM',
                'BPMINI c USE.BLT SRC >CREATE.LOG',
                'if errorlevel 1 goto failure',
                'BPMINI t USE.BLT >TEST.LOG',
                'if errorlevel 1 goto failure',
                'BPMINI x USE.BLT OUT >EXTRACT.LOG',
                'if errorlevel 1 echo PARTIAL >EXTRACT.RC',
                'OUT\\SRC\\RUN.COM >USABLE.TXT',
                'if errorlevel 1 goto failure',
                'BPMINI x BAD.BLT FAIL >FAILURE.LOG',
                'if errorlevel 1 echo REFUSED >FAIL.RC',
                'echo FINISHED >DONE.TXT', 'goto done', ':failure',
                'echo FAILED >FAILED.TXT', ':done', 'exit']
    (out / 'CHECK.BAT').write_bytes(('\r\n'.join(commands) + '\r\n').encode('ascii'))
    evidence.run(['mcopy', '-i', image, out / 'CHECK.BAT', '::/CHECK.BAT'])
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    evidence.run([args.dosbox_x, '-nopromptfolder', '-nogui', '-fastlaunch',
                  '-c', 'imgmount a %s -t floppy' % image, '-c', 'a:', '-c', 'CHECK.BAT',
                  '-c', 'exit'], env=env, timeout=120)
    for guest, name in (('DONE.TXT', 'done.txt'), ('USABLE.TXT', 'usable.txt'),
                         ('FAIL.RC', 'failure.rc'), ('FAIL/KEEP.TXT', 'sentinel.txt'),
                         ('OUT/SRC/RUN.COM', 'restored.com'), ('OUT/SRC/DATA.TXT', 'restored.txt'),
                         ('EXTRACT.LOG', 'extract.log'), ('USE.BLT', 'dos-created.blt')):
        evidence.run(['mcopy', '-i', image, '::/' + guest, out / name])
    listing = evidence.run(['mdir', '-b', '-i', image, '::/'])
    evidence.check('guest operations completed without create/test/program failure',
                   b'FAILED.TXT' not in listing.upper())
    evidence.check('restored DOS executable runs', (out / 'usable.txt').read_bytes().strip() == marker.strip())
    evidence.check('DOS application payload and executable bytes',
                   (out / 'restored.com').read_bytes() == program and
                   (out / 'restored.txt').read_bytes() == (out / 'DATA.TXT').read_bytes())
    evidence.run(['mdir', '-i', image, '::/OUT/SRC/EMPTY'])
    attrs = evidence.run(['mattrib', '-i', image, '::/OUT/SRC/RUN.COM']).split(b'::', 1)[0]
    evidence.check('DOS executable read-only/hidden attributes', b'R' in attrs and b'H' in attrs)
    times = []
    for folder in ('::/SRC', '::/OUT/SRC'):
        listing = evidence.run(['mdir', '-a', '-i', image, folder]).decode('ascii')
        fields = next((line.split() for line in listing.splitlines()
                       if line.split()[:2] == ['RUN', 'COM']), [])
        require(len(fields) >= 5, 'DOS executable timestamp not found')
        times.append(fields[-2:])
    evidence.check('DOS executable modification time', times[0] == times[1])
    evidence.check('expected partial: DOS directory times',
                   b'directory' in (out / 'extract.log').read_bytes().lower() or
                   b'PARTIAL' in evidence.run(['mtype', '-i', image, '::/EXTRACT.RC']),
                   detail='DOS16 cannot restore directory times; application still runs.',
                   expected_partial=True)
    evidence.check('failed DOS extraction preserves existing destination',
                   (out / 'sentinel.txt').read_bytes() == (out / 'SENTINEL').read_bytes() and
                   b'REFUSED' in (out / 'failure.rc').read_bytes())
    evidence.run([args.linux_cli, 't', out / 'dos-created.blt'])
    evidence.run([args.linux_cli, 'x', '-o', out / 'linux-out', out / 'dos-created.blt'], expected=(1,))
    evidence.check('DOS archive independently read by Linux',
                   (out / 'linux-out/SRC/RUN.COM').read_bytes() == program)
    return ['DOS16 mini, DOSBox-X, disposable FAT12; not DJGPP/full DOS or physical hardware.',
            'DOS directory timestamp restoration may report partial; Linux reports foreign FAT metadata.',
            'The COM marker proves execution; data bytes are checked separately.']


def prerequisites(args, name):
    tools = {'linux': [args.cc], 'dos': [args.dosbox_x, 'mformat', 'mcopy', 'mmd', 'mdir', 'mattrib', 'mtype'],
             'atari': [args.hatari, args.atari_cc, args.cc, 'mformat', 'mcopy', 'mmd', 'mdir', 'mattrib'],
             'amiga': [args.fs_uae, args.amiga_cc, args.xdftool, 'xvfb-run']}[name]
    files = {'linux': [args.linux_cli], 'dos': [args.linux_cli, args.dos_cli],
             'atari': [args.linux_cli, args.atari_cli, args.tos, args.hconsole],
             'amiga': [args.linux_cli, args.amiga_cli, args.rom, args.workbench]}[name]
    return ['missing tool: ' + tool for tool in tools if not shutil.which(tool)] + \
           ['missing file: ' + str(path) for path in files if not path or not path.is_file()]


def emulator_case(args, evidence, name):
    child = evidence.directory / 'guest'
    if name == 'atari':
        command = [sys.executable, ROOT / 'test/check_hatari_fat.py', '--usable-roundtrip',
                   '--tos', args.tos, '--hatari', args.hatari, '--hconsole', args.hconsole,
                   '--cc', args.atari_cc, '--host-cc', args.cc, '--linux-cli', args.linux_cli,
                   '--atari-cli', args.atari_cli, '--output', child]
    else:
        command = [sys.executable, ROOT / 'test/check_fsuae_interop.py', '--usable-roundtrip',
                   '--rom', args.rom, '--workbench', args.workbench, '--fs-uae', args.fs_uae,
                   '--xdftool', args.xdftool, '--amiga-cc', args.amiga_cc,
                   '--linux-cli', args.linux_cli, '--amiga-cli', args.amiga_cli, '--output', child,
                   '--timeout', '180']
    evidence.run(command, timeout=360)
    record = json.loads((child / 'run.json').read_text())
    required = (['program_ran_with_restored_data', 'payload_and_executable_bytes_match',
                 'executable_fat_attributes_restored'] if name == 'atari' else
                ['script_and_program_ran_with_restored_data', 'payload_and_executable_bytes_match',
                 'script_protection_and_comment_restored'])
    required += ['enabled', 'existing_target_failure_preservation']
    for key in required:
        evidence.check(name + ': ' + key, record.get('usable_roundtrip', {}).get(key) is True)
    for reason in record.get('expected_partials', []):
        evidence.check(name + ': expected partial', True, detail=reason, expected_partial=True)
    return [record['filesystem'], 'Emulator evidence; physical hardware remains unqualified.'] + record.get('expected_partials', [])


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--platforms', default='linux', help='comma-separated linux,dos,atari,amiga, or all')
    p.add_argument('--output', type=Path, help='new directory for retained evidence')
    p.add_argument('--linux-cli', type=Path, default=ROOT / 'build/linux/boltpack')
    p.add_argument('--dos-cli', type=Path, default=ROOT / 'build/dos16/bpmini.exe')
    p.add_argument('--atari-cli', type=Path, default=ROOT / 'build/atari/boltpack.tos')
    p.add_argument('--amiga-cli', type=Path, default=ROOT / 'build/amiga/boltpack')
    p.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    p.add_argument('--dosbox-x', default='dosbox-x')
    p.add_argument('--hatari', default='hatari')
    p.add_argument('--atari-cc', default='m68k-atari-mint-gcc')
    p.add_argument('--tos', type=Path, default=os.environ.get('HATARI_TOS'))
    p.add_argument('--hconsole', type=Path, default=Path('/usr/share/hatari/hconsole/hconsole.py'))
    p.add_argument('--rom', type=Path, default=os.environ.get('AMIGA_ROM'))
    p.add_argument('--workbench', type=Path, default=os.environ.get('AMIGA_WORKBENCH'))
    p.add_argument('--fs-uae', default='fs-uae')
    p.add_argument('--amiga-cc', default='/opt/amiga/bin/m68k-amigaos-gcc')
    p.add_argument('--xdftool', default=os.environ.get('XDFTOOL', 'xdftool'))
    args = p.parse_args(argv)
    names = ['linux', 'dos', 'atari', 'amiga'] if args.platforms == 'all' else args.platforms.split(',')
    if not names or len(set(names)) != len(names) or any(n not in ('linux', 'dos', 'atari', 'amiga') for n in names):
        p.error('--platforms must select distinct linux,dos,atari,amiga entries or all')
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.expanduser().resolve())
    if args.output:
        out = args.output
        out.mkdir(parents=True, exist_ok=False)
    else:
        (ROOT / 'build').mkdir(exist_ok=True)
        out = Path(tempfile.mkdtemp(prefix='usable-roundtrip-', dir=ROOT / 'build'))
    spec = json.loads(SPEC.read_text())
    require(hashlib.sha256(spec['data'].encode('ascii')).hexdigest() == spec['data_sha256'],
            'independent data manifest is inconsistent')
    result = {'schema': 1, 'date_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'host': platform.platform(), 'argv': sys.argv, 'manifest_sha256': digest(SPEC),
              'environment': {'python': sys.version, 'timezone': list(time.tzname),
                              'TZ': os.environ.get('TZ')},
              'results': [], 'unqualified': ['macOS/Classic Mac', 'Windows', 'QL',
                  'other frontends not selected by this gate']}
    result['revision'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    (out / 'git-status.txt').write_bytes(subprocess.check_output(['git', 'status', '--short'], cwd=ROOT))
    (out / 'git-diff.patch').write_bytes(subprocess.check_output(['git', 'diff', '--binary', 'HEAD'], cwd=ROOT))
    for name in names:
        directory = out / name
        directory.mkdir()
        evidence = Evidence(directory)
        row = {'platform': name, 'status': 'unavailable', 'checks': evidence.checks,
               'commands': evidence.commands}
        missing = prerequisites(args, name)
        if missing:
            row['reason'] = missing
        else:
            try:
                row['limitations'] = linux_case(args, evidence, spec) if name == 'linux' else \
                    dos_case(args, evidence, spec) if name == 'dos' else emulator_case(args, evidence, name)
                row['status'] = 'pass'
            except (OSError, subprocess.SubprocessError, RuntimeError, KeyError, ValueError) as exc:
                row['status'] = 'fail'
                row['reason'] = str(exc)
        result['results'].append(row)
        print('%s: %s%s' % (name, row['status'].upper(), ' — ' + str(row['reason']) if 'reason' in row else ''), flush=True)
        (out / 'run.json').write_text(json.dumps(result, indent=2) + '\n')
    # Hash the actual binaries, runner sources, logs, archives and restored data.
    result['binaries'] = {key: {'path': str(value), 'sha256': digest(value)}
                          for key, value in vars(args).items()
                          if key.endswith('_cli') and value.is_file()}
    result['runner_sources'] = {path.relative_to(ROOT).as_posix(): digest(path) for path in
        [Path(__file__).resolve(), ROOT / 'test/usable/probe.c', ROOT / 'test/usable/hatari_wait.py',
         ROOT / 'test/check_hatari_fat.py', ROOT / 'test/atari/guest_runner.c',
         ROOT / 'test/check_fsuae_interop.py']}
    result['artifacts'] = {path.relative_to(out).as_posix(): {'size': path.stat().st_size, 'sha256': digest(path)}
                           for path in sorted(out.rglob('*')) if path.is_file() and path != out / 'run.json'}
    states = [row['status'] for row in result['results']]
    result['status'] = 'fail' if 'fail' in states else 'unavailable' if 'unavailable' in states else 'pass'
    (out / 'run.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Evidence: ' + str(out / 'run.json'))
    return 1 if 'fail' in states else 2 if 'unavailable' in states else 0


if __name__ == '__main__':
    raise SystemExit(main())
