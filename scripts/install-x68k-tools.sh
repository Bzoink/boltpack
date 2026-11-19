#!/bin/sh
# Install the Sharp X68000 (Human68k) build and test tools on Debian/Ubuntu:
#   - elf2x68k: GCC 13 cross-compiler for Human68k (m68k-xelf-gcc)
#   - run68x:   runs Human68k command-line programs on Linux (run68), with
#               scripts/patches/run68x-cmpa-w.patch: run68x 2.3.2 compares only
#               16 bits for CMPA.W, which made correct GCC -O2 code fail
#
# Usage: sh scripts/install-x68k-tools.sh
#   PREFIX=/opt        where elf2x68k goes (default /opt -> /opt/m68k-xelf)
#   BINDIR=/usr/local/bin  where run68 goes
#
# Versions and the toolchain checksum are pinned. Uses sudo for installing.
set -eu

ELF2X68K_VERSION=20261024
ELF2X68K_FILE=elf2x68k-Linux-$ELF2X68K_VERSION.tar.bz2
ELF2X68K_URL=https://github.com/yunkya2/elf2x68k/releases/download/$ELF2X68K_VERSION/$ELF2X68K_FILE
ELF2X68K_SHA256=8104310f677c81f39b7b61a6fc6e1a936b76fce1bba4f50df4bd6d8fef03f3ef
RUN68X_TAG=v2.3.2
RUN68X_URL=https://github.com/kg68k/run68x
RUN68X_PATCH=$(cd "$(dirname "$0")" && pwd)/patches/run68x-cmpa-w.patch

PREFIX=${PREFIX:-/opt}
BINDIR=${BINDIR:-/usr/local/bin}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

say() { printf '\n==> %s\n' "$*"; }

if [ "$(uname -m)" != "x86_64" ]; then
    echo "elf2x68k's Linux release is for x86-64; this machine is $(uname -m)." >&2
    exit 1
fi

say "Installing Debian packages"
sudo apt-get update
sudo apt-get install -y wget bzip2 ca-certificates git cmake build-essential python3 patch

say "Downloading elf2x68k $ELF2X68K_VERSION"
wget -q --show-progress -O "$WORK/$ELF2X68K_FILE" "$ELF2X68K_URL"
echo "$ELF2X68K_SHA256  $WORK/$ELF2X68K_FILE" | sha256sum -c -

say "Installing elf2x68k into $PREFIX/m68k-xelf"
if [ -d "$PREFIX/m68k-xelf" ]; then
    echo "Replacing the existing $PREFIX/m68k-xelf"
    sudo rm -rf "$PREFIX/m68k-xelf"
fi
sudo mkdir -p "$PREFIX"
sudo tar xjf "$WORK/$ELF2X68K_FILE" -C "$PREFIX"
# The archive's directories are owner-only (rwxrwx---); as root, tar keeps
# that, so let every user read the toolchain and run its programs.
sudo chmod -R a+rX "$PREFIX/m68k-xelf"

say "Building run68x $RUN68X_TAG"
git clone --quiet --depth 1 --branch "$RUN68X_TAG" "$RUN68X_URL" "$WORK/run68x"
patch -d "$WORK/run68x" -p1 < "$RUN68X_PATCH"
cmake -S "$WORK/run68x" -B "$WORK/run68x/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$WORK/run68x/build" -j "$(nproc)" >/dev/null
sudo install -m 755 "$WORK/run68x/build/run68" "$BINDIR/run68"

say "Adding $PREFIX/m68k-xelf/bin to PATH"
LINE="export PATH=$PREFIX/m68k-xelf/bin:\$PATH"
if ! grep -qxF "$LINE" "$HOME/.bashrc" 2>/dev/null; then
    printf '\n# X68000 cross-compiler (elf2x68k)\n%s\n' "$LINE" >> "$HOME/.bashrc"
    echo "Added to ~/.bashrc; open a new terminal or run: $LINE"
else
    echo "Already in ~/.bashrc"
fi

say "Checking"
"$PREFIX/m68k-xelf/bin/m68k-xelf-gcc" --version | head -1
printf '#include <stdio.h>\nint main(void){puts("Hello from Human68k");return 0;}\n' > "$WORK/hello.c"
"$PREFIX/m68k-xelf/bin/m68k-xelf-gcc" -O2 "$WORK/hello.c" -o "$WORK/hello.x"
(cd "$WORK" && "$BINDIR/run68" hello.x)
# CMPA.W must compare all 32 bits of the address register (the patch above).
cat > "$WORK/cmpa.c" <<'CMPA_EOF'
#include <stdio.h>
int main(void)
{
    unsigned long ne;
    __asm__ volatile("move.l #0x10000,%%a0\n\tmoveq #0,%0\n\tcmpa.w #0,%%a0\n\tsne %0"
                     : "=&d"(ne) : : "a0", "cc");
    puts((ne & 0xFF) ? "CMPA.W check passed" : "CMPA.W check FAILED");
    return (ne & 0xFF) ? 0 : 1;
}
CMPA_EOF
"$PREFIX/m68k-xelf/bin/m68k-xelf-gcc" -O2 "$WORK/cmpa.c" -o "$WORK/cmpa.x"
(cd "$WORK" && "$BINDIR/run68" cmpa.x)

say "Done: m68k-xelf-gcc in $PREFIX/m68k-xelf/bin, run68 in $BINDIR"
