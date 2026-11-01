#!/usr/bin/env python3
"""Exercise the Linux FLTK GUI target-profile create flow under Xvfb."""

import argparse
import os
import pathlib
import subprocess
import tempfile


SCRIPT = r'''set -eu
"$1" &
gui_pid=$!
trap 'kill "$gui_pid" 2>/dev/null || true' EXIT
main=$(xdotool search --sync --onlyvisible --name '^BoltPack$' | head -1)
choose_path() {
    title=$1
    path=$2
    dialog=$(xdotool search --sync --onlyvisible --name "^$title$" | head -1)
    xdotool windowfocus --sync "$dialog"
    xdotool key ctrl+l
    sleep 0.3
    xdotool type --clearmodifiers "$path"
    xdotool key Return
    sleep 0.5
    eval "$(xdotool getwindowgeometry --shell "$dialog")"
    xdotool mousemove --window "$dialog" "$((WIDTH - 55))" \
        "$((HEIGHT - 28))" click 1
}
xdotool windowfocus --sync "$main"
xdotool key --window "$main" ctrl+n
create_dialog=$(xdotool search --sync --onlyvisible --name '^Create Archive$' | head -1)
eval "$(xdotool getwindowgeometry --shell "$create_dialog")"
xdotool mousemove --window "$create_dialog" 105 "$((HEIGHT - 24))" click 1
choice=$(xdotool search --sync --onlyvisible --name '^$' | tail -1)
xdotool windowfocus --sync "$choice"
xdotool key Tab Tab Return
choose_path 'Select files to archive' "$2"
choose_path 'Save archive as' "$3/out"
for attempt in $(seq 1 150); do
    test -s "$3/out.blt" && break
    sleep 0.1
done
test -s "$3/out.blt"
kill -TERM "$gui_pid"
trap - EXIT
wait "$gui_pid" >/dev/null 2>&1 || true
'''


def run_gui(gui, source, out_dir, prefs_text, environment):
    config_home = out_dir.parent / ("config-" + out_dir.name)
    prefs = config_home / "boltpack"
    prefs.mkdir(parents=True, exist_ok=True)
    (prefs / "gui.conf").write_text(prefs_text, encoding="ascii")
    env = dict(environment)
    env["XDG_CONFIG_HOME"] = str(config_home)
    command = ["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24",
               "sh", "-c", SCRIPT, "linux-gui-target-test", str(gui),
               str(source), str(out_dir)]
    subprocess.run(command, check=True, timeout=120, env=env,
                   capture_output=True)
    archive = out_dir / "out.blt"
    assert archive.is_file() and archive.stat().st_size > 0
    return archive


def check_report(cli, archive, extract_dir, original, payload, partial=False):
    subprocess.run([str(cli), "t", str(archive)], check=True,
                   capture_output=True)
    result = subprocess.run([str(cli), "x", "-o", str(extract_dir),
                             str(archive)], capture_output=True)
    if partial:
        assert result.returncode in (0, 1), result.stderr
    else:
        assert result.returncode == 0, result.stderr
    report = (extract_dir / "BPNAMES.TXT").read_text("ascii")
    rows = dict(line.split("\t", 1) for line in report.splitlines()[2:])
    mapped = rows[original]
    assert (extract_dir / mapped).read_bytes() == payload, (mapped, report)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gui", required=True, type=pathlib.Path)
    parser.add_argument("--cli", required=True, type=pathlib.Path)
    args = parser.parse_args()
    gui = args.gui.resolve()
    cli = args.cli.resolve()

    for executable in ("xvfb-run", "xdotool"):
        if subprocess.run(["sh", "-c", f"command -v {executable}"],
                          capture_output=True).returncode != 0:
            raise SystemExit(f"required GUI test tool is missing: {executable}")

    environment = dict(os.environ)
    environment.setdefault("LIBGL_ALWAYS_SOFTWARE", "1")

    with tempfile.TemporaryDirectory(prefix="boltpack-gui-target-") as temp:
        root = pathlib.Path(temp)
        name = "Long filename 01.txt"
        source = root / name
        payload = b"target-profile-gui\n"
        source.write_bytes(payload)

        atari_dir = root / "atari"
        atari_dir.mkdir()
        atari_archive = run_gui(
            gui, source, atari_dir,
            "method=1\nlevel=1\ntarget_profile=1\ntarget_auto_rename=1\n",
            environment)
        check_report(cli, atari_archive, root / "atari-extract", name, payload)

        plan9_dir = root / "plan9"
        plan9_dir.mkdir()
        plan9_archive = run_gui(
            gui, source, plan9_dir,
            "method=2\nlevel=1\ntarget_profile=16\ntarget_auto_rename=1\n",
            environment)
        check_report(cli, plan9_archive, root / "plan9-extract", name, payload,
                     partial=True)

        print("Linux FLTK GUI target profile: PASS")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
