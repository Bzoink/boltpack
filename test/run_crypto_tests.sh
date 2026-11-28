#!/bin/sh
# Phase 1 tests of src/core/crypto (plan section 13, phase 1).
#
#   sh test/run_crypto_tests.sh [--quick]     (make crypto-test)
#
# Builds the crypto layer in four variants (native, -m32, ASan+UBSan, and without
# suite S1), runs the C unit tests and the oracle comparison in each, checks that
# the C89 files compile as strict C89, that a release build contains no test
# hooks, and, if valgrind is installed, the secret-taint (constant-time) check.
# Every child process runs under timeout.  Not part of `make all`.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${CRYPTO_BUILD:-$ROOT/build/crypto}
CC=${CC:-gcc}
C=$ROOT/src/core/crypto
QUICK=""
[ "${1:-}" = "--quick" ] && QUICK="--quick"
mkdir -p "$OUT"
fail=0
skips=0
ok()   { echo "  ok    $*"; }
bad()  { echo "  FAIL  $*"; fail=1; }

COMMON="-std=c99 -Wall -Wextra -Wno-unused-function -DBP_CRYPTO_TESTING -I$C -I$C/vendor/ascon-c/ref"
BASE="$C/bp_cutil.c $C/bp_crypto.c $C/bp_entropy.c $C/bp_entropy_os_posix.c $C/bp_ascon_ref.c"
S1SRC="$C/bp_s1.c $C/vendor/monocypher/monocypher.c"

echo "== strict C89 compile of the C89 files (no S1; with and without test hooks)"
for f in bp_cutil.c bp_crypto.c bp_entropy.c; do
    for extra in "" "-DBP_CRYPTO_TESTING"; do
        if timeout 120 $CC -std=c89 -pedantic -Wall -Wextra -Werror -Wno-unused-function $extra \
              -I"$C" -c "$C/$f" -o "$OUT/c89_$f.o" 2>"$OUT/c89.log"; then ok "$f $extra"
        else bad "$f $extra"; cat "$OUT/c89.log"; fi
    done
done
echo "== the same files compile with BP_CRYPTO_S1 defined (C89 side of the S1 switch)"
timeout 120 $CC -std=c89 -pedantic -Werror -Wno-unused-function -DBP_CRYPTO_S1 -I"$C" -c "$C/bp_crypto.c" -o "$OUT/c89_s1.o" 2>"$OUT/c89.log" && ok "bp_crypto.c -DBP_CRYPTO_S1" || { bad "bp_crypto.c -DBP_CRYPTO_S1"; cat "$OUT/c89.log"; }

echo "== release build has no test hooks"
timeout 120 $CC -std=c99 -O2 -DBP_CRYPTO_S1 -I"$C" -I"$C/vendor/ascon-c/ref" -Wno-unused-function \
    $BASE $S1SRC -x c - -o "$OUT/release_probe" <<'PROBE' 2>"$OUT/rel.log"
#include <stdio.h>
#include "bp_crypto.h"
int main(void) { printf("%d\n", bp_crypto_has_test_hooks()); return 0; }
PROBE
if [ "$(timeout 20 "$OUT/release_probe" 2>/dev/null)" = "0" ]; then ok "bp_crypto_has_test_hooks() == 0"; else bad "release build reports test hooks"; cat "$OUT/rel.log"; fi
if nm "$OUT/release_probe" 2>/dev/null | grep -q "bp_test_\|bp_entropy_test_"; then bad "test symbols in a release build"; else ok "no bp_test_* or bp_entropy_test_* symbols"; fi

echo "== oracle prerequisites"
oracle_ok=1
if python3 -c "import cryptography.hazmat.primitives.ciphers.aead" 2>/dev/null; then ok "python cryptography (ChaCha20-Poly1305)"
else echo "  skipped: python 'cryptography' is not installed; the oracle comparison cannot run"; oracle_ok=0; fi
if timeout 60 openssl kdf -keylen 16 -kdfopt pass:a -kdfopt salt:saltsalt -kdfopt iter:1 -kdfopt memcost:8 -kdfopt lanes:1 -kdfopt threads:1 ARGON2ID >/dev/null 2>&1; then ok "openssl kdf ARGON2ID"
else echo "  skipped: 'openssl kdf ARGON2ID' is not available (OpenSSL 3.2 or later); the oracle comparison cannot run"; oracle_ok=0; fi
[ "$oracle_ok" = 0 ] && skips=1

run_variant() {
    name=$1; shift
    flags=$1; shift
    srcs=$1; shift
    drv=$1; shift
    echo "== variant: $name"
    if timeout 300 $CC $COMMON $flags "$ROOT/test/unit/crypto_tool.c" $srcs -o "$OUT/tool_$name" 2>"$OUT/b.log" &&
       timeout 300 $CC $COMMON $flags "$ROOT/test/unit/test_crypto.c" $srcs -o "$OUT/unit_$name" 2>>"$OUT/b.log"; then ok "built"
    else bad "build"; head -30 "$OUT/b.log"; return; fi
    r=$(timeout 600 "$OUT/unit_$name" 2>&1 | tail -3)
    case "$r" in *" 0 failed") ok "unit tests: $r";; *) bad "unit tests: $r";; esac
    if [ "$oracle_ok" = 1 ]; then
        r=$(timeout 900 python3 -I "$ROOT/test/check_crypto.py" "$OUT/tool_$name" $QUICK $drv 2>&1 | tail -3)
        case "$r" in *" 0 failed"*) ok "oracle comparison: $r";; *) bad "oracle comparison: $r";; esac
    else echo "  SKIPPED oracle comparison (prerequisites missing)"; fi
}
cd "$ROOT" || exit 1
run_variant native   "-O2 -DBP_CRYPTO_S1"                                                "$BASE $S1SRC" ""
if printf 'int main(void){return 0;}' | $CC -m32 -x c - -o "$OUT/m32probe" 2>/dev/null; then
    run_variant m32 "-O2 -m32 -DBP_CRYPTO_S1" "$BASE $S1SRC" ""
else echo "== variant: m32"; echo "  SKIPPED (no 32-bit libraries: install gcc-multilib)"; skips=1; fi
run_variant san      "-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DBP_CRYPTO_S1" "$BASE $S1SRC" ""
run_variant s2only   "-O2"                                                               "$BASE"        "--s2-only"

echo "== 16-bit smoke: compile-only with Open Watcom wcc (DOS, compact model, -0), not run"
WCC=${WCC16:-/opt/watcom/binl64/wcc}
if [ -x "$WCC" ]; then
    wfail=0
    for f in bp_cutil bp_crypto bp_entropy; do
        for extra in "" "-DBP_CRYPTO_TESTING"; do
            if WATCOM=/opt/watcom INCLUDE=/opt/watcom/h timeout 120 "$WCC" -bt=dos -mc -0 -za -os -zm -w4 -we -wcd=202 \
                 -DSIZEOF_INT=2 -DSIZEOF_LONG=4 $extra -I"$C" -fo="$OUT/w16_$f.obj" "$C/$f.c" >"$OUT/w16.log" 2>&1; then :
            else wfail=1; echo "  wcc failed on $f $extra"; cat "$OUT/w16.log" | head -10; fi
        done
    done
    [ "$wfail" = 0 ] && ok "bp_cutil.c bp_crypto.c bp_entropy.c compile with wcc -bt=dos -mc -0 -za (warnings are errors)" || bad "16-bit compile"
else echo "  SKIPPED ($WCC not found): no 16-bit compiler was run"; skips=1; fi

echo "== constant time (valgrind secret-taint)"
if command -v valgrind >/dev/null 2>&1 && [ -f /usr/include/valgrind/memcheck.h ]; then
    if timeout 300 $CC $COMMON -O1 -g -DBP_CRYPTO_S1 "$ROOT/test/unit/ctgrind_crypto.c" $BASE $S1SRC -o "$OUT/ctgrind" 2>"$OUT/b.log"; then
        if timeout 600 valgrind --error-exitcode=9 -q "$OUT/ctgrind" >"$OUT/ct.log" 2>&1; then ok "no secret-dependent branch or index (compare, derive, seal; both suites)"
        else bad "valgrind reported a secret-dependent branch"; head -20 "$OUT/ct.log"; fi
    else bad "ctgrind build"; head -20 "$OUT/b.log"; fi
else echo "  skipped (valgrind or valgrind/memcheck.h not installed)"; fi

if [ "$fail" != 0 ]; then echo "crypto-test: FAILED"; elif [ "$skips" != 0 ]; then echo "crypto-test: passed, with SKIPPED checks (see above)"; else echo "crypto-test: all passed"; fi
exit $fail
