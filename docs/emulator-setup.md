# Atari and Amiga emulator setup

This is the working setup observed on Tony's Debian machine on 2026-10-28.
Paths are local to this machine; other installations should provide their own
images and use the corresponding paths. Keep ROMs and system disks outside the
repository and run tests against disposable working copies.

## Atari: Hatari

- Emulator: Hatari 2.5.0.
- Machine: STE, TOS 1.62.
- ROM: `~/images/Atari/tos162se.img`.
- Other available TOS images: `tos106se.img` and `tos205se.img` in the same
  directory.

On a graphical desktop, start an interactive emulator with:

```sh
hatari --machine ste --tos "$HOME/images/Atari/tos162se.img" --sound off
```

The following bounded, headless startup check was run successfully. It exits
after 250 vertical blanks; it does not run BoltPack or prove file transfer.

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  hatari --machine ste --tos "$HOME/images/Atari/tos162se.img" \
  --sound off --confirm-quit off --run-vbls 250
```

The Atari cross-compiler is available here. `make atari-cli` and
`make atari-gui` build the applications. A reproducible CLI exchange test uses
Hatari's host-directory drive and retains its commands, exit statuses, logs,
files and ROM/payload hashes in a temporary evidence directory:

```sh
python3 test/check_hatari_interop.py --tos "$HOME/images/Atari/tos162se.img"
```

This checks Linux-to-Atari and Atari-to-Linux file bytes for Stored and
BoltLite, Atari append/delete, failed edit preservation, and checksum failure
cleanup over an existing destination. It also checks nested and empty
directories and pre-output refusal of a long name, case-colliding files, and
case-colliding implicit parents.
Directory timestamp restoration reports partial on this backend. Linux reports partial preservation for
Atari-specific metadata. It does not validate a native FAT disk, attributes,
timestamps, or the GEM GUI; test those separately.

For FAT12 floppy testing, run:

```sh
python3 test/check_hatari_fat.py --tos "$HOME/images/Atari/tos162se.img"
```

This uses a disposable 720 KiB image and `hconsole.py` for a normal Hatari
quit. The `--run-vbls` option terminates before the changed floppy is saved;
the control-console `quit` ejects it. The test therefore reads the final files
back from the image with mtools after Hatari exits. FAT12 file hidden/system
flags and timestamps round-trip. TOS 1.62 reports partial for directory times
and nonzero directory attributes; GEM GUI behavior remains unqualified.

## Amiga: FS-UAE

- Emulator: FS-UAE 3.1.66.
- Machine: A500+ with Kickstart 2.04, revision 37.175.
- ROM: `~/images/Amiga/kickstart-2.04-a500plus.rom`.
- Workbench disk: `~/images/Amiga/Workbench-2.04/1-Workbench.adf`.
  Extras, Fonts, and Install disks are in the same directory.

For an interactive graphical desktop, create a temporary copy of the Workbench
disk and launch:

```sh
BP_AMIGA_TMP=$(mktemp -d /tmp/boltpack-amiga.XXXXXX)
cp "$HOME/images/Amiga/Workbench-2.04/1-Workbench.adf" "$BP_AMIGA_TMP/workbench.adf"
fs-uae --amiga_model='A500+' \
  --kickstart_file="$HOME/images/Amiga/kickstart-2.04-a500plus.rom" \
  --floppy_drive_0="$BP_AMIGA_TMP/workbench.adf" \
  --floppy_drive_speed=800 --warp_mode=1 --fullscreen=0
```

The copy is a working disk: any guest writes affect it, not the original ADF.
Its temporary directory can be removed after the emulator exits and any needed
test output has been collected.

The validated boot used Xvfb with a 1024×768×24 screen and software OpenGL
(`LIBGL_ALWAYS_SOFTWARE=1`). The screen remained white during startup; with
the accelerated settings above, the boot reached a keymap-selection prompt
after roughly 35 seconds. Enter `0` and press Enter to decline copying a keymap;
the Workbench desktop then appeared. A virtual-display run is useful for
automation but is not an interactive desktop for the user.

FS-UAE's Print Screen shortcut saved the actual emulated frame during this
check. A temporary test home needed a `Desktop` directory for the screenshot
files. A guest startup screenshot is not proof of BoltPack interoperability.
For unattended CLI testing, install `amitools` in a Python environment and
provide its `xdftool` executable to the runner:

```sh
python3 test/check_fsuae_interop.py \
  --rom "$HOME/images/Amiga/kickstart-2.04-a500plus.rom" \
  --workbench "$HOME/images/Amiga/Workbench-2.04/1-Workbench.adf" \
  --xdftool /path/to/xdftool
```

The runner modifies only a disposable Workbench copy, boots a host-directory
drive, captures guest exit statuses and verifies Stored and BoltLite profile
file bytes both ways, a successful profile append, and incompatible options.
It also verifies failed creation and append after a valid first input preserve
the existing archive and clean up a failed new archive, a successful and
failed delete, and archive protection/comment preservation across edits.
It checks native protection/comment restoration on an archived file and two
directories (including an empty one), and failed checksum extraction over an
existing file with metadata.
It defaults to a 256 KiB shell stack; add `--stack 0` to use the guest default.
The current corpus passes at the default stack after reducing core writer and
recursive traversal stack use. Deep trees and other codecs still need separate
stack qualification. Linux reports partial
preservation for Amiga-native metadata. Native disk filesystem behavior,
interruption recovery, the GUI, and broader archived metadata coverage remain open.

The host-directory backend does not establish symlink-safe creation: in a
probe, FS-UAE exposed a host symlink as a regular file and the CLI archived its
target. A separate FFS `MakeLink ... SOFT` attempt failed, so native soft-link
handling remains unverified.

To run a separate test against a disposable native OFS or FFS floppy image,
use the same arguments with `test/check_fsuae_native.py`; add `--filesystem ffs`
for FFS (the default is OFS). It creates and formats its
own ADF, enables direct writes only on the disposable images, waits for the
guest's delayed floppy writes, and retains the resulting image and logs. It
checks a BoltLite profile transfer, create/append/delete, failed append, native
file protection, comment and timestamp restoration, and Linux extraction of
the edited archive. It transfers `café.txt` both ways and checks pre-output
refusal for unrepresentable Unicode and case-colliding implicit parents. It
uses a 256 KiB guest stack; it does not qualify the GUI.
