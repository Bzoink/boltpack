#!/usr/bin/env python3

import argparse
import pathlib
import struct
import sys


WB_DISKMAGIC = 0xE310
WB_DISKVERSION = 1
WB_DISKREVISION = 1
WBTOOL = 3
NO_ICON_POSITION = -2147483648
PTR_PRESENT = 0xABADCAFE

GFLG_GADGIMAGE = 0x0004
GACT_RELVERIFY = 0x0001
GACT_IMMEDIATE = 0x0002
GTYP_BOOLGADGET = 0x0001

ICON_WIDTH = 47
ICON_HEIGHT = 37
ICON_DEPTH = 2
STACK_SIZE = 65536

ICON_ROWS = (
    "...............................................",
    "...............................................",
    "...............................................",
    "...............................................",
    ".......................########................",
    ".......................########................",
    "......................########.................",
    "......................########.................",
    ".....................########..................",
    ".............#######################...........",
    "............#.......########......##...........",
    "...........#........########.....#.#...........",
    "...........#.......########.....##.#...........",
    "..........#........########.....#..#...........",
    ".........#........########.....#...#...........",
    "........#######################....#...........",
    "........#........########.....#....#...........",
    "........#........###############...#...........",
    "........#.......###############....#...........",
    "........#..####################....#...........",
    "........#............##########....#...........",
    "........#............########.#....#...........",
    "........#............#######..#....#...........",
    "........#..#################..#....#...........",
    "........#...........######....#...#............",
    "........#...........#####.....#..#.............",
    "........#...........#####.....#.#..............",
    "........#..........#####......#.#..............",
    "........#..........####.......##...............",
    "........#######################................",
    "..................###..........................",
    "..................##...........................",
    "..................##...........................",
    ".................##............................",
    ".................#.............................",
    "...............................................",
    "...............................................",
)

ICON_SPECS = (
    ("boltpack-gui.info", None),
    ("boltpack.info", "CON:10/20/620/160/BoltPack CLI/AUTO/CLOSE/WAIT"),
)


def write_u8(handle, value):
    handle.write(struct.pack(">B", value & 0xFF))


def write_u16(handle, value):
    handle.write(struct.pack(">H", value & 0xFFFF))


def write_i16(handle, value):
    handle.write(struct.pack(">h", value))


def write_u32(handle, value):
    handle.write(struct.pack(">I", value & 0xFFFFFFFF))


def write_i32(handle, value):
    handle.write(struct.pack(">i", value))


def pixel_value(char):
    if char == ".":
        return 0
    if char == "#":
        return 1
    if char in "0123":
        return int(char)
    raise ValueError("unsupported icon pixel: %r" % (char,))


def pack_icon_rows(rows):
    words_per_row = (ICON_WIDTH + 15) // 16
    plane_stride = ICON_HEIGHT * words_per_row
    words = [0] * (ICON_DEPTH * plane_stride)
    plane_index = 0
    row_index = 0
    column_index = 0
    bit_index = 0
    pixel = 0

    for row_index, row in enumerate(rows):
        if len(row) != ICON_WIDTH:
            raise ValueError("icon row %d has width %d, expected %d" % (
                row_index,
                len(row),
                ICON_WIDTH,
            ))
        for column_index, char in enumerate(row):
            pixel = pixel_value(char)
            for plane_index in range(ICON_DEPTH):
                if pixel & (1 << plane_index):
                    bit_index = 15 - (column_index % 16)
                    words[
                        plane_index * plane_stride
                        + row_index * words_per_row
                        + (column_index // 16)
                    ] |= 1 << bit_index

    return words


def write_icon_string(handle, text):
    data = text.encode("latin-1") + b"\0"
    write_u32(handle, len(data))
    handle.write(data)


def write_image(handle, image_words):
    word = 0

    write_i16(handle, 0)
    write_i16(handle, 0)
    write_i16(handle, ICON_WIDTH)
    write_i16(handle, ICON_HEIGHT)
    write_i16(handle, ICON_DEPTH)
    write_u32(handle, PTR_PRESENT)
    write_u8(handle, (1 << ICON_DEPTH) - 1)
    write_u8(handle, 0)
    write_u32(handle, 0)

    for word in image_words:
        write_u16(handle, word)


def write_disk_object(handle, tool_window, image_words):
    write_u16(handle, WB_DISKMAGIC)
    write_u16(handle, WB_DISKVERSION)

    write_u32(handle, 0)
    write_i16(handle, 0)
    write_i16(handle, 0)
    write_i16(handle, ICON_WIDTH)
    write_i16(handle, ICON_HEIGHT)
    write_u16(handle, GFLG_GADGIMAGE)
    write_u16(handle, GACT_RELVERIFY | GACT_IMMEDIATE)
    write_u16(handle, GTYP_BOOLGADGET)
    write_u32(handle, PTR_PRESENT)
    write_u32(handle, 0)
    write_u32(handle, 0)
    write_i32(handle, 0)
    write_u32(handle, 0)
    write_u16(handle, 0)
    write_u32(handle, WB_DISKREVISION)

    write_u8(handle, WBTOOL)
    write_u8(handle, 0)
    write_u32(handle, 0)
    write_u32(handle, 0)
    write_i32(handle, NO_ICON_POSITION)
    write_i32(handle, NO_ICON_POSITION)
    write_u32(handle, 0)
    write_u32(handle, PTR_PRESENT if tool_window else 0)
    write_i32(handle, STACK_SIZE)

    write_image(handle, image_words)

    if tool_window:
        write_icon_string(handle, tool_window)


def create_icons(output_dir):
    output_dir.mkdir(parents=True, exist_ok=True)

    if len(ICON_ROWS) != ICON_HEIGHT:
        raise ValueError("icon height mismatch: %d rows, expected %d" % (
            len(ICON_ROWS),
            ICON_HEIGHT,
        ))

    image_words = pack_icon_rows(ICON_ROWS)

    for filename, tool_window in ICON_SPECS:
        target = output_dir / filename
        with target.open("wb") as handle:
            write_disk_object(handle, tool_window, image_words)
        print(target)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate classic Amiga Workbench .info icons for BoltPack."
    )
    parser.add_argument(
        "output_dir",
        nargs="?",
        default=".",
        help="directory where .info files will be written",
    )
    args = parser.parse_args(argv)

    try:
        create_icons(pathlib.Path(args.output_dir))
    except Exception as exc:
        print("error: %s" % (exc,), file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())