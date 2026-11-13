#!/usr/bin/env python3
"""Drive hconsole until the disposable guest reports completion, then eject cleanly."""
import argparse
import importlib.util
from pathlib import Path
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--hconsole', type=Path, required=True)
    p.add_argument('--hatari', default='hatari')
    p.add_argument('--marker', type=Path, required=True)
    p.add_argument('--timeout', type=float, default=120)
    p.add_argument('options', nargs=argparse.REMAINDER)
    args = p.parse_args()
    module_spec = importlib.util.spec_from_file_location('hconsole', args.hconsole)
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    module.Hatari.hataribin = args.hatari
    options = args.options[1:] if args.options[:1] == ['--'] else args.options
    hatari = module.Hatari(options)
    completed = False
    try:
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline and hatari.is_running():
            if args.marker.is_file() and args.marker.read_bytes().strip() == b'FINISHED':
                completed = True
                break
            time.sleep(0.25)
        # A normal quit lets Hatari flush its disposable floppy image.
        if hatari.is_running():
            hatari.trigger_shortcut('quit')
        deadline = time.monotonic() + 10
        while hatari.is_running() and time.monotonic() < deadline:
            time.sleep(0.1)
        if hatari.is_running():
            raise RuntimeError('Hatari did not shut down cleanly')
        if not completed:
            raise RuntimeError('Guest did not complete before its deadline')
    finally:
        if hatari.is_running():
            hatari.kill_hatari()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
