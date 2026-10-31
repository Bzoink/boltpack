# BoltPack Makefile
# Set FRONTEND to build a different platform frontend (default: linux).

.DEFAULT_GOAL := all

CC       = cc
CFLAGS   = -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 \
           -ffunction-sections -fdata-sections
LDFLAGS  = -Wl,--gc-sections

DEBUG_CFLAGS   = -ansi -pedantic -Wall -Wextra -Wno-unused-function -O0 -g -fno-omit-frame-pointer -fsanitize=address,undefined
DEBUG_LDFLAGS  = -fsanitize=address,undefined
PROFILE_CFLAGS = -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 -g -fno-omit-frame-pointer

ENABLE_FAST_FLUFF ?= 0
FAST_FLUFF_CPPFLAGS =
FAST_FLUFF_LDFLAGS  =
FAST_FLUFF_CORE_OBJ =

ifeq ($(ENABLE_FAST_FLUFF),1)
FAST_FLUFF_CPPFLAGS += -DBOLTPACK_FAST_FLUFF
FAST_FLUFF_LDFLAGS  += -fopenmp
FAST_FLUFF_CORE_OBJ += $(BUILDDIR)/core/fluff_fast.o
endif

# ---- zstd (Zstandard, method 4) ----------------------------------------
# ENABLE_ZSTD=1 (default) compiles the zstd amalgamation and wrapper into
# the native Linux and Windows builds.  Set ENABLE_ZSTD=0 to exclude it
# (required for vintage/16-bit cross-compile targets).
# lib/zstd/zstd.c is C99 and explicitly exempt from the C89 rule.
ZSTDDIR           = lib/zstd
ENABLE_ZSTD       ?= 1
ZSTD_CPPFLAGS     =
ZSTD_OBJS         =
# ZSTD_STRIP_ERROR_STRINGS drops the human-readable error-name table;
# BoltPack maps zstd errors to its own BP_ERR_* codes and never prints them.
ZSTD_CC_FLAGS     = -std=c99 -O2 -Wall -Wno-unused-function \
                    -ffunction-sections -fdata-sections \
                    -DZSTD_STRIP_ERROR_STRINGS

ifeq ($(ENABLE_ZSTD),1)
ZSTD_CPPFLAGS     = -DBOLTPACK_ZSTD
ZSTD_OBJS         = $(BUILDDIR)/zstd/zstd.o $(BUILDDIR)/core/zstd_wrap.o
endif

BOLTLZDIR      = src/core
BOLTLZ_OBJS    = $(BUILDDIR)/boltlz/boltlz.o
BOLTLZ_BUILD_DIR = build/boltlz
BOLTLZ_CORE_FLAGS = -O2 -ffunction-sections -fdata-sections
BOLTLZ_CORE_CFLAGS  = -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 -ffunction-sections -fdata-sections

# Full build version is read from VERSION at the repo root.
# Mini build version is read from VERSION_MINI at the repo root.
# Derive MAJOR/MINOR for -D flags injected into every C compilation below.
BP_VERSION      := $(shell cat VERSION 2>/dev/null | tr -d '[:space:]' || echo 1.0)
BP_VERSION_MAJOR := $(word 1,$(subst ., ,$(BP_VERSION)))
BP_VERSION_MINOR := $(word 2,$(subst ., ,$(BP_VERSION)))
BP_VER_DEFS     := -DBP_VERSION_MAJOR=$(BP_VERSION_MAJOR) -DBP_VERSION_MINOR=$(BP_VERSION_MINOR)

BP_VERSION_MINI      := $(shell cat VERSION_MINI 2>/dev/null | tr -d '[:space:]' || echo 1.0)
BP_VERSION_MINI_MAJOR := $(word 1,$(subst ., ,$(BP_VERSION_MINI)))
BP_VERSION_MINI_MINOR := $(word 2,$(subst ., ,$(BP_VERSION_MINI)))
MINI_BP_VER_DEFS := -DBP_VERSION_MAJOR=$(BP_VERSION_MINI_MAJOR) -DBP_VERSION_MINOR=$(BP_VERSION_MINI_MINOR)

COREDIR      = src/core
BOLTLZDIR    = src/core
FRONTEND ?= linux
FRONTDIR    = src/frontend/$(FRONTEND)
BUILDDIR    = build/$(FRONTEND)

# ---- network send/receive (POSIX linux frontend only) ------------------
# ENABLE_NET=1 (default) compiles the local-network send/receive feature
# into the standard linux build as a single binary.  Only the linux
# frontend ships bp_net.c; other frontends and mini/16-bit builds never
# define BOLTPACK_NET.
ENABLE_NET   ?= 1
NET_CPPFLAGS =
NET_OBJS     =
ifeq ($(FRONTEND),linux)
ifeq ($(ENABLE_NET),1)
NET_CPPFLAGS = -DBOLTPACK_NET
NET_OBJS     = $(BUILDDIR)/frontend/bp_net.o
endif
endif

WIN32_BUILDDIR = build/win32
WIN64_BUILDDIR = build/win64
TOTALCMD_PLUGIN_README = packaging/totalcmd-plugin/README.txt

CORE_OBJS  = $(BUILDDIR)/core/archiver.o \
			 $(BUILDDIR)/core/fluff.o \
			 $(FAST_FLUFF_CORE_OBJ) \
             $(BUILDDIR)/core/compress.o \
             $(BUILDDIR)/core/boltlite.o \
             $(BUILDDIR)/core/bp_codepage.o

FRONT_OBJS = $(BUILDDIR)/frontend/main.o $(NET_OBJS)

OBJS       = $(CORE_OBJS) $(FRONT_OBJS) $(BOLTLZ_OBJS) $(ZSTD_OBJS)
TARGET     = $(BUILDDIR)/boltpack

# Run all recipe lines for a target in a single shell invocation.
# This makes 'exit 0' in the check_cc guard stop the entire recipe,
# rather than only the current recipe line.
.ONESHELL:
.SHELLFLAGS := -e -c

# Configuration stamp: compiler flags and feature toggles are not ordinary
# make prerequisites, so record them explicitly.  This prevents `make modern`
# from reusing objects compiled without the fast-fluff or zstd defines.
.PHONY: FORCE_CONFIG
FORCE_CONFIG:

$(BUILDDIR)/.config: FORCE_CONFIG | $(BUILDDIR)
	@printf '%s\n' \
	    'CC=$(CC)' 'CFLAGS=$(CFLAGS)' 'LDFLAGS=$(LDFLAGS)' \
	    'ENABLE_FAST_FLUFF=$(ENABLE_FAST_FLUFF)' 'ENABLE_ZSTD=$(ENABLE_ZSTD)' \
	    'ENABLE_NET=$(ENABLE_NET)' \
	    'FAST_FLUFF_CPPFLAGS=$(FAST_FLUFF_CPPFLAGS)' \
	    'FAST_FLUFF_LDFLAGS=$(FAST_FLUFF_LDFLAGS)' \
	    'FLUFF_NATIVE_FLAGS=$(FLUFF_NATIVE_FLAGS)' \
	    'FLUFF_FAST_FLAGS=$(FLUFF_FAST_FLAGS)' \
	    'BOLTLZ_CORE_CFLAGS=$(BOLTLZ_CORE_CFLAGS)' 'ZSTD_CC_FLAGS=$(ZSTD_CC_FLAGS)' \
	    'ZSTD_CPPFLAGS=$(ZSTD_CPPFLAGS)' 'NET_CPPFLAGS=$(NET_CPPFLAGS)' \
	    | cmp -s - $@ || printf '%s\n' \
	    'CC=$(CC)' 'CFLAGS=$(CFLAGS)' 'LDFLAGS=$(LDFLAGS)' \
	    'ENABLE_FAST_FLUFF=$(ENABLE_FAST_FLUFF)' 'ENABLE_ZSTD=$(ENABLE_ZSTD)' \
	    'ENABLE_NET=$(ENABLE_NET)' \
	    'FAST_FLUFF_CPPFLAGS=$(FAST_FLUFF_CPPFLAGS)' \
	    'FAST_FLUFF_LDFLAGS=$(FAST_FLUFF_LDFLAGS)' \
	    'FLUFF_NATIVE_FLAGS=$(FLUFF_NATIVE_FLAGS)' \
	    'FLUFF_FAST_FLAGS=$(FLUFF_FAST_FLAGS)' \
	    'BOLTLZ_CORE_CFLAGS=$(BOLTLZ_CORE_CFLAGS)' 'ZSTD_CC_FLAGS=$(ZSTD_CC_FLAGS)' \
	    'ZSTD_CPPFLAGS=$(ZSTD_CPPFLAGS)' 'NET_CPPFLAGS=$(NET_CPPFLAGS)' > $@

# Guard macro: skip a cross target gracefully if the toolchain cannot compile
# and link a trivial C program.  Catches both "compiler not installed" and
# "compiler present but sysroot/libraries missing" cases.
# Usage: $(call check_cc, CC [FLAGS], target-name)
define check_cc
	@printf 'int main(void){return 0;}\n' | $(1) -x c -o /dev/null - >/dev/null 2>&1 || \
	    { printf '  SKIP  %s (%s not usable)\n' '$(2)' '$(firstword $(1))'; exit 0; }
endef

.PHONY: all clean debug profile modern modern-debug modern-profile install-debug-tools linux-i386 dos-host-test dos-djgpp

# Version stamp: every object that bakes BP_VERSION_MAJOR/MINOR in via
# BP_VER_DEFS (or MINI_BP_VER_DEFS) must depend on this file so that editing
# VERSION / VERSION_MINI forces a rebuild.  The stamp is touched only when
# its checksum changes, so a normal rebuild does not relink anything.
$(BUILDDIR)/.version: VERSION VERSION_MINI | $(BUILDDIR)
	@printf '%s %s\n' "$$(cat VERSION 2>/dev/null)" "$$(cat VERSION_MINI 2>/dev/null)" \
	    | cmp -s - $@ || printf '%s %s\n' "$$(cat VERSION 2>/dev/null)" "$$(cat VERSION_MINI 2>/dev/null)" > $@

all: $(TARGET)

# Build the full DOS frontend with the host toolchain and exercise its
# destination projection, staged extraction, and staged archive-edit paths.
# This is not a DOS target build; it complements, but does not replace, a
# DJGPP/DOSBox run.
dos-host-test:
	$(MAKE) FRONTEND=dos CC=cc ENABLE_NET=0 ENABLE_ZSTD=0 \
	    CFLAGS='$(CFLAGS) -DBP_DOS_TEST_ATTRIBUTES' \
	    BUILDDIR=build/dos-host-test TARGET=build/dos-host-test/boltpack all
	$(MAKE) FRONTEND=linux CC=cc ENABLE_NET=0 ENABLE_ZSTD=0 all
	python3 test/check_dos_frontend_host.py \
	    --boltpack build/dos-host-test/boltpack --linux-boltpack build/linux/boltpack
	python3 test/check_dos_profile.py \
	    --boltpack build/dos-host-test/boltpack --linux-boltpack build/linux/boltpack

# QNX, unidentified/pre-7.3 AIX, HP-UX, IRIX, NeXTSTEP, SCO, and Tru64 builds
# cannot currently guarantee race-safe multi-component traversal. Their
# wrappers refuse real extraction; host builds verify refusal happens before
# output creation and dry-run remains available.
.PHONY: posix-path-extract-refusal-host-test
posix-path-extract-refusal-host-test:
	$(MAKE) FRONTEND=qnx CC=$(CC) ENABLE_NET=0 ENABLE_ZSTD=0 \
	    BUILDDIR=build/qnx-host-test TARGET=build/qnx-host-test/boltpack all
	$(MAKE) FRONTEND=aix CC=$(CC) ENABLE_NET=0 ENABLE_ZSTD=0 \
	    BUILDDIR=build/aix-legacy-host-test TARGET=build/aix-legacy-host-test/boltpack all
	$(MAKE) FRONTEND=hpux CC=$(CC) ENABLE_NET=0 ENABLE_ZSTD=0 \
	    BUILDDIR=build/hpux-host-test TARGET=build/hpux-host-test/boltpack all
	$(MAKE) FRONTEND=irix CC=$(CC) ENABLE_NET=0 ENABLE_ZSTD=0 \
	    BUILDDIR=build/irix-host-test TARGET=build/irix-host-test/boltpack all
	$(MAKE) FRONTEND=nextstep CC=$(CC) ENABLE_NET=0 ENABLE_ZSTD=0 \
	    BUILDDIR=build/nextstep-host-test TARGET=build/nextstep-host-test/boltpack all
	$(MAKE) FRONTEND=sco CC=$(CC) ENABLE_NET=0 ENABLE_ZSTD=0 \
	    BUILDDIR=build/sco-host-test TARGET=build/sco-host-test/boltpack all
	$(MAKE) FRONTEND=tru64 CC=$(CC) ENABLE_NET=0 ENABLE_ZSTD=0 \
	    BUILDDIR=build/tru64-host-test TARGET=build/tru64-host-test/boltpack all
	$(MAKE) FRONTEND=linux CC=$(CC) ENABLE_NET=0 ENABLE_ZSTD=0 \
	    BUILDDIR=build/linux-posix-refusal-host-test \
	    TARGET=build/linux-posix-refusal-host-test/boltpack all
	python3 test/check_qnx_extract_refusal.py \
	    --refusing-boltpack build/qnx-host-test/boltpack \
	    --refusing-boltpack build/aix-legacy-host-test/boltpack \
	    --refusing-boltpack build/hpux-host-test/boltpack \
	    --refusing-boltpack build/irix-host-test/boltpack \
	    --refusing-boltpack build/nextstep-host-test/boltpack \
	    --refusing-boltpack build/sco-host-test/boltpack \
	    --refusing-boltpack build/tru64-host-test/boltpack \
	    --linux-boltpack build/linux-posix-refusal-host-test/boltpack

# Build selected POSIX wrappers against the host libc with Linux-only feature
# branches disabled, then exercise directory-relative extraction. These host
# shims do not replace target-system builds or runtime checks. The AIX shim
# defines _AIX73 to exercise that wrapper's version-gated selection; the z/OS
# wrapper enables APIs available with the z/OS 3.1 June 2024 refresh or later.
POSIX_DIRFD_HOST_DIR = build/posix-dirfd-host
POSIX_DIRFD_FRONTENDS = hurd solaris illumos freebsd netbsd openbsd aix zos
POSIX_DIRFD_HOST_BINS = $(foreach frontend,$(POSIX_DIRFD_FRONTENDS),$(POSIX_DIRFD_HOST_DIR)/$(frontend)/boltpack)

$(POSIX_DIRFD_HOST_DIR)/%/boltpack: Makefile src/frontend/%/main.c src/frontend/linux/main.c \
	    src/frontend/posix/extract_paths.h \
	    $(COREDIR)/archiver.c $(COREDIR)/compress.c $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c $(COREDIR)/bp_codepage.c $(BOLTLZDIR)/boltlz.c
	mkdir -p $(@D)
	$(CC) -U__linux__ $(if $(filter aix,$*),-D_AIX73,$(if $(filter zos,$*),,-DBP_POSIX_USE_DIRFD_EXTRACTION=1)) \
	    -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 -fno-lto \
	    -DBP_VERSION_MAJOR=$(BP_VERSION_MAJOR) -DBP_VERSION_MINOR=$(BP_VERSION_MINOR) \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c $(COREDIR)/compress.c $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c $(COREDIR)/bp_codepage.c \
	    src/frontend/$*/main.c $(BOLTLZDIR)/boltlz.c -o $@

.PHONY: posix-dirfd-host-test hurd-host-test solaris-host-test illumos-host-test
posix-dirfd-host-test: $(POSIX_DIRFD_HOST_BINS)
	@set -e; for frontend in $(POSIX_DIRFD_FRONTENDS); do \
	    binary="$(POSIX_DIRFD_HOST_DIR)/$$frontend/boltpack"; \
	    python3 test/check_extract_race.py --boltpack "$$binary"; \
	    python3 test/check_symlink_atomic.py --boltpack "$$binary" --cc "$(CC)"; \
	done

.PHONY: source-race-test
source-race-test: modern
	python3 test/check_source_race.py --boltpack build/linux/boltpack \
	    --parallel-boltpack build/linux/boltpack

hurd-host-test: $(POSIX_DIRFD_HOST_DIR)/hurd/boltpack
	python3 test/check_extract_race.py --boltpack $<
	python3 test/check_symlink_atomic.py --boltpack $< --cc "$(CC)"

solaris-host-test: $(POSIX_DIRFD_HOST_DIR)/solaris/boltpack
	python3 test/check_extract_race.py --boltpack $<
	python3 test/check_symlink_atomic.py --boltpack $< --cc "$(CC)"

illumos-host-test: $(POSIX_DIRFD_HOST_DIR)/illumos/boltpack
	python3 test/check_extract_race.py --boltpack $<
	python3 test/check_symlink_atomic.py --boltpack $< --cc "$(CC)"

# Full DOS command-line build using the DJGPP cross-toolchain.
# DJGPP hides its POSIX/DOS declarations when GCC sets __STRICT_ANSI__;
# keep ANSI language mode but expose those declarations to the frontend.
DOS_CC ?= $(or $(wildcard /opt/djgpp/cross/bin/i586-pc-msdosdjgpp-gcc),i586-pc-msdosdjgpp-gcc)
dos-djgpp:
	$(call check_cc,$(DOS_CC),full DOS DJGPP)
	$(MAKE) FRONTEND=dos CC=$(DOS_CC) ENABLE_NET=0 ENABLE_ZSTD=0 \
	    CFLAGS='$(CFLAGS) -U__STRICT_ANSI__' \
	    BUILDDIR=build/dos-djgpp TARGET=build/dos-djgpp/boltpack.exe all

# 32-bit Linux CLI. Keep its artifacts separate from the native Linux build so
# aggregate builds cannot overwrite build/linux/boltpack with a 32-bit binary.
linux-i386:
	mkdir -p build/linux-x86
	@if ! printf 'int main(void){return 0;}\n' | gcc -m32 -x c -o /dev/null - >/dev/null 2>&1; then \
	    rm -f build/linux-x86/boltpack; \
	    echo '  SKIP  linux-i386 (gcc -m32 cannot compile and link a test program)'; \
	    exit 0; \
	fi
	$(MAKE) FRONTEND=linux CC=gcc \
	    CFLAGS='$(CFLAGS) -m32' LDFLAGS='$(LDFLAGS) -m32' \
	    BOLTLZ_CORE_CFLAGS='$(BOLTLZ_CORE_CFLAGS) -m32' \
	    ZSTD_CC_FLAGS='$(ZSTD_CC_FLAGS) -m32' FLUFF_NATIVE_FLAGS= \
	    BUILDDIR=build/linux-x86 TARGET=build/linux-x86/boltpack all

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $(FAST_FLUFF_LDFLAGS) -o $@ $(OBJS)

$(BUILDDIR)/core/%.o: $(COREDIR)/%.c $(BUILDDIR)/.version $(BUILDDIR)/.config | $(BUILDDIR)/core
	$(CC) $(CFLAGS) -I$(COREDIR) -c -o $@ $<

$(BUILDDIR)/core/bp_codepage.o: $(COREDIR)/bp_sjis_table.h

# archiver.c uses OpenMP for both parallel Fluff and BoltLZ batch paths.
$(BUILDDIR)/core/archiver.o: $(COREDIR)/archiver.c $(BUILDDIR)/.version $(BUILDDIR)/.config | $(BUILDDIR)/core
	$(CC) $(CFLAGS) $(FAST_FLUFF_CPPFLAGS) $(FAST_FLUFF_LDFLAGS) $(ZSTD_CPPFLAGS) -I$(COREDIR) -c -o $@ $<

# fluff.c is the hot decompression path: compile with -O3 and (on native
# linux/x86-64 builds) -march=native so the compiler can exploit CMOV,
# BMI2, and better instruction scheduling.  The source remains strict C89.
# Cross-compilation targets that do not support -march=native should set
# FLUFF_NATIVE_FLAGS= on the make command line or in their recipe.
FLUFF_NATIVE_FLAGS ?= -march=native
$(BUILDDIR)/core/fluff.o: $(COREDIR)/fluff.c $(BUILDDIR)/.version $(BUILDDIR)/.config | $(BUILDDIR)/core
	$(CC) $(CFLAGS) $(FAST_FLUFF_CPPFLAGS) -O3 $(FLUFF_NATIVE_FLAGS) -I$(COREDIR) -c -o $@ $<

# fluff_fast.c: C11 + OpenMP parallel block compression for modern targets.
# Compiled with -std=c11 (overrides -ansi), -fopenmp, and -march=native so
# the compiler can emit SIMD for the match-length inner loop.
# -DBOLTPACK_FAST_FLUFF activates the batch path in archiver.c.
FLUFF_FAST_FLAGS ?= -std=c11 -O3 $(FLUFF_NATIVE_FLAGS) -fopenmp -DBOLTPACK_FAST_FLUFF
$(BUILDDIR)/core/fluff_fast.o: $(COREDIR)/fluff_fast.c $(BUILDDIR)/.config | $(BUILDDIR)/core
	$(CC) $(FLUFF_FAST_FLAGS) -Wall -Wextra -Wno-unused-function \
	    -I$(COREDIR) -c -o $@ $<

$(BUILDDIR)/frontend/%.o: $(FRONTDIR)/%.c $(BUILDDIR)/.version $(BUILDDIR)/.config | $(BUILDDIR)/frontend
	$(CC) $(CFLAGS) $(FAST_FLUFF_CPPFLAGS) $(FAST_FLUFF_LDFLAGS) $(ZSTD_CPPFLAGS) $(NET_CPPFLAGS) -I$(COREDIR) -c -o $@ $<

$(BUILDDIR)/frontend/main.o: src/frontend/posix/extract_paths.h \
	                          src/frontend/posix/archive_preflight.h

# boltlz vendored codec: compiled with warnings suppressed (lib code, strict
# C89 port kept in src/core; the C89 port already suppresses the fallthrough
# case labels via the -w flag below so no -Wimplicit-fallthrough noise).
$(BUILDDIR)/boltlz/boltlz.o: $(BOLTLZDIR)/boltlz.c $(BUILDDIR)/.config | $(BUILDDIR)/boltlz
	$(CC) $(BOLTLZ_CORE_CFLAGS) -I$(BOLTLZDIR) -c -o $@ $<

# zstd amalgamation: compiled as C99 (lib/zstd is exempt from the C89 rule).
$(BUILDDIR)/zstd/zstd.o: $(ZSTDDIR)/zstd.c $(BUILDDIR)/.config | $(BUILDDIR)/zstd
	$(CC) $(ZSTD_CC_FLAGS) -I$(ZSTDDIR) -c -o $@ $<

# zstd C89-compatible wrapper (compiled as C99 to include lib/zstd/zstd.h).
$(BUILDDIR)/core/zstd_wrap.o: $(COREDIR)/zstd_wrap.c $(BUILDDIR)/.version $(BUILDDIR)/.config | $(BUILDDIR)/core
	$(CC) $(ZSTD_CC_FLAGS) $(ZSTD_CPPFLAGS) -I$(COREDIR) -I$(ZSTDDIR) -c -o $@ $<

$(BUILDDIR)/core $(BUILDDIR)/frontend $(BUILDDIR)/boltlz $(BUILDDIR)/zstd:
	mkdir -p $@

clean:
	rm -rf build/

debug:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(DEBUG_CFLAGS)' LDFLAGS='$(DEBUG_LDFLAGS)' all

profile:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(PROFILE_CFLAGS)' all

modern:
	$(MAKE) ENABLE_FAST_FLUFF=1 all

modern-debug:
	$(MAKE) ENABLE_FAST_FLUFF=1 CFLAGS='$(DEBUG_CFLAGS)' LDFLAGS='$(DEBUG_LDFLAGS)' all

modern-profile:
	$(MAKE) ENABLE_FAST_FLUFF=1 CFLAGS='$(PROFILE_CFLAGS)' all

install-debug-tools:
	.venv/bin/python -m pip install -r requirements-debug.txt

# ---- Fluff Optimization Tiers -----------------------------------------------
# Tier detection is automatic at compile time via ULONG_MAX macro.
# Use these targets to build and test specific fluff optimization tiers.
# Each tier gets its own build directory to avoid mixing object files.

.PHONY: tier2 tier3 tier4 all-tiers

# Tier 3: 32-bit native (x86, x86 32-bit mode, ARM 32-bit, etc.)
# 32-bit builds on 64-bit systems use -m32, which forces 32-bit Tier 3.
tier3:
	mkdir -p build/linux-tier3/core build/linux-tier3/frontend build/linux-tier3/boltlz build/linux
	$(CC) $(CFLAGS) -m32 -I$(COREDIR) -c -o build/linux-tier3/core/archiver.o $(COREDIR)/archiver.c
	$(CC) $(CFLAGS) -m32 -I$(COREDIR) -c -o build/linux-tier3/core/fluff.o $(COREDIR)/fluff.c
	$(CC) $(CFLAGS) -m32 -I$(COREDIR) -c -o build/linux-tier3/core/compress.o $(COREDIR)/compress.c
	$(CC) $(CFLAGS) -m32 -I$(COREDIR) -c -o build/linux-tier3/core/boltlite.o $(COREDIR)/boltlite.c
	$(CC) $(CFLAGS) -m32 -I$(COREDIR) -c -o build/linux-tier3/core/bp_codepage.o $(COREDIR)/bp_codepage.c
	$(CC) $(CFLAGS) -m32 -DBOLTPACK_NET -I$(COREDIR) -c -o build/linux-tier3/frontend/main.o src/frontend/linux/main.c
	$(CC) $(CFLAGS) -m32 -DBOLTPACK_NET -I$(COREDIR) -c -o build/linux-tier3/frontend/bp_net.o src/frontend/linux/bp_net.c
	$(CC) $(CFLAGS) -m32 -Wall -O2 -c -o build/linux-tier3/boltlz/boltlz.o $(BOLTLZDIR)/boltlz.c
	$(CC) -m32 -o build/linux/boltpack-tier3 \
		build/linux-tier3/core/archiver.o build/linux-tier3/core/fluff.o \
		build/linux-tier3/core/compress.o build/linux-tier3/core/boltlite.o build/linux-tier3/core/bp_codepage.o \
		build/linux-tier3/frontend/main.o build/linux-tier3/frontend/bp_net.o build/linux-tier3/boltlz/boltlz.o
	@echo "  OK    build/linux/boltpack-tier3 [Tier 3: 32-bit fluff]"

# Tier 4: 64-bit native (x86-64, ARM64, RISC-V 64, PPC64, etc.)
# 64-bit builds use the system's native word size and ULONG_MAX > 0xFFFFFFFFUL.
tier4:
	mkdir -p build/linux-tier4/core build/linux-tier4/frontend build/linux-tier4/boltlz build/linux
	$(CC) $(CFLAGS) -I$(COREDIR) -c -o build/linux-tier4/core/archiver.o $(COREDIR)/archiver.c
	$(CC) $(CFLAGS) -I$(COREDIR) -c -o build/linux-tier4/core/fluff.o $(COREDIR)/fluff.c
	$(CC) $(CFLAGS) -I$(COREDIR) -c -o build/linux-tier4/core/compress.o $(COREDIR)/compress.c
	$(CC) $(CFLAGS) -I$(COREDIR) -c -o build/linux-tier4/core/boltlite.o $(COREDIR)/boltlite.c
	$(CC) $(CFLAGS) -I$(COREDIR) -c -o build/linux-tier4/core/bp_codepage.o $(COREDIR)/bp_codepage.c
	$(CC) $(CFLAGS) -DBOLTPACK_NET -I$(COREDIR) -c -o build/linux-tier4/frontend/main.o src/frontend/linux/main.c
	$(CC) $(CFLAGS) -DBOLTPACK_NET -I$(COREDIR) -c -o build/linux-tier4/frontend/bp_net.o src/frontend/linux/bp_net.c
	$(CC) $(CFLAGS) -Wall -O2 -c -o build/linux-tier4/boltlz/boltlz.o $(BOLTLZDIR)/boltlz.c
	$(CC) $(LDFLAGS) -o build/linux/boltpack-tier4 \
		build/linux-tier4/core/archiver.o build/linux-tier4/core/fluff.o \
		build/linux-tier4/core/compress.o build/linux-tier4/core/boltlite.o build/linux-tier4/core/bp_codepage.o \
		build/linux-tier4/frontend/main.o build/linux-tier4/frontend/bp_net.o build/linux-tier4/boltlz/boltlz.o
	@echo "  OK    build/linux/boltpack-tier4 [Tier 4: 64-bit fluff]"

# Tier 2: Big-endian or unaligned-unfriendly systems (MIPS, PPC BE, etc.)
# 32-bit build with explicit big-endian flags if available.
tier2:
	@echo "  WARN  Tier 2 is for cross-compile targets (build.sh arm-be, etc.)"
	@echo "        Use make tier3 or make tier4 for native builds"

# Build all native tiers on the current system for comparison.
all-tiers: tier3 tier4
	@echo ""
	@echo "Fluff tiers built:"
	@ls -lh build/linux/boltpack-tier* 2>/dev/null | awk '{print "  - " $$9 " (" $$5 ")"}'
	@echo ""
	@echo "To benchmark:"
	@echo "  python3 test/bench_corpus.py       (benchmarks all tiers)"

WCX_SRCDIR = src/frontend/win_wcx
WCX_FLAGS  = -Wall -Wextra -Wno-unused-function -O2 -I$(COREDIR) -I$(BOLTLZDIR)
WCX_SRCS   = $(WCX_SRCDIR)/boltpack_wcx.c \
             $(COREDIR)/archiver.c \
             $(COREDIR)/compress.c \
             $(COREDIR)/boltlite.c \
             $(COREDIR)/fluff.c \
             $(COREDIR)/bp_codepage.c \
			 $(BOLTLZDIR)/boltlz.c
WCX_RC     = $(WCX_SRCDIR)/boltpack_wcx.rc
WCX_ZSTD_CPPFLAGS = -DBOLTPACK_ZSTD -I$(ZSTDDIR)

WIN_SRCDIR = src/frontend/win
WIN_FLAGS  = -Wall -Wextra -Wno-unused-function -O2 -D_WIN32_WINNT=0x0600 -DWINVER=0x0600 -I$(COREDIR) -I$(BOLTLZDIR)
WIN_SRCS   = $(WIN_SRCDIR)/main.c \
             $(WIN_SRCDIR)/bp_net_win.c \
             $(COREDIR)/archiver.c \
             $(COREDIR)/compress.c \
             $(COREDIR)/boltlite.c \
             $(COREDIR)/fluff.c \
             $(COREDIR)/bp_codepage.c \
             $(BOLTLZDIR)/boltlz.c
WIN_RC     = $(WIN_SRCDIR)/boltpack.rc
# LAN send/receive (WinSock2).  bp_net_win.c is #ifdef BOLTPACK_NET-guarded,
# so it is a near-empty translation unit when the flag is absent.
WIN_NET_CPPFLAGS = -DBOLTPACK_NET
WIN_NET_LIBS     = -lws2_32 -liphlpapi
WIN_FAST_FLUFF_CPPFLAGS = -DBOLTPACK_FAST_FLUFF
# GCC appends its implicit OpenMP runtime after the source list; the final
# linker state on each executable target must therefore also be static.
WIN_FAST_FLUFF_LDFLAGS  = -fopenmp -static-libgcc -Wl,-Bstatic,-lgomp,-lwinpthread -Wl,-Bdynamic
WIN_FAST_FLUFF_CFLAGS   = -Wall -Wextra -Wno-unused-function -O3 -std=c11 \
	$(WIN_FAST_FLUFF_CPPFLAGS) $(WIN_FAST_FLUFF_LDFLAGS) -I$(COREDIR)

.PHONY: win32 win64 win32-wcx win64-wcx win64-wcx-wine-test win32-config win64-config win32-exe win64-exe
.PHONY: win32-totalcmd-plugin win64-totalcmd-plugin

win32: win32-wcx win32-config win32-exe
	@echo "Windows 32-bit build complete: build/win32/"

win64: win64-wcx win64-config win64-exe
	@echo "Windows 64-bit build complete: build/win64/"

win32-wcx:
	mkdir -p $(WIN32_BUILDDIR)
	i686-w64-mingw32-gcc $(ZSTD_CC_FLAGS) -m32 -I$(ZSTDDIR) \
	    -c $(ZSTDDIR)/zstd.c -o $(WIN32_BUILDDIR)/wcx_zstd.o
	i686-w64-mingw32-gcc $(ZSTD_CC_FLAGS) -m32 $(WCX_ZSTD_CPPFLAGS) -I$(COREDIR) \
	    -c $(COREDIR)/zstd_wrap.c -o $(WIN32_BUILDDIR)/wcx_zstd_wrap.o
	i686-w64-mingw32-gcc $(WCX_FLAGS) $(WCX_ZSTD_CPPFLAGS) -m32 \
	    $(WCX_SRCS) \
	    $(WIN32_BUILDDIR)/wcx_zstd.o $(WIN32_BUILDDIR)/wcx_zstd_wrap.o \
	    -shared -Wl,--kill-at \
	    -o $(WIN32_BUILDDIR)/boltpack.wcx
	@echo "  OK    $(WIN32_BUILDDIR)/boltpack.wcx"

win64-wcx:
	mkdir -p $(WIN64_BUILDDIR)
	x86_64-w64-mingw32-gcc $(ZSTD_CC_FLAGS) -I$(ZSTDDIR) \
	    -c $(ZSTDDIR)/zstd.c -o $(WIN64_BUILDDIR)/wcx_zstd.o
	x86_64-w64-mingw32-gcc $(ZSTD_CC_FLAGS) $(WCX_ZSTD_CPPFLAGS) -I$(COREDIR) \
	    -c $(COREDIR)/zstd_wrap.c -o $(WIN64_BUILDDIR)/wcx_zstd_wrap.o
	x86_64-w64-mingw32-gcc $(WCX_FLAGS) $(WCX_ZSTD_CPPFLAGS) \
	    $(WCX_SRCS) \
	    $(WIN64_BUILDDIR)/wcx_zstd.o $(WIN64_BUILDDIR)/wcx_zstd_wrap.o \
	    -shared -Wl,--kill-at \
	    -o $(WIN64_BUILDDIR)/boltpack.wcx64
	@echo "  OK    $(WIN64_BUILDDIR)/boltpack.wcx64"

# Exercise the exported UTF-16 WCX API with a plugin-host-shaped caller under Wine.
WINE ?= wine64
.PHONY: win64-wcx-wine-test
win64-wcx-wine-test: win64-wcx all
	mkdir -p $(WIN64_BUILDDIR)
	x86_64-w64-mingw32-gcc -Wall -Wextra -Wno-cast-function-type -municode \
	    test/wcx_unicode_wine.c -o $(WIN64_BUILDDIR)/wcx_unicode_wine.exe
	python3 test/check_wcx_unicode_wine.py --wine "$(WINE)" \
	    --runner $(WIN64_BUILDDIR)/wcx_unicode_wine.exe \
	    --wcx $(WIN64_BUILDDIR)/boltpack.wcx64 \
	    --linux-boltpack build/linux/boltpack

win32-config:
	mkdir -p $(WIN32_BUILDDIR)
	i686-w64-mingw32-windres $(WCX_SRCDIR)/boltpack-config.rc \
	    -o $(WIN32_BUILDDIR)/boltpack-config-res.o
	i686-w64-mingw32-gcc -m32 \
	    $(WCX_SRCDIR)/boltpack-config.c \
	    $(WIN32_BUILDDIR)/boltpack-config-res.o \
	    -o $(WIN32_BUILDDIR)/boltpack-config.exe \
	    -luser32 -ladvapi32 -lshell32
	@echo "  OK    $(WIN32_BUILDDIR)/boltpack-config.exe"

win64-config:
	mkdir -p $(WIN64_BUILDDIR)
	x86_64-w64-mingw32-windres $(WCX_SRCDIR)/boltpack-config.rc \
	    -o $(WIN64_BUILDDIR)/boltpack-config-res.o
	x86_64-w64-mingw32-gcc \
	    $(WCX_SRCDIR)/boltpack-config.c \
	    $(WIN64_BUILDDIR)/boltpack-config-res.o \
	    -o $(WIN64_BUILDDIR)/boltpack-config.exe \
	    -luser32 -ladvapi32 -lshell32
	@echo "  OK    $(WIN64_BUILDDIR)/boltpack-config.exe"

win32-exe:
	mkdir -p $(WIN32_BUILDDIR)
	i686-w64-mingw32-windres $(WIN_RC) \
	    -o $(WIN32_BUILDDIR)/boltpack-res.o
	i686-w64-mingw32-gcc $(WIN_FAST_FLUFF_CFLAGS) -m32 \
	    -c $(COREDIR)/fluff_fast.c -o $(WIN32_BUILDDIR)/fluff_fast.o
	i686-w64-mingw32-gcc $(ZSTD_CC_FLAGS) -m32 -I$(ZSTDDIR) \
	    -c $(ZSTDDIR)/zstd.c -o $(WIN32_BUILDDIR)/zstd.o
	i686-w64-mingw32-gcc $(ZSTD_CC_FLAGS) -m32 $(ZSTD_CPPFLAGS) -I$(COREDIR) -I$(ZSTDDIR) \
	    -c $(COREDIR)/zstd_wrap.c -o $(WIN32_BUILDDIR)/zstd_wrap.o
	i686-w64-mingw32-gcc $(WIN_FLAGS) $(WIN_FAST_FLUFF_CPPFLAGS) $(WIN_NET_CPPFLAGS) $(ZSTD_CPPFLAGS) -I$(ZSTDDIR) \
	    $(WIN_FAST_FLUFF_LDFLAGS) -m32 \
	    $(WIN_SRCS) $(WIN32_BUILDDIR)/fluff_fast.o \
	    $(WIN32_BUILDDIR)/zstd.o $(WIN32_BUILDDIR)/zstd_wrap.o \
	    $(WIN32_BUILDDIR)/boltpack-res.o \
	    $(WIN_NET_LIBS) \
	    -lshell32 \
	    -Wl,-Bstatic \
	    -o $(WIN32_BUILDDIR)/boltpack.exe
	@echo "  OK    $(WIN32_BUILDDIR)/boltpack.exe"

win64-exe:
	mkdir -p $(WIN64_BUILDDIR)
	x86_64-w64-mingw32-windres $(WIN_RC) \
	    -o $(WIN64_BUILDDIR)/boltpack-res.o
	x86_64-w64-mingw32-gcc $(WIN_FAST_FLUFF_CFLAGS) \
	    -c $(COREDIR)/fluff_fast.c -o $(WIN64_BUILDDIR)/fluff_fast.o
	x86_64-w64-mingw32-gcc $(ZSTD_CC_FLAGS) -I$(ZSTDDIR) \
	    -c $(ZSTDDIR)/zstd.c -o $(WIN64_BUILDDIR)/zstd.o
	x86_64-w64-mingw32-gcc $(ZSTD_CC_FLAGS) $(ZSTD_CPPFLAGS) -I$(COREDIR) -I$(ZSTDDIR) \
	    -c $(COREDIR)/zstd_wrap.c -o $(WIN64_BUILDDIR)/zstd_wrap.o
	x86_64-w64-mingw32-gcc $(WIN_FLAGS) $(WIN_FAST_FLUFF_CPPFLAGS) $(WIN_NET_CPPFLAGS) $(ZSTD_CPPFLAGS) -I$(ZSTDDIR) \
	    $(WIN_FAST_FLUFF_LDFLAGS) \
	    $(WIN_SRCS) $(WIN64_BUILDDIR)/fluff_fast.o \
	    $(WIN64_BUILDDIR)/zstd.o $(WIN64_BUILDDIR)/zstd_wrap.o \
	    $(WIN64_BUILDDIR)/boltpack-res.o \
	    $(WIN_NET_LIBS) \
	    -lshell32 \
	    -Wl,-Bstatic \
	    -o $(WIN64_BUILDDIR)/boltpack.exe
	@echo "  OK    $(WIN64_BUILDDIR)/boltpack.exe"

win32-totalcmd-plugin: win32-wcx win32-config
	@which zip > /dev/null 2>&1 || { echo "ERROR: zip not found (install zip package)"; exit 1; }
	rm -rf $(WIN32_BUILDDIR)/totalcmd-plugin
	mkdir -p $(WIN32_BUILDDIR)/totalcmd-plugin
	cp $(WIN32_BUILDDIR)/boltpack.wcx $(WIN32_BUILDDIR)/totalcmd-plugin/
	cp $(WIN32_BUILDDIR)/boltpack-config.exe $(WIN32_BUILDDIR)/totalcmd-plugin/
	cp $(TOTALCMD_PLUGIN_README) $(WIN32_BUILDDIR)/totalcmd-plugin/README.txt
	cp LICENSE.md $(WIN32_BUILDDIR)/totalcmd-plugin/LICENSE
	@printf '%s\n' \
	    '[plugininstall]' \
	    'description=BoltPack archive support v$(BP_VERSION) (32-bit)' \
	    'type=wcx' \
	    'file=boltpack.wcx' \
	    'defaultdir=plugins\wcx\boltpack' \
	    'defaultextension=bolt,blt' \
	    > $(WIN32_BUILDDIR)/totalcmd-plugin/pluginst.inf
	rm -f $(WIN32_BUILDDIR)/boltpack-$(BP_VERSION)-totalcmd-plugin-win32.zip
	cd $(WIN32_BUILDDIR)/totalcmd-plugin && zip -rq ../boltpack-$(BP_VERSION)-totalcmd-plugin-win32.zip .
	@echo "  OK    $(WIN32_BUILDDIR)/boltpack-$(BP_VERSION)-totalcmd-plugin-win32.zip"

win64-totalcmd-plugin: win64-wcx win64-config
	@which zip > /dev/null 2>&1 || { echo "ERROR: zip not found (install zip package)"; exit 1; }
	rm -rf $(WIN64_BUILDDIR)/totalcmd-plugin
	mkdir -p $(WIN64_BUILDDIR)/totalcmd-plugin
	cp $(WIN64_BUILDDIR)/boltpack.wcx64 $(WIN64_BUILDDIR)/totalcmd-plugin/
	cp $(WIN64_BUILDDIR)/boltpack-config.exe $(WIN64_BUILDDIR)/totalcmd-plugin/
	cp $(TOTALCMD_PLUGIN_README) $(WIN64_BUILDDIR)/totalcmd-plugin/README.txt
	cp LICENSE.md $(WIN64_BUILDDIR)/totalcmd-plugin/LICENSE
	@printf '%s\n' \
	    '[plugininstall]' \
	    'description=BoltPack archive support v$(BP_VERSION) (64-bit)' \
	    'type=wcx' \
	    'file=boltpack.wcx' \
	    'defaultdir=plugins\wcx\boltpack' \
	    'defaultextension=bolt,blt' \
	    > $(WIN64_BUILDDIR)/totalcmd-plugin/pluginst.inf
	rm -f $(WIN64_BUILDDIR)/boltpack-$(BP_VERSION)-totalcmd-plugin-win64.zip
	cd $(WIN64_BUILDDIR)/totalcmd-plugin && zip -rq ../boltpack-$(BP_VERSION)-totalcmd-plugin-win64.zip .
	@echo "  OK    $(WIN64_BUILDDIR)/boltpack-$(BP_VERSION)-totalcmd-plugin-win64.zip"

# WebAssembly (Node/browser via emscripten)
WASM_CC      = emcc
WASM_CFLAGS  = -std=c89 -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 -s WASM=1 -s ENVIRONMENT=node,web -s EXIT_RUNTIME=1
WASM_BUILDDIR = build/wasm

.PHONY: wasm
wasm:
	mkdir -p $(WASM_BUILDDIR)
	$(WASM_CC) $(WASM_CFLAGS) \
		-I$(COREDIR) -I$(BOLTLZDIR) \
		$(COREDIR)/archiver.c \
		$(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
		$(COREDIR)/fluff.c \
		$(COREDIR)/bp_codepage.c \
		src/frontend/linux/main.c \
		$(BOLTLZDIR)/boltlz.c \
		-o $(WASM_BUILDDIR)/boltpack.js

# AmigaOS 2.x/3.x 68k — cross-compile from Linux using bebbo's amiga-gcc.
# Install: https://github.com/bebbo/amiga-gcc
# -noixemul: link against native AmigaDOS clib (no ixemul.library)
# -pedantic is omitted because AmigaOS NDK headers are not strict C89.
# The Amiga package ships both the CLI and the GadTools GUI build.
AMIGA68K_CC      ?= /opt/amiga/bin/m68k-amigaos-gcc
AMIGA68K_CFLAGS  = -noixemul -Wall -Wextra -Wno-unused-function -O2
AMIGA68K_BUILDDIR = build/amiga

.PHONY: amiga amiga-cli amiga68k
amiga: amiga-cli amiga-gui

amiga-cli: amiga68k

amiga68k:
	mkdir -p $(AMIGA68K_BUILDDIR)
	rm -f $(AMIGA68K_BUILDDIR)/BoltPack $(AMIGA68K_BUILDDIR)/boltpack-gui
	$(AMIGA68K_CC) $(AMIGA68K_CFLAGS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/amiga/main.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(AMIGA68K_BUILDDIR)/boltpack
	@echo "  OK    $(AMIGA68K_BUILDDIR)/boltpack"

# Atari TOS/MiNT 68k -- cross-compile from Linux using Rivière's toolchain.
# The Atari package ships both the CLI (.TOS) and GEM GUI (.PRG) builds.
ATARI_CC       = m68k-atari-mint-gcc
ATARI_CFLAGS   = -ansi -Wall -Wextra -Wno-unused-function -O2
ATARI_BUILDDIR = build/atari
ATARI_PATH_TEST_BIN = build/atari-path-test/test_atari_path
HATARI ?= hatari
HCONSOLE ?= /usr/share/hatari/hconsole/hconsole.py
HATARI_TOS ?=
EMUTOS_ROM ?=
FREEMINT_ROOT ?= build/freemint-probe/diskroot

.PHONY: atari atari-cli atari-cli-source-race-test atari-cli-parent-race-test atari-gui-create-test atari-gui-create-fat12-test atari-gui-unicode-refusal-test atari-gui-menu-pointer-test atari-gui-cancel-test atari-gui-active-cancel-test atari-gui-interruption-test atari-gui-stage-substitution-test atari-gui-parent-substitution-test atari-gui-publish-rollback-test atari-gui-recovery-path-test atari-gui-interrupt-after-backup-test atari-gui-stage-recovery-test atari-gui-journal-path-test atari-gui-freemint-cleanup-test atari-gui-media-error-test atari-gui-time-range-test atari-path-test atari-hatari-fat-test atari-amiga-interoperability-test
atari: atari-cli atari-gui

atari-path-test:
	mkdir -p build/atari-path-test
	$(CC) -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 -Isrc/core \
	    test/unit/test_atari_path.c -o $(ATARI_PATH_TEST_BIN)
	$(ATARI_PATH_TEST_BIN)

atari-cli:
	mkdir -p $(ATARI_BUILDDIR)
	rm -f $(ATARI_BUILDDIR)/boltpack $(ATARI_BUILDDIR)/boltpack.ttp
	$(ATARI_CC) $(ATARI_CFLAGS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/atari/main.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(ATARI_BUILDDIR)/boltpack.tos
	@echo "  OK    $(ATARI_BUILDDIR)/boltpack.tos"

ATARI_SOURCE_RACE_BIN = $(ATARI_BUILDDIR)/boltpack-source-race.tos
atari-cli-source-race-test:
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image"; exit 2; }
	mkdir -p $(ATARI_BUILDDIR)
	$(ATARI_CC) $(ATARI_CFLAGS) -DBP_TEST_PAUSE_AFTER_SOURCE_STAT \
	    -DBP_TEST_PAUSE_AFTER_SOURCE_OPEN \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c $(COREDIR)/compress.c $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c $(COREDIR)/bp_codepage.c \
	    src/frontend/atari/main.c $(BOLTLZDIR)/boltlz.c \
	    -o $(ATARI_SOURCE_RACE_BIN)
	python3 test/check_hatari_source_race.py --tos "$(HATARI_TOS)" \
	    --hatari "$(HATARI)" --cc "$(ATARI_CC)" \
	    --atari-cli $(ATARI_SOURCE_RACE_BIN)

ATARI_PARENT_RACE_BIN = $(ATARI_BUILDDIR)/boltpack-parent-race.tos
atari-cli-parent-race-test: modern
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image"; exit 2; }
	mkdir -p $(ATARI_BUILDDIR)
	$(ATARI_CC) $(ATARI_CFLAGS) -DBP_ATARI_TEST_PAUSE_BEFORE_ROOT_CHECK \
	    -DBP_ATARI_TEST_PAUSE_AFTER_EXTRACT_STAGE \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c $(COREDIR)/compress.c $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c $(COREDIR)/bp_codepage.c \
	    src/frontend/atari/main.c $(BOLTLZDIR)/boltlz.c \
	    -o $(ATARI_PARENT_RACE_BIN)
	python3 test/check_hatari_parent_race.py --tos "$(HATARI_TOS)" \
	    --hatari "$(HATARI)" --cc "$(ATARI_CC)" \
	    --linux-cli $(TARGET) --atari-cli $(ATARI_PARENT_RACE_BIN)

# Run the Atari/Linux FAT12 guest transfer fixture. Supply the TOS ROM path,
# for example: make atari-hatari-fat-test HATARI_TOS=~/images/Atari/tos162se.img
ATARI_HATARI_FAT_RUN_TAG ?= $(shell date +%Y%m%d-%H%M%S)
ATARI_HATARI_FAT_OUTPUT ?= build/atari-hatari-fat-$(ATARI_HATARI_FAT_RUN_TAG)
atari-hatari-fat-test: all atari-cli
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image"; exit 2; }
	python3 test/check_hatari_fat.py --hatari "$(HATARI)" \
	    --hconsole "$(HCONSOLE)" --tos "$(HATARI_TOS)" \
	    --atari-cli $(ATARI_BUILDDIR)/boltpack.tos --linux-cli $(TARGET) \
	    --output "$(ATARI_HATARI_FAT_OUTPUT)"

ATARI_AMIGA_RUN_TAG ?= $(shell date +%Y%m%d-%H%M%S)
ATARI_AMIGA_OUTPUT ?= build/atari-amiga-interoperability-$(ATARI_AMIGA_RUN_TAG)
atari-amiga-interoperability-test: modern atari-cli amiga68k
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image"; exit 2; }
	python3 test/check_atari_amiga.py --hatari "$(HATARI)" \
	    --hconsole "$(HCONSOLE)" --tos "$(HATARI_TOS)" \
	    --rom "$(AMIGA_ROM)" --workbench "$(AMIGA_WORKBENCH)" \
	    --fs-uae "$(FSUAE_BIN)" --xdftool "$(XDFTOOL)" \
	    --atari-cli $(ATARI_BUILDDIR)/boltpack.tos \
	    --amiga-cli build/amiga/boltpack --linux-cli $(TARGET) \
	    --output "$(ATARI_AMIGA_OUTPUT)"

# Classic Mac OS 68k -- cross-compile from Linux using Retro68 (GCC 12).
# Install: https://github.com/autc04/Retro68
# -pedantic omitted: classic Mac Universal Headers are not strict C89.
RETRO68_ROOT      = /opt/retro68/toolchain
MACOS68K_CC       = $(RETRO68_ROOT)/bin/m68k-apple-macos-gcc
MACOS68K_CFLAGS   = -Wall -Wextra -Wno-unused-function -O2 \
                    -I$(RETRO68_ROOT)/multiversal/CIncludes \
                    -Isrc/frontend/macos9/68k/compat \
                    -include src/frontend/macos9/68k/compat/MacCompat.h
MACOS68K_BUILDDIR = build/macos9-68k
MACOS68K_FRONTEND = src/frontend/macos9/68k/Source
# Retro68 executables need the Boost shared libraries provided by the
# development image. Override this directory for other installations.
MACOS68K_BOOST_LIBDIR ?= /opt/boost1_92/lib
MACOS68K_ENV = LD_LIBRARY_PATH=$(MACOS68K_BOOST_LIBDIR)$(if $(LD_LIBRARY_PATH),:$(LD_LIBRARY_PATH))

MACOS68K_REZ      = $(RETRO68_ROOT)/bin/Rez
MACOS68K_RINCLUDES = $(RETRO68_ROOT)/RIncludes
MACOS68K_MULTIVERSAL_RINCLUDES = $(RETRO68_ROOT)/multiversal/RIncludes

.PHONY: macos9-core-sync-test macos9-68k
macos9-core-sync-test:
	python3 scripts/check_macos9_core_sync.py

macos9-68k: macos9-core-sync-test
	mkdir -p $(MACOS68K_BUILDDIR)
	@smoke='$(MACOS68K_BUILDDIR)/.link-smoke'; \
	if ! printf 'int main(void){return 0;}\n' | $(MACOS68K_ENV) $(MACOS68K_CC) -x c -o "$$smoke" - >/dev/null 2>&1; then \
	    rm -f $(MACOS68K_BUILDDIR)/.rsrc/BoltPack; \
	    rm -f "$$smoke" "$$smoke.gdb"; \
	    echo '  SKIP  macos9-68k (Retro68 compiler cannot link a test program)'; \
	    exit 0; \
	fi
	rm -f '$(MACOS68K_BUILDDIR)/.link-smoke' '$(MACOS68K_BUILDDIR)/.link-smoke.gdb'
	$(MACOS68K_ENV) $(MACOS68K_CC) $(MACOS68K_CFLAGS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    $(MACOS68K_FRONTEND)/main.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(MACOS68K_BUILDDIR)/BoltPack
	$(MACOS68K_ENV) $(MACOS68K_REZ) -I $(MACOS68K_RINCLUDES) -I $(MACOS68K_MULTIVERSAL_RINCLUDES) \
	    $(MACOS68K_FRONTEND)/boltpack.r \
	    -o $(MACOS68K_BUILDDIR)/boltpack_gui.rsrc
	python3 scripts/merge_rsrc.py \
	    $(MACOS68K_BUILDDIR)/.rsrc/BoltPack \
	    $(MACOS68K_BUILDDIR)/.rsrc/boltpack_gui.rsrc \
	    $(MACOS68K_BUILDDIR)/.rsrc/BoltPack
	@echo "  OK    $(MACOS68K_BUILDDIR)/BoltPack"

.PHONY: macos9-68k-kit
macos9-68k-kit:
	$(MAKE) CC=$(CC) COREDIR=$(MACOS68K_FRONTEND) \
	    BOLTLZDIR=$(MACOS68K_FRONTEND) \
	    MACOS68K_BUILDDIR=build/macos9-68k-kit macos9-68k

# Classic Mac OS PPC frontend link using Retro68 and the PowerPC import
# libraries. Retro68 emits XCOFF here, not a runnable Mac OS 9 PEF application.
MACOS9_PPC_CC       ?= $(RETRO68_ROOT)/bin/powerpc-apple-macos-gcc
MACOS9_PPC_LIBDIR   ?= $(RETRO68_ROOT)/universal/libppc
MACOS9_PPC_BUILDDIR ?= build/macos9-ppc

.PHONY: macos9-ppc-xcoff
macos9-ppc-xcoff:
	mkdir -p $(MACOS9_PPC_BUILDDIR)
	$(MACOS68K_ENV) $(MACOS9_PPC_CC) -O2 \
	    -I$(RETRO68_ROOT)/universal/CIncludes \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/macos9/main.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -Wl,--start-group $(MACOS9_PPC_LIBDIR)/*.a -Wl,--end-group \
	    -o $(MACOS9_PPC_BUILDDIR)/BoltPack.xcoff

# macOS (via OSXCross arm64-apple-macosx11-clang)
MACOSX_CC      = arm64-apple-macosx11-clang
MACOSX_CFLAGS  = -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2
MACOSX_BUILDDIR = build/macosx

.PHONY: macosx
macosx:
	mkdir -p $(MACOSX_BUILDDIR)
	$(MACOSX_CC) $(MACOSX_CFLAGS) -DBOLTPACK_NET \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/macosx/main.c \
	    src/frontend/linux/bp_net.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -framework CoreFoundation \
	    -o $(MACOSX_BUILDDIR)/boltpack

# Host lifecycle fixture for the POSIX parts of the macOS CLI. This does not
# claim to exercise Apple xattrs or target filesystem behavior.
.PHONY: macosx-host-test
macosx-host-test: modern
	python3 test/check_macosx_host.py --cc "$(CC)" \
	    --linux-boltpack build/linux/boltpack \
	    --output-root build/macosx-host/runs \
	    --manifest test/reference/interoperability/macosx-host.json

# Haiku / BeOS (x86_64)
HAIKU_CC      = x86_64-unknown-haiku-gcc
HAIKU_CFLAGS  = -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2
HAIKU_BUILDDIR = build/haiku

.PHONY: haiku
haiku:
	$(call check_cc,$(HAIKU_CC),Haiku)
	mkdir -p $(HAIKU_BUILDDIR)
	$(HAIKU_CC) $(HAIKU_CFLAGS) -DBOLTPACK_NET \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/haiku/main.c \
	    src/frontend/linux/bp_net.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(HAIKU_BUILDDIR)/boltpack

# RISC OS (ARM via GCCSDK)
RISCOS_CC      = arm-unknown-riscos-gcc
RISCOS_CFLAGS  = -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 \
	-DBP_CREATOR_OS=BP_OS_RISCOS -DBP_CREATOR_CPU=BP_CPU_ARM32
RISCOS_BUILDDIR = build/riscos

.PHONY: riscos
riscos:
	$(call check_cc,$(RISCOS_CC) $(RISCOS_CFLAGS),riscos)
	mkdir -p $(RISCOS_BUILDDIR)
	$(RISCOS_CC) $(RISCOS_CFLAGS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/riscos/main.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(RISCOS_BUILDDIR)/boltpack,ff8

# ARM Linux (32-bit)
ARM_CC      = arm-linux-gnueabihf-gcc
ARM_CFLAGS  = -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2
ARM_BUILDDIR = build/linux-arm32

.PHONY: arm
arm:
	$(call check_cc,$(ARM_CC) $(ARM_CFLAGS),arm)
	mkdir -p $(ARM_BUILDDIR)
	$(ARM_CC) $(ARM_CFLAGS) -DBOLTPACK_NET \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/linux/main.c \
	    src/frontend/linux/bp_net.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(ARM_BUILDDIR)/boltpack

# ARM64 Linux
ARM64_CC      = aarch64-linux-gnu-gcc
ARM64_CFLAGS  = -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2
ARM64_BUILDDIR = build/linux-arm64
ARM64_FAST_FLUFF_CPPFLAGS = -DBOLTPACK_FAST_FLUFF
ARM64_FAST_FLUFF_LDFLAGS  = -fopenmp
ARM64_FAST_FLUFF_CFLAGS   = -Wall -Wextra -Wno-unused-function -O3 -std=c11 \
	$(ARM64_FAST_FLUFF_CPPFLAGS) $(ARM64_FAST_FLUFF_LDFLAGS) -I$(COREDIR)
ARM64_CORE_OBJS = \
	$(ARM64_BUILDDIR)/archiver.o \
	$(ARM64_BUILDDIR)/compress.o \
	$(ARM64_BUILDDIR)/boltlite.o \
	$(ARM64_BUILDDIR)/fluff.o \
	$(ARM64_BUILDDIR)/fluff_fast.o \
	$(ARM64_BUILDDIR)/bp_codepage.o \
	$(ARM64_BUILDDIR)/main.o \
	$(ARM64_BUILDDIR)/bp_net.o \
	$(ARM64_BUILDDIR)/boltlz.o \
	$(ARM64_BUILDDIR)/zstd.o \
	$(ARM64_BUILDDIR)/zstd_wrap.o

.PHONY: arm64
arm64:
	mkdir -p $(ARM64_BUILDDIR)
	$(ARM64_CC) $(ARM64_CFLAGS) $(ARM64_FAST_FLUFF_CPPFLAGS) $(ARM64_FAST_FLUFF_LDFLAGS) -DBOLTPACK_ZSTD \
	    -I$(COREDIR) -I$(BOLTLZDIR) -c $(COREDIR)/archiver.c \
	    -o $(ARM64_BUILDDIR)/archiver.o
	$(ARM64_CC) $(ARM64_CFLAGS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) -c $(COREDIR)/compress.c \
	    -o $(ARM64_BUILDDIR)/compress.o
	$(ARM64_CC) $(ARM64_CFLAGS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) -c $(COREDIR)/boltlite.c \
	    -o $(ARM64_BUILDDIR)/boltlite.o
	$(ARM64_CC) $(ARM64_CFLAGS) $(ARM64_FAST_FLUFF_CPPFLAGS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) -c $(COREDIR)/fluff.c \
	    -o $(ARM64_BUILDDIR)/fluff.o
	$(ARM64_CC) $(ARM64_FAST_FLUFF_CFLAGS) \
	    -c $(COREDIR)/fluff_fast.c -o $(ARM64_BUILDDIR)/fluff_fast.o
	$(ARM64_CC) $(ARM64_CFLAGS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) -c $(COREDIR)/bp_codepage.c \
	    -o $(ARM64_BUILDDIR)/bp_codepage.o
	$(ARM64_CC) $(ARM64_CFLAGS) $(ARM64_FAST_FLUFF_CPPFLAGS) $(ARM64_FAST_FLUFF_LDFLAGS) \
	    -DBOLTPACK_ZSTD -DBOLTPACK_NET -I$(COREDIR) -I$(BOLTLZDIR) -c src/frontend/linux/main.c \
	    -o $(ARM64_BUILDDIR)/main.o
	$(ARM64_CC) $(ARM64_CFLAGS) -DBOLTPACK_NET \
	    -I$(COREDIR) -I$(BOLTLZDIR) -c src/frontend/linux/bp_net.c \
	    -o $(ARM64_BUILDDIR)/bp_net.o
	$(ARM64_CC) $(ARM64_CFLAGS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) -O2 -c $(BOLTLZDIR)/boltlz.c \
	    -o $(ARM64_BUILDDIR)/boltlz.o
	$(ARM64_CC) $(ZSTD_CC_FLAGS) -I$(ZSTDDIR) \
	    -c $(ZSTDDIR)/zstd.c -o $(ARM64_BUILDDIR)/zstd.o
	$(ARM64_CC) $(ZSTD_CC_FLAGS) -DBOLTPACK_ZSTD -I$(COREDIR) -I$(ZSTDDIR) \
	    -c $(COREDIR)/zstd_wrap.c -o $(ARM64_BUILDDIR)/zstd_wrap.o
	$(ARM64_CC) $(ARM64_CFLAGS) $(ARM64_FAST_FLUFF_LDFLAGS) \
	    -o $(ARM64_BUILDDIR)/boltpack $(ARM64_CORE_OBJS)

# AROS (x86_64) via AROS SDK
AROS_CC      = aros-x86_64-gcc
AROS_CFLAGS  = -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2
AROS_BUILDDIR = build/aros



.PHONY: aros
aros:
	$(call check_cc,$(AROS_CC) $(AROS_CFLAGS),aros)
	mkdir -p $(AROS_BUILDDIR)
	$(AROS_CC) $(AROS_CFLAGS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/aros/main.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(AROS_BUILDDIR)/boltpack

# AmigaOS 4.x PPC via adtools
AMIGAOS4_CC      = ppc-amigaos-gcc
AMIGAOS4_CFLAGS  = -athread=native -ansi -Wall -Wextra -Wno-unused-function -O2
AMIGAOS4_BUILDDIR = build/amigaos4

.PHONY: amigaos4
amigaos4:
	$(call check_cc,$(AMIGAOS4_CC) $(AMIGAOS4_CFLAGS),amigaos4)
	mkdir -p $(AMIGAOS4_BUILDDIR)
	$(AMIGAOS4_CC) $(AMIGAOS4_CFLAGS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/amigaos4/main.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(AMIGAOS4_BUILDDIR)/boltpack

# MorphOS (PPC) via MorphOS SDK
MORPHOS_CC      = ppc-morphos-gcc
MORPHOS_CFLAGS  = -noixemul -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2
MORPHOS_BUILDDIR = build/morphos

.PHONY: morphos
morphos:
	$(call check_cc,$(MORPHOS_CC) $(MORPHOS_CFLAGS),morphos)
	mkdir -p $(MORPHOS_BUILDDIR)
	$(MORPHOS_CC) $(MORPHOS_CFLAGS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/morphos/main.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(MORPHOS_BUILDDIR)/boltpack

# FreeBSD
FREEBSD_CC      = clang
FREEBSD_CFLAGS  = -target x86_64-unknown-freebsd14 -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 -Wno-c2y-extensions
FREEBSD_BUILDDIR = build/freebsd

.PHONY: freebsd
freebsd:
	$(call check_cc,$(FREEBSD_CC) $(FREEBSD_CFLAGS),freebsd)
	mkdir -p $(FREEBSD_BUILDDIR)
	$(FREEBSD_CC) $(FREEBSD_CFLAGS) -DBOLTPACK_NET \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/freebsd/main.c \
	    src/frontend/linux/bp_net.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(FREEBSD_BUILDDIR)/boltpack

# NetBSD
NETBSD_CC      = clang
NETBSD_CFLAGS  = -target x86_64--netbsd -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 -Wno-c2y-extensions
NETBSD_BUILDDIR = build/netbsd

.PHONY: netbsd
netbsd:
	$(call check_cc,$(NETBSD_CC) $(NETBSD_CFLAGS),netbsd)
	mkdir -p $(NETBSD_BUILDDIR)
	$(NETBSD_CC) $(NETBSD_CFLAGS) -DBOLTPACK_NET \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/netbsd/main.c \
	    src/frontend/linux/bp_net.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(NETBSD_BUILDDIR)/boltpack



# RISC-V
RISCV_CC      = riscv64-linux-gnu-gcc
RISCV_CFLAGS  = -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2
RISCV_BUILDDIR = build/riscv

.PHONY: riscv
riscv:
	mkdir -p $(RISCV_BUILDDIR)
	$(RISCV_CC) $(RISCV_CFLAGS) -DBOLTPACK_NET \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/linux/main.c \
	    src/frontend/linux/bp_net.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(RISCV_BUILDDIR)/boltpack

# musl Linux
MUSL_CC=musl-gcc
MUSL_CFLAGS=-static -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 -fno-lto
MUSL_BUILDDIR=build/musl

.PHONY: musl
musl:
	mkdir -p $(MUSL_BUILDDIR)
	$(MUSL_CC) $(MUSL_CFLAGS) -DBOLTPACK_NET \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/linux/main.c \
	    src/frontend/linux/bp_net.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(MUSL_BUILDDIR)/boltpack

# illumos
ILLUMOS_CC=x86_64-unknown-illumos-gcc
ILLUMOS_CFLAGS=-ansi -pedantic -Wall -Wextra -Wno-unused-function -O2
ILLUMOS_BUILDDIR=build/illumos

.PHONY: illumos
illumos:
	$(call check_cc,$(ILLUMOS_CC) $(ILLUMOS_CFLAGS),illumos)
	mkdir -p $(ILLUMOS_BUILDDIR)
	$(ILLUMOS_CC) $(ILLUMOS_CFLAGS) -DBOLTPACK_NET \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/illumos/main.c \
	    src/frontend/linux/bp_net.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(ILLUMOS_BUILDDIR)/boltpack

# E2K (Elbrus)
E2K_CC=e2k-unknown-linux-gcc
E2K_CFLAGS=-ansi -pedantic -Wall -Wextra -Wno-unused-function -O2
E2K_BUILDDIR=build/e2k

.PHONY: e2k
e2k:
	$(call check_cc,$(E2K_CC) $(E2K_CFLAGS),e2k)
	mkdir -p $(E2K_BUILDDIR)
	$(E2K_CC) $(E2K_CFLAGS) -DBOLTPACK_NET \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/linux/main.c \
	    src/frontend/linux/bp_net.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(E2K_BUILDDIR)/boltpack

# OS/2
OS2_CC=os2-gcc
OS2_CFLAGS=-ansi -pedantic -Wall -Wextra -Wno-unused-function -O2
OS2_BUILDDIR=build/os2
OS2_WATCOM_BUILDDIR=build/os2-watcom
OS2_WATCOM_EXE=$(OS2_WATCOM_BUILDDIR)/boltpack.exe
OS2_WATCOM_FLAGS=-bt=os2 -w4
OS2_WATCOM_INCS=-i=$(WATCOM_ROOT)/h/os2 -i=$(COREDIR) -i=$(BOLTLZDIR)

.PHONY: os2 os2-watcom
os2:
	$(call check_cc,$(OS2_CC) $(OS2_CFLAGS),os2)
	mkdir -p $(OS2_BUILDDIR)
	$(OS2_CC) $(OS2_CFLAGS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/os2/main.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(OS2_BUILDDIR)/boltpack.exe

# Open Watcom 32-bit OS/2 cross-build. The Watcom OS/2 SDK headers reject
# strict -za mode because they declare USHORT bitfields, so use the compiler's
# default ANSI-compatible mode for this target.
os2-watcom:
	mkdir -p $(OS2_WATCOM_BUILDDIR)
	$(MINI_ENV) $(WATCOM_BIN)/wcc386 $(OS2_WATCOM_FLAGS) $(OS2_WATCOM_INCS) -fo=$(OS2_WATCOM_BUILDDIR)/archiver.obj $(COREDIR)/archiver.c
	$(MINI_ENV) $(WATCOM_BIN)/wcc386 $(OS2_WATCOM_FLAGS) $(OS2_WATCOM_INCS) -fo=$(OS2_WATCOM_BUILDDIR)/compress.obj $(COREDIR)/compress.c
	$(MINI_ENV) $(WATCOM_BIN)/wcc386 $(OS2_WATCOM_FLAGS) $(OS2_WATCOM_INCS) -fo=$(OS2_WATCOM_BUILDDIR)/boltlite.obj $(COREDIR)/boltlite.c
	$(MINI_ENV) $(WATCOM_BIN)/wcc386 $(OS2_WATCOM_FLAGS) $(OS2_WATCOM_INCS) -fo=$(OS2_WATCOM_BUILDDIR)/fluff.obj $(COREDIR)/fluff.c
	$(MINI_ENV) $(WATCOM_BIN)/wcc386 $(OS2_WATCOM_FLAGS) $(OS2_WATCOM_INCS) -fo=$(OS2_WATCOM_BUILDDIR)/bp_codepage.obj $(COREDIR)/bp_codepage.c
	$(MINI_ENV) $(WATCOM_BIN)/wcc386 $(OS2_WATCOM_FLAGS) $(OS2_WATCOM_INCS) -fo=$(OS2_WATCOM_BUILDDIR)/main.obj src/frontend/os2/main.c
	$(MINI_ENV) $(WATCOM_BIN)/wcc386 $(OS2_WATCOM_FLAGS) $(OS2_WATCOM_INCS) -fo=$(OS2_WATCOM_BUILDDIR)/boltlz.obj $(BOLTLZDIR)/boltlz.c
	$(MINI_ENV) WLINK_LNK=$(WATCOM_BIN)/wlink.lnk $(WLINK16) system os2v2 option quiet \
	    file {$(OS2_WATCOM_BUILDDIR)/archiver.obj} \
	    file {$(OS2_WATCOM_BUILDDIR)/compress.obj} \
	    file {$(OS2_WATCOM_BUILDDIR)/boltlite.obj} \
	    file {$(OS2_WATCOM_BUILDDIR)/fluff.obj} \
	    file {$(OS2_WATCOM_BUILDDIR)/bp_codepage.obj} \
	    file {$(OS2_WATCOM_BUILDDIR)/main.obj} \
	    file {$(OS2_WATCOM_BUILDDIR)/boltlz.obj} \
	    name $(OS2_WATCOM_EXE)
	@echo "  OK    $(OS2_WATCOM_EXE)"

# Sinclair QL (QDOS / SMSQ/E)
QL_CC ?= qcc
QL_CFLAGS=-O -DBP_CREATOR_OS=BP_OS_QL -DBP_CREATOR_CPU=BP_CPU_M68K
QL_BUILDDIR=build/ql
QL_CC_EXEC=$(shell command -v $(firstword $(QL_CC)) 2>/dev/null)
QL_CC_DIR=$(patsubst %/,%,$(dir $(QL_CC_EXEC)))

.PHONY: ql ql-host-test ql-path-test ql-time-host-test ql-sqlux-qdirect-diagnostic
ql:
	@if [ -z "$(QL_CC_EXEC)" ]; then
	    echo "  SKIP  ql ($(firstword $(QL_CC)) not found)"
	    exit 0
	fi
	mkdir -p $(QL_BUILDDIR)
	printf 'int main(void){return 0;}\n' > $(QL_BUILDDIR)/cc_probe.c
	if ! PATH="$(QL_CC_DIR):$$PATH" $(QL_CC) -o $(QL_BUILDDIR)/cc_probe $(QL_BUILDDIR)/cc_probe.c >/dev/null 2>&1; then
	    echo "  SKIP  ql ($(firstword $(QL_CC)) cannot link with its QDOS runtime)"
	    rm -f $(QL_BUILDDIR)/cc_probe.c $(QL_BUILDDIR)/cc_probe
	    exit 0
	fi
	rm -f $(QL_BUILDDIR)/cc_probe.c $(QL_BUILDDIR)/cc_probe
	rm -f $(QL_BUILDDIR)/boltpack
	cd src/frontend/ql
	if ! PATH="$(QL_CC_DIR):$$PATH" $(QL_CC) $(QL_CFLAGS) \
	    -I../../core -I../../../$(BOLTLZDIR) \
	    ../../core/archiver.c \
	    ../../core/compress.c \
	    ../../core/boltlite.c \
	    ../../core/fluff.c \
	    ../../core/bp_codepage.c \
	    main.c \
	    ../../../$(BOLTLZDIR)/boltlz.c \
	    -o ../../../$(QL_BUILDDIR)/boltpack > ../../../$(QL_BUILDDIR)/qcc.log 2>&1; then \
	    cat ../../../$(QL_BUILDDIR)/qcc.log; \
	    exit 1; \
	fi
	cat ../../../$(QL_BUILDDIR)/qcc.log
	@if grep -q 'Undefined Symbols:' ../../../$(QL_BUILDDIR)/qcc.log; then \
	    rm -f ../../../$(QL_BUILDDIR)/boltpack; \
	    echo "  ERROR ql unresolved symbols reported by $(firstword $(QL_CC))"; \
	    exit 1; \
	fi
	test -s ../../../$(QL_BUILDDIR)/boltpack

# Host-build the ANSI-C QL frontend to exercise profile wiring and archive
# interoperability when the QDOS cross-compiler/runtime is unavailable.
QL_HOST_DIR = build/ql-host
QL_HOST_BIN = $(QL_HOST_DIR)/boltpack
ql-host-test:
	mkdir -p $(QL_HOST_DIR)
	$(CC) -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 \
	    -DBP_CREATOR_OS=BP_OS_QL -DBP_CREATOR_CPU=BP_CPU_M68K \
	    -DBP_QL_HOST_TEST \
	    -DBP_VERSION_MAJOR=$(BP_VERSION_MAJOR) -DBP_VERSION_MINOR=$(BP_VERSION_MINOR) \
	    -Isrc/core src/core/archiver.c src/core/compress.c \
	    src/core/boltlite.c src/core/fluff.c src/core/bp_codepage.c \
	    src/frontend/ql/main.c src/core/boltlz.c -o $(QL_HOST_BIN)
	$(CC) -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 \
	    -DBP_VERSION_MAJOR=$(BP_VERSION_MAJOR) -DBP_VERSION_MINOR=$(BP_VERSION_MINOR) \
	    -Isrc/core test/ql_host/create_collision_archive.c src/core/archiver.c \
	    src/core/compress.c src/core/boltlite.c src/core/fluff.c \
	    src/core/bp_codepage.c src/core/boltlz.c \
	    -o $(QL_HOST_DIR)/create_collision_archive
	$(MAKE) CC=$(CC) all linuxmini
	python3 test/check_ql_profile.py $(QL_HOST_BIN) build/linux/boltpack \
	    build/linux32mini/bpmini $(QL_HOST_DIR)/create_collision_archive

# Run the native QDOS binary in sQLux and transfer files to/from Linux.
# Set QL_SQLUX and QL_ROM to the emulator and its Minerva ROM paths.
QL_SQLUX ?= build/ql-sdk/sqlux-build3/sqlux
QL_ROM ?= build/ql-sdk/source/sqlux/roms/Minerva_1.98a1.bin
QL_QDIRECT_OUTPUT ?= build/r07-ql-qdirect-$(shell date +%Y%m%d-%H%M%S)
ql-sqlux-interoperability-test: ql all linuxmini
	python3 test/check_sqlux_interop.py --sqlux "$(QL_SQLUX)" \
	    --rom "$(QL_ROM)" --ql-binary build/ql/boltpack \
	    --linux-binary build/linux/boltpack \
	    --mini-binary build/linux32mini/bpmini

ql-sqlux-qdirect-diagnostic: ql all linuxmini
	@test -n "$(QL_CC_EXEC)" || { echo "Set QL_CC to the working qcc compiler" >&2; exit 2; }
	mkdir -p $(QL_BUILDDIR)
	PATH="$(QL_CC_DIR):$$PATH" $(QL_CC) -o $(QL_BUILDDIR)/qdirect-probe \
	    test/ql_host/qdirect_probe.c
	python3 test/check_sqlux_qdirect.py --sqlux "$(QL_SQLUX)" \
	    --rom "$(QL_ROM)" --probe $(QL_BUILDDIR)/qdirect-probe \
	    --boltpack $(QL_BUILDDIR)/boltpack --linux-boltpack $(TARGET) \
	    --mini-boltpack build/linux32mini/bpmini \
	    --output "$(QL_QDIRECT_OUTPUT)"

ql-path-test:
	mkdir -p build/ql-path-test
	$(CC) -std=c89 -Wall -Wextra -Wno-unused-function \
	    test/unit/test_ql_path.c -o build/ql-path-test/test_ql_path
	build/ql-path-test/test_ql_path

ql-time-host-test:
	mkdir -p build/ql-time-host-test
	$(CC) -std=c89 -pedantic -Wall -Wextra -Wno-unused-function \
	    test/unit/test_ql_time.c -o build/ql-time-host-test/test_ql_time
	build/ql-time-host-test/test_ql_time

# Sharp X68000 (Human68k)
# Toolchain: m68k-x680x-human-gcc (GCC cross-compiler for Human68k).
X68K_CC ?= m68k-x680x-human-gcc
X68K_CFLAGS=-ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 \
	-DBP_CREATOR_OS=BP_OS_X68K -DBP_CREATOR_CPU=BP_CPU_M68K
X68K_BUILDDIR=build/x68k

.PHONY: x68k x68k-host-test
x68k:
	$(call check_cc,$(X68K_CC) $(X68K_CFLAGS) -c,x68k)
	mkdir -p $(X68K_BUILDDIR)
	$(X68K_CC) $(X68K_CFLAGS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/x68k/main.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(X68K_BUILDDIR)/boltpack.x \
	    $(COREDIR)/fluff.c

# Host-build the ANSI-C X68000 CLI with filesystem-call stubs to exercise
# Interchange Profile 1 output where the Human68k toolchain is unavailable.
X68K_HOST_DIR = build/x68k-host
X68K_HOST_BIN = $(X68K_HOST_DIR)/boltpack
X68K_HOST_OUTPUT_ROOT ?= $(X68K_HOST_DIR)/runs
x68k-host-test:
	mkdir -p $(X68K_HOST_DIR)
	$(CC) -D_DEFAULT_SOURCE -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 \
	    -DX68K_HOST_TEST \
	    -DBP_CREATOR_OS=BP_OS_X68K -DBP_CREATOR_CPU=BP_CPU_M68K \
	    -DBP_VERSION_MAJOR=$(BP_VERSION_MAJOR) -DBP_VERSION_MINOR=$(BP_VERSION_MINOR) \
	    -Itest/x68k_host -Isrc/core src/core/archiver.c src/core/compress.c \
	    src/core/boltlite.c src/core/fluff.c src/core/bp_codepage.c \
	    src/frontend/x68k/main.c test/x68k_host/dos_stub.c src/core/boltlz.c \
	    -o $(X68K_HOST_BIN)
	$(CC) -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 -Isrc/core \
	    test/x68k_host/create_collision_archive.c src/core/archiver.c \
	    src/core/compress.c src/core/boltlite.c src/core/boltlz.c \
	    src/core/fluff.c -o $(X68K_HOST_DIR)/create_collision_archive
	$(MAKE) CC=$(CC) all linuxmini
	python3 test/check_x68k_profile.py $(X68K_HOST_BIN) build/linux/boltpack build/linux32mini/bpmini test/reference/interoperability/x68k-host-shim.json $(X68K_HOST_DIR)/create_collision_archive --output-root $(X68K_HOST_OUTPUT_ROOT) --compiler "$(CC)" --version-major $(BP_VERSION_MAJOR) --version-minor $(BP_VERSION_MINOR)

# Build the Plan 9 frontend against host shims for profile CLI testing. Native
# Plan 9 runtime qualification is unavailable on this host.
PLAN9_HOST_DIR = build/plan9-host
PLAN9_HOST_BIN = $(PLAN9_HOST_DIR)/boltpack
.PHONY: plan9-host-test
plan9-host-test:
	mkdir -p $(PLAN9_HOST_DIR)
	$(CC) -std=gnu89 -Wall -Wextra -Wno-unused-function -O2 -Isrc/core \
	    test/plan9_host/test_path_index.c -o $(PLAN9_HOST_DIR)/test_path_index
	$(PLAN9_HOST_DIR)/test_path_index
	$(CC) -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 \
	    -DBP_VERSION_MAJOR=$(BP_VERSION_MAJOR) -DBP_VERSION_MINOR=$(BP_VERSION_MINOR) \
	    -Isrc/core test/plan9_host/create_collision_archive.c \
	    src/core/archiver.c src/core/compress.c src/core/boltlite.c \
	    src/core/fluff.c src/core/bp_codepage.c src/core/boltlz.c \
	    -o $(PLAN9_HOST_DIR)/create_collision_archive
	$(CC) -D_DEFAULT_SOURCE -std=gnu89 -Wno-main -Wall -Wextra \
	    -Wno-unused-function -O2 \
	    -DBP_VERSION_MAJOR=$(BP_VERSION_MAJOR) -DBP_VERSION_MINOR=$(BP_VERSION_MINOR) \
	    -Itest/plan9_host -Isrc/core src/core/archiver.c src/core/compress.c \
	    src/core/boltlite.c src/core/fluff.c src/core/bp_codepage.c \
	    src/frontend/plan9/main.c test/plan9_host/plan9_stubs.c src/core/boltlz.c \
	    -o $(PLAN9_HOST_BIN)
	$(MAKE) CC=$(CC) all linuxmini
	python3 test/check_plan9_profile.py $(PLAN9_HOST_BIN) build/linux/boltpack \
	    build/linux32mini/bpmini $(PLAN9_HOST_DIR)/create_collision_archive \
	    test/reference/interoperability/plan9-host.json

.PHONY: haiku-host-stage-test
haiku-host-stage-test: modern
	python3 test/check_haiku_host.py --cc "$(CC)" \
	    --linux-boltpack build/linux/boltpack

.PHONY: haiku-gui-extract-host-test
haiku-gui-extract-host-test:
	mkdir -p build/haiku-gui-extract-test
	$(CC) -ansi -pedantic -D_DEFAULT_SOURCE -ffunction-sections -fdata-sections \
	    -Isrc/core -c src/core/archiver.c -o build/haiku-gui-extract-test/archiver.o
	$(CC) -ansi -pedantic -D_DEFAULT_SOURCE -ffunction-sections -fdata-sections \
	    -Isrc/core -c src/core/compress.c -o build/haiku-gui-extract-test/compress.o
	$(CC) -ansi -pedantic -D_DEFAULT_SOURCE -ffunction-sections -fdata-sections \
	    -Isrc/core -c src/core/boltlite.c -o build/haiku-gui-extract-test/boltlite.o
	$(CC) -ansi -pedantic -D_DEFAULT_SOURCE -ffunction-sections -fdata-sections \
	    -Isrc/core -c src/core/fluff.c -o build/haiku-gui-extract-test/fluff.o
	$(CC) -ansi -pedantic -D_DEFAULT_SOURCE -ffunction-sections -fdata-sections \
	    -Isrc/core -c src/core/bp_codepage.c -o build/haiku-gui-extract-test/bp_codepage.o
	$(CC) -ansi -pedantic -D_DEFAULT_SOURCE -ffunction-sections -fdata-sections \
	    -Isrc/core -c src/core/boltlz.c -o build/haiku-gui-extract-test/boltlz.o
	$(CXX) -std=c++11 -D_DEFAULT_SOURCE -Wall -Wextra \
	    -Wno-unused-function -ffunction-sections -fdata-sections \
	    -Isrc/core test/unit/test_haiku_gui_extract_helpers.cpp \
	    build/haiku-gui-extract-test/archiver.o \
	    build/haiku-gui-extract-test/compress.o \
	    build/haiku-gui-extract-test/boltlite.o \
	    build/haiku-gui-extract-test/fluff.o \
	    build/haiku-gui-extract-test/bp_codepage.o \
	    build/haiku-gui-extract-test/boltlz.o -Wl,--gc-sections -lm \
	    -o build/haiku-gui-extract-test/test
	build/haiku-gui-extract-test/test

.PHONY: riscos-path-test
riscos-path-test:
	mkdir -p build/riscos-path-test
	$(CC) -std=c89 -Wall -Wextra -Wno-unused-function \
	    test/unit/test_riscos_path.c src/core/bp_codepage.c \
	    -o build/riscos-path-test/test
	build/riscos-path-test/test

.PHONY: riscos-stage-host-test
riscos-stage-host-test:
	mkdir -p build/riscos-stage-test
	$(CC) -m32 -D_DEFAULT_SOURCE -std=c89 -Wall -Wextra \
	    -Wno-unused-function -DBP_VERSION_MAJOR=$(BP_VERSION_MAJOR) \
	    -DBP_VERSION_MINOR=$(BP_VERSION_MINOR) -Itest/riscos_host -Isrc/core \
	    test/unit/test_riscos_staged_extract.c src/core/archiver.c \
	    src/core/compress.c src/core/boltlite.c src/core/fluff.c \
	    src/core/bp_codepage.c src/core/boltlz.c -lm \
	    -o build/riscos-stage-test/test
	build/riscos-stage-test/test

.PHONY: riscos-profile-host-test
riscos-profile-host-test: modern linuxmini
	mkdir -p build/riscos-profile-test
	$(CC) -m32 -D_DEFAULT_SOURCE -std=c89 -Wall -Wextra \
	    -Wno-unused-function -DBP_VERSION_MAJOR=$(BP_VERSION_MAJOR) \
	    -DBP_VERSION_MINOR=$(BP_VERSION_MINOR) \
	    -DBP_CREATOR_OS=BP_OS_RISCOS -DBP_CREATOR_CPU=BP_CPU_ARM32 \
	    -Itest/riscos_host -Isrc/core test/riscos_host/profile_fixture.c \
	    src/core/archiver.c src/core/compress.c src/core/boltlite.c \
	    src/core/fluff.c src/core/bp_codepage.c src/core/boltlz.c -lm \
	    -o build/riscos-profile-test/boltpack-riscos-host
	python3 test/check_riscos_profile.py \
	    --riscos-host build/riscos-profile-test/boltpack-riscos-host \
	    --linux-boltpack build/linux/boltpack \
	    --mini-boltpack build/linux32mini/bpmini


# GNU Hurd (x86 / x86-64). Use a Hurd-targeting cross compiler, never the
# host Linux compiler. Supported triples include i386-gnu and x86_64-pc-gnu.
HURD_CC      ?= x86_64-gnu-gcc
HURD_CFLAGS  = -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 -fno-lto
HURD_BUILDDIR = build/hurd
HURD_TARGET   = $(shell $(HURD_CC) -dumpmachine 2>/dev/null)

.PHONY: hurd
hurd:
	@mkdir -p $(HURD_BUILDDIR)
	@if [ -f $(HURD_BUILDDIR)/boltpack ]; then \
	    osabi=$$(od -An -tu1 -j7 -N1 $(HURD_BUILDDIR)/boltpack 2>/dev/null | tr -d '[:space:]'); \
	    if [ "$$osabi" != 4 ]; then rm -f $(HURD_BUILDDIR)/boltpack; fi; \
	fi
	$(call check_cc,$(HURD_CC) $(HURD_CFLAGS),hurd)
	@target='$(HURD_TARGET)'; \
	case "$$target" in *linux*|*kfreebsd*|*freebsd*|*netbsd*|*openbsd*|*darwin*|*mingw*|*cygwin*) \
	    printf 'ERROR: HURD_CC=%s targets %s, not GNU/Hurd.\n' '$(HURD_CC)' "$$target" >&2; exit 1 ;; \
	esac; \
	case "$$target" in \
	    i?86-gnu*|i?86-*-gnu*|x86_64-gnu*|x86_64-*-gnu*) ;; \
	    *) printf 'ERROR: HURD_CC=%s targets %s; expected an x86 GNU/Hurd compiler.\n' '$(HURD_CC)' "$${target:-<unknown>}" >&2; exit 1 ;; \
	esac
	$(HURD_CC) $(HURD_CFLAGS) -DBOLTPACK_NET \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c \
	    $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c \
	    src/frontend/hurd/main.c \
	    src/frontend/linux/bp_net.c \
	    $(BOLTLZDIR)/boltlz.c \
	    -o $(HURD_BUILDDIR)/boltpack
	@osabi=$$(od -An -tu1 -j7 -N1 $(HURD_BUILDDIR)/boltpack 2>/dev/null | tr -d '[:space:]'); \
	if [ "$$osabi" != 4 ]; then \
	    printf 'ERROR: %s has ELF OSABI %s; expected GNU/Hurd (4).\n' '$(HURD_BUILDDIR)/boltpack' "$${osabi:-<unknown>}" >&2; \
	    rm -f $(HURD_BUILDDIR)/boltpack; exit 1; \
	fi

# ---- Test and fuzz targets --------------------------------------------------

ASAN_TARGET      = build/linux/boltpack_asan

ASAN_CORE_OBJS  = build/linux/core/archiver_asan.o \
                  build/linux/core/fluff_asan.o \
                  build/linux/core/compress_asan.o \
                  build/linux/core/boltlite_asan.o \
                  build/linux/core/bp_codepage_asan.o
ASAN_FRONT_OBJS = build/linux/frontend/main_asan.o
ASAN_BOLTLZ_OBJS = build/linux/boltlz/boltlz_asan.o
ASAN_OBJS       = $(ASAN_CORE_OBJS) $(ASAN_FRONT_OBJS) $(ASAN_BOLTLZ_OBJS)

build/linux/core/%_asan.o: $(COREDIR)/%.c | build/linux/core
	$(CC) $(DEBUG_CFLAGS) -I$(COREDIR) -c -o $@ $<

build/linux/frontend/%_asan.o: $(FRONTDIR)/%.c | build/linux/frontend
	$(CC) $(DEBUG_CFLAGS) -I$(COREDIR) -c -o $@ $<

build/linux/boltlz/%_asan.o: $(BOLTLZDIR)/%.c | build/linux/boltlz
	$(CC) $(DEBUG_CFLAGS) -Wall -c -o $@ $<

$(ASAN_TARGET): $(ASAN_OBJS)
	$(CC) $(DEBUG_LDFLAGS) -o $@ $(ASAN_OBJS)

# ---- Cross-compilation batch target ----------------------------------------
# Builds every platform that has a viable cross-compiler from Linux.
# Targets requiring native builds (OpenBSD, Solaris, IRIX, HP-UX, AIX, macOS,
# NeXTSTEP, QNX, Plan 9, Tru64, SCO, OpenVMS, z/OS, CP/M-86) are excluded.

.PHONY: cross-all
cross-all: all musl arm arm64 riscv \
           win32 win64 \
           win32-fltk-installer win64-fltk-installer \
           wasm \
           amiga68k amigaos4 morphos aros \
           atari macos9-68k \
           haiku \
           riscos \
           freebsd netbsd \
           illumos \
           e2k \
           os2 \
           hurd \
           ql x68k

.PHONY: asan test fuzz

asan: $(ASAN_TARGET)

# Optional cross-runtime checks for environments with 32-bit Linux support,
# QEMU-i386, Open Watcom, and DOSBox. The mini target runs directly by default;
# set LINUXMINI_RUNNER=qemu-i386 where native 32-bit execution is unavailable.
LINUXMINI_RUNNER ?=
.PHONY: linuxmini-interoperability-test dos16-interoperability-test dos16-stack-interoperability-test dosboxx-fat12-interoperability-test dosboxx-fat12-cp850-interoperability-test freedos-cp850-interoperability-test dos-djgpp-dosboxx-fat12-test
linuxmini-interoperability-test: modern linuxmini
	BP_TEST_LINUXMINI_RUNNER="$(LINUXMINI_RUNNER)" python3 test/check_linuxmini_interop.py
	python3 test/check_extract_race.py --boltpack build/linux/boltpack
	BP_TEST_RUNNER="$(LINUXMINI_RUNNER)" python3 test/check_extract_race.py \
	    --boltpack $(LINUXMINI_EXE) --mini

dos16-interoperability-test: modern dos16 unit-test-dos16
	$(BUILDDIR)/boltpack t build/unit-test-dos16/LIMIT.BLT
	python3 test/check_dos16_interop.py

dos16-stack-interoperability-test: modern dos16-stack-probe unit-test-dos16
	BP_DOS16_EXE="$(CURDIR)/$(DOS16_STACK_EXE)" \
	    BP_DOS16_WRITER_PROBE="$(CURDIR)/$(DOS16_STACK_WRITER_EXE)" \
	    BP_DOS16_WRITER_ARCHIVE="$(CURDIR)/$(DOS16_STACK_BDIR)/WRITMAX.BLT" \
	    BP_DOS16_STACK_REPORT=1 \
	    BP_DOS16_STACK_REPORT_FILE="$(CURDIR)/$(DOS16_STACK_BDIR)/resource-low-water.txt" \
	    python3 test/check_dos16_interop.py

dosboxx-fat12-interoperability-test: modern dos16
	python3 test/check_dosboxx_cp437.py

dosboxx-fat12-cp850-interoperability-test: modern dos16-cp850
	python3 test/check_dosboxx_cp850.py

# Set the boot image and the MODE.COM, DISPLAY.EXE, and EGA.CPX files from the
# FreeDOS floppy/base packages to qualify lowercase CP850 names on DOS.
FREEDOS_BOOT_IMAGE ?=
FREEDOS_MODE ?=
FREEDOS_DISPLAY ?=
FREEDOS_EGA_CPX ?=
FREEDOS_OUTPUT ?=
.PHONY: freedos-cp850-interoperability-test
freedos-cp850-interoperability-test: modern dos16-cp850 dos16-cp850-crash-hook
	@test -f "$(FREEDOS_BOOT_IMAGE)" -a -f "$(FREEDOS_MODE)" -a \
	    -f "$(FREEDOS_DISPLAY)" -a -f "$(FREEDOS_EGA_CPX)" || { \
	    echo "Set FREEDOS_BOOT_IMAGE, FREEDOS_MODE, FREEDOS_DISPLAY, and FREEDOS_EGA_CPX" >&2; exit 2; }
	python3 test/check_freedos_cp850.py --boot-image "$(FREEDOS_BOOT_IMAGE)" \
	    --mode "$(FREEDOS_MODE)" --display "$(FREEDOS_DISPLAY)" \
	    --cpx "$(FREEDOS_EGA_CPX)" \
	    --crash-exe "$(CURDIR)/build/dos16-cp850-crash-hook/bpcrash.exe" \
	    $(if $(strip $(FREEDOS_OUTPUT)),--output "$(FREEDOS_OUTPUT)",)

# Set CWSDPMI to CWSDPMI.EXE from the DJGPP csdpmi package to qualify the
# full 32-bit DOS frontend under DOSBox-X, independently from the DOS16 mini.
dos-djgpp-dosboxx-fat12-test: modern dos-djgpp
	@test -n "$(CWSDPMI)" || { echo "Set CWSDPMI=/path/to/CWSDPMI.EXE (DJGPP csdpmi package)" >&2; exit 2; }
	python3 test/check_djgpp_dosboxx.py --cwsdpmi "$(CWSDPMI)" \
	    --dos-cc "$(DOS_CC)"

AMIGA_ROM ?= $(HOME)/images/Amiga/kickstart-2.04-a500plus.rom
AMIGA_WORKBENCH ?= $(HOME)/images/Amiga/Workbench-2.04/1-Workbench.adf
FSUAE_BIN ?= fs-uae
XDFTOOL ?= xdftool
AMIGA_INTERRUPT_SIZE_MIB ?= 64
AMIGA_PUBLISH_TEST_BIN = $(AMIGA68K_BUILDDIR)/boltpack-publish-failure
AMIGA_PUBLISH_PAUSE_TEST_BIN = $(AMIGA68K_BUILDDIR)/boltpack-publish-pause
AMIGA_NATIVE_PUBLISH_PAUSE_BIN = $(AMIGA68K_BUILDDIR)/boltpack-native-publish-pause
AMIGA_SOURCE_PAUSE_TEST_BIN = $(AMIGA68K_BUILDDIR)/boltpack-source-pause
AMIGA_PARENT_SUBSTITUTION_TEST_BIN = $(AMIGA68K_BUILDDIR)/boltpack-parent-substitution

.PHONY: amiga68k-publish-failure-test amiga68k-publish-pause-test amiga68k-native-publish-pause-test amiga68k-source-pause-test amiga68k-parent-substitution-test fsuae-interruption-test fsuae-publish-rollback-test fsuae-publish-interruption-test fsuae-native-publish-recovery-test fsuae-native-interoperability-test fsuae-source-race-test fsuae-source-swap-restore-test fsuae-parent-substitution-test
amiga68k-publish-failure-test:
	mkdir -p $(AMIGA68K_BUILDDIR)
	$(AMIGA68K_CC) $(AMIGA68K_CFLAGS) -DBP_TEST_FAIL_PUBLISH \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c src/frontend/amiga/main.c \
	    $(BOLTLZDIR)/boltlz.c -o $(AMIGA_PUBLISH_TEST_BIN)
	@echo "  OK    $(AMIGA_PUBLISH_TEST_BIN)"

amiga68k-publish-pause-test:
	mkdir -p $(AMIGA68K_BUILDDIR)
	$(AMIGA68K_CC) $(AMIGA68K_CFLAGS) -DBP_TEST_PAUSE_AFTER_BACKUP \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c src/frontend/amiga/main.c \
	    $(BOLTLZDIR)/boltlz.c -o $(AMIGA_PUBLISH_PAUSE_TEST_BIN)
	@echo "  OK    $(AMIGA_PUBLISH_PAUSE_TEST_BIN)"

amiga68k-native-publish-pause-test:
	mkdir -p $(AMIGA68K_BUILDDIR)
	$(AMIGA68K_CC) $(AMIGA68K_CFLAGS) -DBP_TEST_PAUSE_AFTER_BACKUP \
	    -DBP_TEST_PAUSE_MARKER \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c src/frontend/amiga/main.c \
	    $(BOLTLZDIR)/boltlz.c -o $(AMIGA_NATIVE_PUBLISH_PAUSE_BIN)
	@echo "  OK    $(AMIGA_NATIVE_PUBLISH_PAUSE_BIN)"

amiga68k-source-pause-test:
	mkdir -p $(AMIGA68K_BUILDDIR)
	$(AMIGA68K_CC) $(AMIGA68K_CFLAGS) -DBP_TEST_PAUSE_AFTER_SOURCE_OPEN \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c src/frontend/amiga/main.c \
	    $(BOLTLZDIR)/boltlz.c -o $(AMIGA_SOURCE_PAUSE_TEST_BIN)
	@echo "  OK    $(AMIGA_SOURCE_PAUSE_TEST_BIN)"

amiga68k-parent-substitution-test:
	mkdir -p $(AMIGA68K_BUILDDIR)
	$(AMIGA68K_CC) $(AMIGA68K_CFLAGS) -DBP_TEST_PAUSE_AFTER_EXTRACT_STAGE \
	    -I$(COREDIR) -I$(BOLTLZDIR) \
	    $(COREDIR)/archiver.c $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c src/frontend/amiga/main.c \
	    $(BOLTLZDIR)/boltlz.c -o $(AMIGA_PARENT_SUBSTITUTION_TEST_BIN)
	@echo "  OK    $(AMIGA_PARENT_SUBSTITUTION_TEST_BIN)"

fsuae-source-race-test: modern amiga68k-source-pause-test
	python3 test/check_fsuae_interrupt.py --rom "$(AMIGA_ROM)" \
	    --workbench "$(AMIGA_WORKBENCH)" --fs-uae "$(FSUAE_BIN)" \
	    --xdftool "$(XDFTOOL)" --size-mib 64 --operation source-replace \
	    --amiga-cli "$(AMIGA_SOURCE_PAUSE_TEST_BIN)"

AMIGA_SOURCE_SWAP_RESTORE_OUTPUT ?= build/r05-amiga-source-swap-restore-$(shell date +%Y%m%d-%H%M%S)
fsuae-source-swap-restore-test: modern amiga68k-source-pause-test
	python3 test/check_fsuae_interrupt.py --rom "$(AMIGA_ROM)" \
	    --workbench "$(AMIGA_WORKBENCH)" --fs-uae "$(FSUAE_BIN)" \
	    --xdftool "$(XDFTOOL)" --size-mib 1 --operation source-swap-restore \
	    --amiga-cli "$(AMIGA_SOURCE_PAUSE_TEST_BIN)" \
	    --output "$(AMIGA_SOURCE_SWAP_RESTORE_OUTPUT)"

fsuae-parent-substitution-test: modern amiga68k-parent-substitution-test
	python3 test/check_fsuae_interrupt.py --rom "$(AMIGA_ROM)" \
	    --workbench "$(AMIGA_WORKBENCH)" --fs-uae "$(FSUAE_BIN)" \
	    --xdftool "$(XDFTOOL)" --size-mib 1 --operation parent-substitute \
	    --amiga-cli "$(AMIGA_PARENT_SUBSTITUTION_TEST_BIN)"

fsuae-interruption-test: modern amiga68k
	python3 test/check_fsuae_interrupt.py --rom "$(AMIGA_ROM)" \
	    --workbench "$(AMIGA_WORKBENCH)" --fs-uae "$(FSUAE_BIN)" \
	    --xdftool "$(XDFTOOL)" --size-mib "$(AMIGA_INTERRUPT_SIZE_MIB)" \
	    --operation extract
	python3 test/check_fsuae_interrupt.py --rom "$(AMIGA_ROM)" \
	    --workbench "$(AMIGA_WORKBENCH)" --fs-uae "$(FSUAE_BIN)" \
	    --xdftool "$(XDFTOOL)" --size-mib "$(AMIGA_INTERRUPT_SIZE_MIB)" \
	    --operation append

fsuae-publish-rollback-test: modern amiga68k-publish-failure-test
	python3 test/check_fsuae_interrupt.py --rom "$(AMIGA_ROM)" \
	    --workbench "$(AMIGA_WORKBENCH)" --fs-uae "$(FSUAE_BIN)" \
	    --xdftool "$(XDFTOOL)" --size-mib "$(AMIGA_INTERRUPT_SIZE_MIB)" \
	    --amiga-cli "$(AMIGA_PUBLISH_TEST_BIN)" --operation publish-failure

fsuae-publish-interruption-test: modern amiga68k-publish-pause-test amiga68k
	python3 test/check_fsuae_interrupt.py --rom "$(AMIGA_ROM)" \
	    --workbench "$(AMIGA_WORKBENCH)" --fs-uae "$(FSUAE_BIN)" \
	    --xdftool "$(XDFTOOL)" --size-mib "$(AMIGA_INTERRUPT_SIZE_MIB)" \
	    --amiga-cli "$(AMIGA_PUBLISH_PAUSE_TEST_BIN)" \
	    --recovery-cli "build/amiga/boltpack" --operation publish-interrupt

AMIGA_NATIVE_RECOVERY_TAG ?= $(shell date +%Y%m%d-%H%M%S)
AMIGA_NATIVE_RECOVERY_OFS_OUTPUT ?= build/amiga-native-recovery-ofs-$(AMIGA_NATIVE_RECOVERY_TAG)
AMIGA_NATIVE_RECOVERY_FFS_OUTPUT ?= build/amiga-native-recovery-ffs-$(AMIGA_NATIVE_RECOVERY_TAG)
fsuae-native-publish-recovery-test: modern amiga68k-native-publish-pause-test amiga68k
	python3 test/check_fsuae_native_recovery.py --rom "$(AMIGA_ROM)" \
	    --workbench "$(AMIGA_WORKBENCH)" --fs-uae "$(FSUAE_BIN)" \
	    --xdftool "$(XDFTOOL)" --pause-cli "$(AMIGA_NATIVE_PUBLISH_PAUSE_BIN)" \
	    --recovery-cli "build/amiga/boltpack" --filesystem ofs \
	    --output "$(AMIGA_NATIVE_RECOVERY_OFS_OUTPUT)"
	python3 test/check_fsuae_native_recovery.py --rom "$(AMIGA_ROM)" \
	    --workbench "$(AMIGA_WORKBENCH)" --fs-uae "$(FSUAE_BIN)" \
	    --xdftool "$(XDFTOOL)" --pause-cli "$(AMIGA_NATIVE_PUBLISH_PAUSE_BIN)" \
	    --recovery-cli "build/amiga/boltpack" --filesystem ffs \
	    --output "$(AMIGA_NATIVE_RECOVERY_FFS_OUTPUT)"

AMIGA_NATIVE_RUN_TAG ?= $(shell date +%Y%m%d-%H%M%S)
AMIGA_NATIVE_OFS_OUTPUT ?= build/amiga-native-ofs-$(AMIGA_NATIVE_RUN_TAG)
AMIGA_NATIVE_FFS_OUTPUT ?= build/amiga-native-ffs-$(AMIGA_NATIVE_RUN_TAG)
AMIGA_NATIVE_DEPTH ?= 4
fsuae-native-interoperability-test: modern amiga68k
	python3 test/check_fsuae_native.py --rom "$(AMIGA_ROM)" \
	    --workbench "$(AMIGA_WORKBENCH)" --fs-uae "$(FSUAE_BIN)" \
	    --output "$(AMIGA_NATIVE_OFS_OUTPUT)" \
	    --xdftool "$(XDFTOOL)" --filesystem ofs --skip-codecs \
	    --depth "$(AMIGA_NATIVE_DEPTH)" --stack 0
	python3 test/check_fsuae_native.py --rom "$(AMIGA_ROM)" \
	    --workbench "$(AMIGA_WORKBENCH)" --fs-uae "$(FSUAE_BIN)" \
	    --output "$(AMIGA_NATIVE_FFS_OUTPUT)" \
	    --xdftool "$(XDFTOOL)" --filesystem ffs --skip-codecs \
	    --depth "$(AMIGA_NATIVE_DEPTH)" --stack 0

.PHONY: amiga-meta-host-test amiga-gui-dirs-host-test amiga-gui-stage-host-test amiga-gui-gather-host-test amiga-gui-source-name-host-test amiga-gui-name-index-host-test
amiga-meta-host-test:
	mkdir -p build/amiga-meta-test
	$(CC) -std=c89 -pedantic -Wall -Wextra -Wno-unused-function \
	    test/unit/test_amiga_meta.c -o build/amiga-meta-test/test
	build/amiga-meta-test/test

amiga-gui-dirs-host-test:
	mkdir -p build/amiga-gui-dirs-test
	$(CC) -std=c89 -pedantic -Wall -Wextra -Wno-unused-function \
	    test/unit/test_amiga_gui_dirs.c -o build/amiga-gui-dirs-test/test
	build/amiga-gui-dirs-test/test

amiga-gui-stage-host-test:
	mkdir -p build/amiga-gui-stage-test
	$(CC) -std=c89 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-function \
	    test/unit/test_amiga_gui_stage.c -o build/amiga-gui-stage-test/test
	build/amiga-gui-stage-test/test

amiga-gui-gather-host-test:
	mkdir -p build/amiga-gui-gather-test
	$(CC) -std=c89 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-function \
	    test/unit/test_amiga_gui_gather.c -o build/amiga-gui-gather-test/test
	build/amiga-gui-gather-test/test

amiga-gui-source-name-host-test:
	mkdir -p build/amiga-gui-source-name-test
	$(CC) -std=c89 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-function \
	    test/unit/test_amiga_gui_source_name.c src/core/bp_codepage.c \
	    -o build/amiga-gui-source-name-test/test
	build/amiga-gui-source-name-test/test

amiga-gui-name-index-host-test:
	mkdir -p build/amiga-gui-name-index-test
	$(CC) -std=c89 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-function \
	    test/unit/test_amiga_gui_name_index.c src/core/bp_codepage.c \
	    -o build/amiga-gui-name-index-test/test
	build/amiga-gui-name-index-test/test

.PHONY: os2-publish-host-test
os2-publish-host-test:
	mkdir -p build/os2-publish-host-test
	$(CC) -std=c89 -pedantic -Wall -Wextra -Wno-unused-function -Isrc/core \
	    test/unit/test_os2_publish.c -o build/os2-publish-host-test/test
	build/os2-publish-host-test/test

# Compile the full DOS adapter when its DJGPP toolchain is available. Runtime
# coverage remains in the separate DOSBox interoperability target.
test: modern unit-test dos-djgpp linux-cli-extract-failure-test posix-dirfd-host-test posix-path-extract-refusal-host-test atari-path-test ql-path-test ql-time-host-test amiga-meta-host-test amiga-gui-dirs-host-test amiga-gui-stage-host-test amiga-gui-gather-host-test amiga-gui-source-name-host-test amiga-gui-name-index-host-test os2-publish-host-test macos9-core-sync-test riscos-path-test riscos-stage-host-test riscos-profile-host-test haiku-host-stage-test haiku-gui-extract-host-test
	@echo "=== check_boltpack ==="
	python3 test/check_boltpack.py
	@echo "=== source identity and mutation ==="
	python3 test/check_source_race.py --boltpack build/linux/boltpack \
	    --parallel-boltpack build/linux/boltpack
	@echo "=== symlink timestamp and publication ==="
	python3 test/check_symlink_atomic.py --boltpack build/linux/boltpack --cc "$(CC)"
	@echo "=== legacy CLI profile interoperability ==="
	$(MAKE) CC=$(CC) ql-host-test
	$(MAKE) CC=$(CC) x68k-host-test
	$(MAKE) CC=$(CC) plan9-host-test
	$(MAKE) CC=$(CC) macosx-host-test
	$(MAKE) CC=$(CC) dos-host-test
	$(MAKE) CC=$(CC) cpm86-host-test
	@echo "=== All test suites passed ==="

fuzz: $(ASAN_TARGET)
	python3 test/test_fuzz.py --boltpack $(ASAN_TARGET)

# ---- Parse-trace dump tool (for C vs Python prototype comparison) --------
# Builds fluff_parse_dump: reads a file, runs the fluff LZ77 parser with the
# FLUFF_DEBUG_PARSE hook enabled, and writes parse decisions as CSV.
PARSE_DUMP_TARGET = $(BUILDDIR)/fluff_parse_dump
PARSE_DUMP_SRCS   = src/tools/fluff_parse_dump.c \
                    $(COREDIR)/fluff.c \
                    $(COREDIR)/bp_types.h
PARSE_DUMP_CFLAGS = -ansi -pedantic -Wall -Wextra -Wno-unused-function \
                    -O2 -DFLUFF_DEBUG_PARSE -I$(COREDIR)

# --- Inject version defines into every build variant --------------------------
# Placed after all *_CFLAGS / *_FLAGS definitions so += has a target to append
# to.  bp_types.h uses #ifndef guards so these -D flags take precedence over
# the fallback values in the header (useful when cross-compiling externally).
CFLAGS          += $(BP_VER_DEFS)
DEBUG_CFLAGS    += $(BP_VER_DEFS)
PROFILE_CFLAGS  += $(BP_VER_DEFS)
WCX_FLAGS       += $(BP_VER_DEFS)
WIN_FLAGS       += $(BP_VER_DEFS)
WASM_CFLAGS     += $(BP_VER_DEFS)
AMIGA68K_CFLAGS += $(BP_VER_DEFS)
ATARI_CFLAGS    += $(BP_VER_DEFS)
MACOS68K_CFLAGS += $(BP_VER_DEFS)
MACOSX_CFLAGS   += $(BP_VER_DEFS)
HAIKU_CFLAGS    += $(BP_VER_DEFS)
RISCOS_CFLAGS   += $(BP_VER_DEFS)
ARM_CFLAGS      += $(BP_VER_DEFS)
ARM64_CFLAGS    += $(BP_VER_DEFS)
AROS_CFLAGS     += $(BP_VER_DEFS)
AMIGAOS4_CFLAGS += $(BP_VER_DEFS)
MORPHOS_CFLAGS  += $(BP_VER_DEFS)
FREEBSD_CFLAGS  += $(BP_VER_DEFS)
NETBSD_CFLAGS   += $(BP_VER_DEFS)
RISCV_CFLAGS    += $(BP_VER_DEFS)
MUSL_CFLAGS     += $(BP_VER_DEFS)
ILLUMOS_CFLAGS  += $(BP_VER_DEFS)
E2K_CFLAGS      += $(BP_VER_DEFS)
OS2_CFLAGS      += $(BP_VER_DEFS)
QL_CFLAGS       += $(BP_VER_DEFS)
X68K_CFLAGS     += $(BP_VER_DEFS)
HURD_CFLAGS     += $(BP_VER_DEFS)

PARSE_DUMP_CFLAGS += $(BP_VER_DEFS)

.PHONY: parse-dump
parse-dump: $(PARSE_DUMP_TARGET)

$(PARSE_DUMP_TARGET): src/tools/fluff_parse_dump.c $(COREDIR)/fluff.c | $(BUILDDIR)
	$(CC) $(PARSE_DUMP_CFLAGS) -o $@ src/tools/fluff_parse_dump.c $(COREDIR)/fluff.c
	@echo "  OK    $@ (parse-trace dump tool)"

$(BUILDDIR):
	mkdir -p $@


# Targets for four 16-bit platforms using Open Watcom (wcc / wlink).
# Prerequisite: Open Watcom 2.0 must be installed and on PATH.
#   Ubuntu/Debian: apt-get install open-watcom
#   Or download from https://github.com/open-watcom/open-watcom-v2/releases
#
# All mini builds share src/core/mini/ and src/frontend/mini_shared/.
# The archive format is byte-for-byte identical to the full build.
#
# Open Watcom toolchain (used by all 16-bit targets)
WATCOM_ROOT  = /opt/watcom
WATCOM_BIN   = $(WATCOM_ROOT)/binl64
WCC16        = $(WATCOM_BIN)/wcc
WLINK16      = $(WATCOM_BIN)/wlink
# MINI_ENV: set WATCOM (for wlink runtime library search) and INCLUDE (neutral C headers)
MINI_ENV     = WATCOM=$(WATCOM_ROOT) INCLUDE=$(WATCOM_ROOT)/h

# Core files: strict ANSI C89 (-za); -wcd=202 suppresses W202 for unused static
# helpers in bp_types.h (defined but not referenced is expected for shared headers)
# -zm places each function in its own segment so wlink "option eliminate" can
# drop unused functions; safe in small-code models (segments recombine as _TEXT)
MINI_CORE_FLAGS  = -za -os -zm -w4 -wcd=202 -DSIZEOF_INT=2 -DSIZEOF_LONG=4 \
                   -DBP_MINI -DBP_MINI_QUIET \
                   -DBP_CREATOR_CPU=BP_CPU_8086

# Frontend files: no -za (POSIX extensions needed for struct stat, snprintf)
MINI_FRONT_FLAGS =     -os -zm -w4 -wcd=202 -DSIZEOF_INT=2 -DSIZEOF_LONG=4 \
                   -DBP_MINI \
                   -DBP_CREATOR_CPU=BP_CPU_8086
MINI_CORE_FLAGS  += $(MINI_BP_VER_DEFS)
MINI_FRONT_FLAGS += $(MINI_BP_VER_DEFS)

# Include paths for mini builds (Watcom neutral headers first, then project dirs)
MINI_INCS    = -i=$(WATCOM_ROOT)/h -i=src/core -i=src/core/mini -i=src/frontend/mini_shared

MINI_COREDIR  = src/core/mini
MINI_SHARED   = src/frontend/mini_shared

# ---- mini-test: Linux build to verify C89 syntax (no 16-bit flags) ------
# Core sources are compiled with strict -ansi -pedantic to confirm C89 compliance.
# Frontend sources are compiled with -Wall only (frontends may use C99 snprintf etc.)
MINI_TEST_CORE  = -ansi -pedantic -Wall -Wextra -Wno-unused-function -DBP_MINI $(MINI_BP_VER_DEFS)
MINI_TEST_FRONT = -Wall -Wextra -Wno-unused-function -Wno-format-truncation -DBP_MINI $(MINI_BP_VER_DEFS)
MINI_TEST_DIR   = build/mini-test

.PHONY: mini-test
mini-test:
	mkdir -p $(MINI_TEST_DIR)
	$(CC) $(MINI_TEST_CORE)  -I$(COREDIR) -c -o $(MINI_TEST_DIR)/archiver.o    $(MINI_COREDIR)/archiver.c
	$(CC) $(MINI_TEST_CORE)  -I$(COREDIR) -c -o $(MINI_TEST_DIR)/compress.o    $(MINI_COREDIR)/compress.c
	$(CC) $(MINI_TEST_CORE)  -I$(COREDIR) -c -o $(MINI_TEST_DIR)/boltlite.o        $(COREDIR)/boltlite.c
	$(CC) $(MINI_TEST_CORE)  -I$(COREDIR) -O2 -c -o $(MINI_TEST_DIR)/boltlz_decomp.o  $(MINI_COREDIR)/boltlz_decomp.c
	$(CC) $(MINI_TEST_FRONT) -I$(COREDIR) -c -o $(MINI_TEST_DIR)/mini_common.o $(MINI_SHARED)/mini_common.c
	$(CC) $(MINI_TEST_CORE)  -I$(COREDIR) -c -o $(MINI_TEST_DIR)/bp_codepage.o $(COREDIR)/bp_codepage.c
	$(CC) $(MINI_TEST_CORE)  -Wall        -O2 -c -o $(MINI_TEST_DIR)/boltlz.o     $(BOLTLZDIR)/boltlz.c
	$(CC) $(MINI_TEST_FRONT) -I$(COREDIR) -I$(MINI_COREDIR) -I$(MINI_SHARED) \
	    -c -o $(MINI_TEST_DIR)/main_dos16.o  src/frontend/dos16/main.c
	$(CC) $(MINI_TEST_FRONT) -I$(COREDIR) -I$(MINI_COREDIR) -I$(MINI_SHARED) \
	    -c -o $(MINI_TEST_DIR)/main_cpm86.o  src/frontend/cpm86/main.c
	$(CC) $(MINI_TEST_FRONT) -I$(COREDIR) -I$(MINI_COREDIR) -I$(MINI_SHARED) \
	    -c -o $(MINI_TEST_DIR)/main_win16.o  src/frontend/win16/main.c
	$(CC) $(MINI_TEST_FRONT) -I$(COREDIR) -I$(MINI_COREDIR) -I$(MINI_SHARED) \
	    -c -o $(MINI_TEST_DIR)/main_os2_16.o src/frontend/os2_16/main.c
	$(CC) $(MINI_TEST_FRONT) -o $(MINI_TEST_DIR)/bpmini-test \
	    $(MINI_TEST_DIR)/archiver.o $(MINI_TEST_DIR)/compress.o \
	    $(MINI_TEST_DIR)/boltlite.o $(MINI_TEST_DIR)/mini_common.o $(MINI_TEST_DIR)/bp_codepage.o \
	    $(MINI_TEST_DIR)/boltlz.o $(MINI_TEST_DIR)/main_dos16.o
	@echo "  OK    $(MINI_TEST_DIR)/bpmini-test [C89 core check + linux test binary]"

# ---- linuxmini: 32-bit Linux bpmini (mini archiver, POSIX filesystem) ----
# Mirrors the dos16/win16 mini builds but compiled natively for Linux -m32.
# Useful for testing the mini archiver without DOS/Windows emulation.
LINUXMINI_BDIR  = build/linux32mini
LINUXMINI_EXE   = $(LINUXMINI_BDIR)/bpmini
LINUXMINI_CFLAGS = -ansi -pedantic -Wall -Wextra -Wno-unused-function -Os \
                   -ffunction-sections -fdata-sections -m32 \
                   -fno-asynchronous-unwind-tables -fno-unwind-tables \
                   -fno-stack-protector -fno-ident \
                   -DBP_MINI -DBP_CREATOR_OS=BP_OS_LINUX $(MINI_BP_VER_DEFS)
LINUXMINI_FFLAGS = -Wall -Wextra -Wno-unused-function -Wno-format-truncation -Os \
                   -ffunction-sections -fdata-sections -m32 \
                   -fno-asynchronous-unwind-tables -fno-unwind-tables \
                   -fno-stack-protector -fno-ident \
                   -DBP_MINI -DBP_CREATOR_OS=BP_OS_LINUX $(MINI_BP_VER_DEFS)

.PHONY: linuxmini
linuxmini:
	mkdir -p $(LINUXMINI_BDIR)
	$(CC) $(LINUXMINI_CFLAGS) -I$(COREDIR)                          -c -o $(LINUXMINI_BDIR)/archiver.o    $(MINI_COREDIR)/archiver.c
	$(CC) $(LINUXMINI_CFLAGS) -I$(COREDIR)                          -c -o $(LINUXMINI_BDIR)/compress.o    $(MINI_COREDIR)/compress.c
	$(CC) $(LINUXMINI_CFLAGS) -I$(COREDIR)                          -c -o $(LINUXMINI_BDIR)/boltlite.o        $(COREDIR)/boltlite.c
	$(CC) $(LINUXMINI_CFLAGS)  -Wall              -O2 -c -o $(LINUXMINI_BDIR)/boltlz.o     $(BOLTLZDIR)/boltlz.c
	$(CC) $(LINUXMINI_FFLAGS) -I$(COREDIR) -I$(MINI_COREDIR)        -c -o $(LINUXMINI_BDIR)/mini_common.o $(MINI_SHARED)/mini_common.c
	$(CC) $(LINUXMINI_CFLAGS) -I$(COREDIR) -c -o $(LINUXMINI_BDIR)/bp_codepage.o $(COREDIR)/bp_codepage.c
	$(CC) $(LINUXMINI_FFLAGS) -I$(COREDIR) -I$(MINI_COREDIR) -I$(MINI_SHARED) \
	    -c -o $(LINUXMINI_BDIR)/main.o src/frontend/linuxmini/main.c
	$(CC) -m32 -fuse-ld=bfd -Wl,--gc-sections \
	    -Wl,--strip-all -Wl,--build-id=none -Wl,-z,noseparate-code \
	    -o $(LINUXMINI_EXE) \
	    $(LINUXMINI_BDIR)/archiver.o $(LINUXMINI_BDIR)/compress.o \
	    $(LINUXMINI_BDIR)/boltlite.o $(LINUXMINI_BDIR)/boltlz.o \
	    $(LINUXMINI_BDIR)/mini_common.o $(LINUXMINI_BDIR)/bp_codepage.o $(LINUXMINI_BDIR)/main.o
	@echo "  OK    $(LINUXMINI_EXE)"

# ---- dos16: Open Watcom, 16-bit DOS compact model -----------------------
DOS16_CODEPAGE_FLAGS ?=
DOS16_TEST_FLAGS ?=
DOS16_CORE  = -bt=dos -mc -0 $(MINI_CORE_FLAGS)  -DBP_CREATOR_OS=BP_OS_DOS $(DOS16_CODEPAGE_FLAGS)
DOS16_FRONT = -bt=dos -mc -0 $(MINI_FRONT_FLAGS) -DBP_CREATOR_OS=BP_OS_DOS $(DOS16_CODEPAGE_FLAGS) $(DOS16_TEST_FLAGS)
DOS16_BDIR   = build/dos16
DOS16_EXE    = $(DOS16_BDIR)/bpmini.exe
DOS16_STACK_BDIR = build/dos16-stack
DOS16_STACK_EXE = $(DOS16_STACK_BDIR)/bpmini.exe
DOS16_STACK_WRITER_EXE = $(DOS16_STACK_BDIR)/writer-resource.exe
DOS16_STACK_BYTES ?= 8192
DOS16_STACK_ALLOC_FLAGS = -DBP_TEST_ALLOC_ACCOUNTING \
	-Dmalloc=bp_test_malloc -Dcalloc=bp_test_calloc \
	-Drealloc=bp_test_realloc -Dfree=bp_test_free

.PHONY: dos16
dos16:
	mkdir -p $(DOS16_BDIR)
	$(MINI_ENV) $(WCC16) $(DOS16_CORE)  $(MINI_INCS) -fo=$(DOS16_BDIR)/archiver.obj   $(MINI_COREDIR)/archiver.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE)  $(MINI_INCS) -fo=$(DOS16_BDIR)/compress.obj   $(MINI_COREDIR)/compress.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE)  $(MINI_INCS) -fo=$(DOS16_BDIR)/boltlite.obj       $(COREDIR)/boltlite.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE)  $(MINI_INCS) -fo=$(DOS16_BDIR)/boltlz_decomp.obj $(MINI_COREDIR)/boltlz_decomp.c
	$(MINI_ENV) $(WCC16) $(DOS16_FRONT) $(MINI_INCS) -fo=$(DOS16_BDIR)/mini_common.obj $(MINI_SHARED)/mini_common.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE)  $(MINI_INCS) -fo=$(DOS16_BDIR)/bp_codepage.obj $(COREDIR)/bp_codepage.c
	$(MINI_ENV) $(WCC16) $(DOS16_FRONT) $(MINI_INCS) -fo=$(DOS16_BDIR)/main.obj       src/frontend/dos16/main.c
	$(MINI_ENV) $(WLINK16) @$(WATCOM_BIN)/wlink.lnk system dos option eliminate \
	    option stack=$(DOS16_STACK_BYTES) \
	    option map=$(DOS16_BDIR)/bpmini.map \
	    file {$(DOS16_BDIR)/archiver.obj} \
	    file {$(DOS16_BDIR)/compress.obj} \
	    file {$(DOS16_BDIR)/boltlite.obj} \
	    file {$(DOS16_BDIR)/boltlz_decomp.obj} \
	    file {$(DOS16_BDIR)/mini_common.obj} \
	    file {$(DOS16_BDIR)/bp_codepage.obj} \
	    file {$(DOS16_BDIR)/main.obj} \
	    name $(DOS16_EXE)
	@echo "  OK    $(DOS16_EXE)"

.PHONY: dos16-stack-probe
dos16-stack-probe:
	mkdir -p $(DOS16_STACK_BDIR)
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) -DBP_TEST_STACK_HIGHWATER $(DOS16_STACK_ALLOC_FLAGS) $(MINI_INCS) \
	    -fo=$(DOS16_STACK_BDIR)/archiver.obj $(MINI_COREDIR)/archiver.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) -DBP_TEST_STACK_HIGHWATER $(DOS16_STACK_ALLOC_FLAGS) $(MINI_INCS) \
	    -fo=$(DOS16_STACK_BDIR)/compress.obj $(MINI_COREDIR)/compress.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) -DBP_TEST_STACK_HIGHWATER $(MINI_INCS) \
	    -fo=$(DOS16_STACK_BDIR)/boltlite.obj $(COREDIR)/boltlite.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) -DBP_TEST_STACK_HIGHWATER $(MINI_INCS) \
	    -fo=$(DOS16_STACK_BDIR)/boltlz_decomp.obj $(MINI_COREDIR)/boltlz_decomp.c
	$(MINI_ENV) $(WCC16) $(DOS16_FRONT) -DBP_TEST_STACK_HIGHWATER $(DOS16_STACK_ALLOC_FLAGS) $(MINI_INCS) \
	    -fo=$(DOS16_STACK_BDIR)/mini_common.obj $(MINI_SHARED)/mini_common.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) $(MINI_INCS) \
	    -fo=$(DOS16_STACK_BDIR)/bp_codepage.obj $(COREDIR)/bp_codepage.c
	$(MINI_ENV) $(WCC16) $(DOS16_FRONT) -DBP_TEST_STACK_HIGHWATER $(DOS16_STACK_ALLOC_FLAGS) $(MINI_INCS) \
	    -fo=$(DOS16_STACK_BDIR)/main.obj src/frontend/dos16/main.c
	$(MINI_ENV) $(WCC16) -bt=dos -mc -0 -za -os -zm -w4 -wcd=202 \
	    -i=$(WATCOM)/h -fo=$(DOS16_STACK_BDIR)/alloc_probe.obj \
	    test/dos16_alloc_probe.c
	$(MINI_ENV) $(WCC16) $(DOS16_FRONT) $(MINI_INCS) \
	    -fo=$(DOS16_STACK_BDIR)/writer_resource.obj test/dos16_writer_resource16.c
	$(MINI_ENV) $(WLINK16) @$(WATCOM_BIN)/wlink.lnk system dos option eliminate \
	    option stack=$(DOS16_STACK_BYTES) \
	    option map=$(DOS16_STACK_BDIR)/bpmini.map \
	    file {$(DOS16_STACK_BDIR)/archiver.obj} \
	    file {$(DOS16_STACK_BDIR)/compress.obj} \
	    file {$(DOS16_STACK_BDIR)/boltlite.obj} \
	    file {$(DOS16_STACK_BDIR)/boltlz_decomp.obj} \
	    file {$(DOS16_STACK_BDIR)/mini_common.obj} \
	    file {$(DOS16_STACK_BDIR)/bp_codepage.obj} \
	    file {$(DOS16_STACK_BDIR)/main.obj} \
	    file {$(DOS16_STACK_BDIR)/alloc_probe.obj} \
	    name $(DOS16_STACK_EXE)
	@echo "  OK    $(DOS16_STACK_EXE)"
	$(MINI_ENV) WATCOM=$(WATCOM_ROOT) $(WLINK16) @$(WATCOM_BIN)/wlink.lnk \
	    system dos option eliminate option stack=$(DOS16_STACK_BYTES) \
	    file {$(DOS16_STACK_BDIR)/archiver.obj} \
	    file {$(DOS16_STACK_BDIR)/compress.obj} \
	    file {$(DOS16_STACK_BDIR)/boltlite.obj} \
	    file {$(DOS16_STACK_BDIR)/boltlz_decomp.obj} \
	    file {$(DOS16_STACK_BDIR)/mini_common.obj} \
	    file {$(DOS16_STACK_BDIR)/bp_codepage.obj} \
	    file {$(DOS16_STACK_BDIR)/writer_resource.obj} \
	    file {$(DOS16_STACK_BDIR)/alloc_probe.obj} \
	    name $(DOS16_STACK_WRITER_EXE)
	@echo "  OK    $(DOS16_STACK_WRITER_EXE)"

.PHONY: dos16-cp850 dos16-cp850-crash-hook
dos16-cp850:
	$(MAKE) dos16 DOS16_BDIR=build/dos16-cp850 DOS16_EXE=build/dos16-cp850/bpmini.exe \
	    DOS16_CODEPAGE_FLAGS=-DBP_MINI_CODEPAGE=BP_CP_850

dos16-cp850-crash-hook:
	$(MAKE) dos16 DOS16_BDIR=build/dos16-cp850-crash-hook \
	    DOS16_EXE=build/dos16-cp850-crash-hook/bpcrash.exe \
	    DOS16_CODEPAGE_FLAGS=-DBP_MINI_CODEPAGE=BP_CP_850 \
	    DOS16_TEST_FLAGS=-DBP_TEST_EXIT_AFTER_BACKUP

# ---- win16: Open Watcom, 16-bit Windows 3.x compact model ---------------
# The format-6 validator and metadata support exceed a single 64 KiB code
# segment. Use the large code model for Win16 while retaining 16-bit data.
WIN16_CORE  = -bt=windows -ml -0 $(MINI_CORE_FLAGS)  -DBP_CREATOR_OS=BP_OS_WIN16
WIN16_FRONT = -bt=windows -ml -0 $(MINI_FRONT_FLAGS) -DBP_CREATOR_OS=BP_OS_WIN16
WIN16_BDIR   = build/win16
WIN16_EXE    = $(WIN16_BDIR)/bpmini.exe

.PHONY: win16
win16:
	mkdir -p $(WIN16_BDIR)
	$(MINI_ENV) $(WCC16) $(WIN16_CORE)  $(MINI_INCS) -fo=$(WIN16_BDIR)/archiver.obj   $(MINI_COREDIR)/archiver.c
	$(MINI_ENV) $(WCC16) $(WIN16_CORE)  $(MINI_INCS) -fo=$(WIN16_BDIR)/compress.obj   $(MINI_COREDIR)/compress.c
	$(MINI_ENV) $(WCC16) $(WIN16_CORE)  $(MINI_INCS) -fo=$(WIN16_BDIR)/boltlite.obj       $(COREDIR)/boltlite.c
	$(MINI_ENV) $(WCC16) $(WIN16_CORE)  $(MINI_INCS) -fo=$(WIN16_BDIR)/boltlz_decomp.obj $(MINI_COREDIR)/boltlz_decomp.c
	$(MINI_ENV) $(WCC16) $(WIN16_FRONT) $(MINI_INCS) -fo=$(WIN16_BDIR)/mini_common.obj $(MINI_SHARED)/mini_common.c
	$(MINI_ENV) $(WCC16) $(WIN16_CORE)  $(MINI_INCS) -fo=$(WIN16_BDIR)/bp_codepage.obj $(COREDIR)/bp_codepage.c
	$(MINI_ENV) $(WCC16) $(WIN16_FRONT) $(MINI_INCS) -fo=$(WIN16_BDIR)/main.obj       src/frontend/win16/main.c
	$(MINI_ENV) $(WCC16) $(WIN16_FRONT) $(MINI_INCS) \
	    -fo=$(WIN16_BDIR)/resource_layout.obj test/unit/mini_test_resource16.c
	$(MINI_ENV) $(WLINK16) @$(WATCOM_BIN)/wlink.lnk system windows option eliminate \
	    option map=$(WIN16_BDIR)/bpmini.map \
	    file {$(WIN16_BDIR)/archiver.obj} \
	    file {$(WIN16_BDIR)/compress.obj} \
	    file {$(WIN16_BDIR)/boltlite.obj} \
	    file {$(WIN16_BDIR)/boltlz_decomp.obj} \
	    file {$(WIN16_BDIR)/mini_common.obj} \
	    file {$(WIN16_BDIR)/bp_codepage.obj} \
	    file {$(WIN16_BDIR)/main.obj} \
	    name $(WIN16_EXE)
	@echo "  OK    $(WIN16_EXE)"

# ---- os2-16: Open Watcom, 16-bit OS/2 1.x compact model ----------------
# OS/2 1.x headers are in a separate subdirectory from OS/2 2.x headers.
OS2_16_CORE  = -bt=os2 -mc -0 $(MINI_CORE_FLAGS)  -DBP_CREATOR_OS=BP_OS_OS2
OS2_16_FRONT = -bt=os2 -mc -0 $(MINI_FRONT_FLAGS) -DBP_CREATOR_OS=BP_OS_OS2
OS2_16_INCS  = $(MINI_INCS) -i=$(WATCOM_ROOT)/h/os21x
OS2_16_BDIR  = build/os2-16
OS2_16_EXE   = $(OS2_16_BDIR)/bpmini.exe

.PHONY: os2-16
os2-16:
	mkdir -p $(OS2_16_BDIR)
	$(MINI_ENV) $(WCC16) $(OS2_16_CORE)  $(OS2_16_INCS) -fo=$(OS2_16_BDIR)/archiver.obj   $(MINI_COREDIR)/archiver.c
	$(MINI_ENV) $(WCC16) $(OS2_16_CORE)  $(OS2_16_INCS) -fo=$(OS2_16_BDIR)/compress.obj   $(MINI_COREDIR)/compress.c
	$(MINI_ENV) $(WCC16) $(OS2_16_CORE)  $(OS2_16_INCS) -fo=$(OS2_16_BDIR)/boltlite.obj       $(COREDIR)/boltlite.c
	$(MINI_ENV) $(WCC16) $(OS2_16_CORE)  $(OS2_16_INCS) -fo=$(OS2_16_BDIR)/boltlz_decomp.obj $(MINI_COREDIR)/boltlz_decomp.c
	$(MINI_ENV) $(WCC16) $(OS2_16_FRONT) $(OS2_16_INCS) -fo=$(OS2_16_BDIR)/mini_common.obj $(MINI_SHARED)/mini_common.c
	$(MINI_ENV) $(WCC16) $(OS2_16_CORE)  $(OS2_16_INCS) -fo=$(OS2_16_BDIR)/bp_codepage.obj $(COREDIR)/bp_codepage.c
	$(MINI_ENV) $(WCC16) $(OS2_16_FRONT) $(OS2_16_INCS) -fo=$(OS2_16_BDIR)/main.obj       src/frontend/os2_16/main.c
	$(MINI_ENV) $(WCC16) $(OS2_16_FRONT) $(OS2_16_INCS) \
	    -fo=$(OS2_16_BDIR)/resource_layout.obj test/unit/mini_test_resource16.c
	$(MINI_ENV) $(WLINK16) @$(WATCOM_BIN)/wlink.lnk system os2 option eliminate \
	    option map=$(OS2_16_BDIR)/bpmini.map \
	    file {$(OS2_16_BDIR)/archiver.obj} \
	    file {$(OS2_16_BDIR)/compress.obj} \
	    file {$(OS2_16_BDIR)/boltlite.obj} \
	    file {$(OS2_16_BDIR)/boltlz_decomp.obj} \
	    file {$(OS2_16_BDIR)/mini_common.obj} \
	    file {$(OS2_16_BDIR)/bp_codepage.obj} \
	    file {$(OS2_16_BDIR)/main.obj} \
	    name $(OS2_16_EXE)
	@echo "  OK    $(OS2_16_EXE)"

# ---- cpm86: Aztec C 86 (manual build, instructions only) ----------------
# CP/M-86 requires Aztec C 86 (or compatible CP/M-86 C compiler).
# Open Watcom does not target CP/M-86.  Build manually on a CP/M-86 system
# or use an emulator (e.g., RunCPM).
#
# Compile flags (Aztec C 86 syntax):
#   cc -DSIZEOF_INT=2 -DSIZEOF_LONG=4 -DBP_MINI \
#      -DBP_CREATOR_OS=BP_OS_CPM86 -DBP_CREATOR_CPU=BP_CPU_8086 \
#      src/core/mini/archiver.c src/core/mini/compress.c \
#      src/core/boltlite.c src/core/bp_codepage.c \
#      src/frontend/mini_shared/mini_common.c \
#      src/core/mini/boltlz_decomp.c \
#      src/frontend/cpm86/main.c \
#      -o BPMINI.CMD
.PHONY: cpm86
cpm86:
	@echo "  NOTE  CP/M-86 requires Aztec C 86; see comment in Makefile for build instructions"

# Host-run policy fixture for the CP/M-86 frontend. This validates the C89
# frontend logic, not CP/M-86 filesystem or runtime behavior.
CPM86_HOST_DIR = build/cpm86-host
CPM86_HOST_BIN = $(CPM86_HOST_DIR)/bpmini
.PHONY: cpm86-host-test
cpm86-host-test:
	mkdir -p $(CPM86_HOST_DIR)
	$(CC) $(filter-out $(BP_VER_DEFS),$(CFLAGS)) $(MINI_BP_VER_DEFS) -DBP_MINI \
	    -DBP_CREATOR_OS=BP_OS_CPM86 -DBP_CREATOR_CPU=BP_CPU_8086 \
	    -I$(COREDIR) -I$(MINI_COREDIR) -I$(MINI_SHARED) \
	    -o $(CPM86_HOST_BIN) \
	    $(MINI_COREDIR)/archiver.c $(MINI_COREDIR)/compress.c \
	    $(MINI_COREDIR)/boltlz_decomp.c $(COREDIR)/boltlite.c \
	    $(COREDIR)/boltlz.c \
	    $(MINI_SHARED)/mini_common.c $(COREDIR)/bp_codepage.c \
	    src/frontend/cpm86/main.c
	python3 test/check_cpm86_host.py $(CPM86_HOST_BIN)

# ---- mini-all: build all cross-compilable mini targets ------------------
.PHONY: mini-all
mini-all: mini-test dos16 win16 os2-16
	@echo "Mini builds complete."

# ---- GUI frontends --------------------------------------------------------

GUI_COMMON     = src/frontend/gui_common

# Core source list shared by all GUI builds
GUI_CORE_SRCS  = $(COREDIR)/archiver.c \
                 $(COREDIR)/compress.c \
                 $(COREDIR)/boltlite.c \
                 $(COREDIR)/fluff.c \
                 $(COREDIR)/bp_codepage.c \
                 $(BOLTLZDIR)/boltlz.c

GUI_CORE_FLAGS = $(BP_VER_DEFS) -Wall -Wextra -Wno-unused-function -O2 -I$(COREDIR) -I$(BOLTLZDIR) -I$(GUI_COMMON)
GUI_RC_FLAGS   = $(BP_VER_DEFS)
GUI_FAST_FLUFF_CPPFLAGS = -DBOLTPACK_FAST_FLUFF
GUI_FAST_FLUFF_LDFLAGS  = -fopenmp
GUI_FAST_FLUFF_CFLAGS   = $(GUI_CORE_FLAGS) $(GUI_FAST_FLUFF_CPPFLAGS) \
	$(GUI_FAST_FLUFF_LDFLAGS) -std=c11 -O3

# ---- Linux FLTK GUI (x86-64) ----
LINUX_GUI_BUILDDIR  = build/linux-gui
LINUX_GUI_CORE_OBJS = \
    $(LINUX_GUI_BUILDDIR)/archiver.o    \
    $(LINUX_GUI_BUILDDIR)/compress.o    \
    $(LINUX_GUI_BUILDDIR)/boltlite.o        \
    $(LINUX_GUI_BUILDDIR)/fluff.o       \
	$(LINUX_GUI_BUILDDIR)/fluff_fast.o  \
    $(LINUX_GUI_BUILDDIR)/bp_codepage.o \
    $(LINUX_GUI_BUILDDIR)/boltlz.o     \
    $(LINUX_GUI_BUILDDIR)/zstd.o        \
    $(LINUX_GUI_BUILDDIR)/zstd_wrap.o

.PHONY: linux-gui
linux-gui:
	mkdir -p $(LINUX_GUI_BUILDDIR)
	gcc $(GUI_CORE_FLAGS) $(GUI_FAST_FLUFF_CPPFLAGS) $(GUI_FAST_FLUFF_LDFLAGS) -DBOLTPACK_ZSTD -I$(ZSTDDIR) \
	    -c $(COREDIR)/archiver.c    -o $(LINUX_GUI_BUILDDIR)/archiver.o
	gcc $(GUI_CORE_FLAGS) -c $(COREDIR)/compress.c    -o $(LINUX_GUI_BUILDDIR)/compress.o
	gcc $(GUI_CORE_FLAGS) -c $(COREDIR)/boltlite.c        -o $(LINUX_GUI_BUILDDIR)/boltlite.o
	gcc $(GUI_CORE_FLAGS) -c $(COREDIR)/fluff.c       -o $(LINUX_GUI_BUILDDIR)/fluff.o
	gcc $(GUI_FAST_FLUFF_CFLAGS) -c $(COREDIR)/fluff_fast.c  -o $(LINUX_GUI_BUILDDIR)/fluff_fast.o
	gcc $(GUI_CORE_FLAGS) -c $(COREDIR)/bp_codepage.c -o $(LINUX_GUI_BUILDDIR)/bp_codepage.o
	gcc $(GUI_CORE_FLAGS) -O2 -c $(BOLTLZDIR)/boltlz.c      -o $(LINUX_GUI_BUILDDIR)/boltlz.o
	gcc $(ZSTD_CC_FLAGS) -I$(ZSTDDIR) \
	    -c $(ZSTDDIR)/zstd.c          -o $(LINUX_GUI_BUILDDIR)/zstd.o
	gcc $(ZSTD_CC_FLAGS) -DBOLTPACK_ZSTD -I$(COREDIR) -I$(ZSTDDIR) \
	    -c $(COREDIR)/zstd_wrap.c     -o $(LINUX_GUI_BUILDDIR)/zstd_wrap.o
	gcc $(GUI_CORE_FLAGS) -DBOLTPACK_NET \
	    -c src/frontend/linux/bp_net.c -o $(LINUX_GUI_BUILDDIR)/bp_net.o
	g++ $(GUI_CORE_FLAGS) $(GUI_FAST_FLUFF_CPPFLAGS) -DBOLTPACK_ZSTD -DBOLTPACK_NET -I$(ZSTDDIR) \
	    $(LINUX_GUI_CORE_OBJS) \
	    $(LINUX_GUI_BUILDDIR)/bp_net.o \
	    src/frontend/linux_gui/main.cpp \
	    $$(fltk-config --use-images --cxxflags --ldflags) \
	    $(GUI_FAST_FLUFF_LDFLAGS) \
	    -o $(LINUX_GUI_BUILDDIR)/boltpack-gui
	@echo "  OK    $(LINUX_GUI_BUILDDIR)/boltpack-gui"

.PHONY: linux-gui-interoperability-test
LINUX_GUI_PREFIX_FIXTURE = $(LINUX_GUI_BUILDDIR)/prefix-conflict-fixture
$(LINUX_GUI_PREFIX_FIXTURE): test/haiku_name_conflict_fixture.c \
	    $(COREDIR)/archiver.c $(COREDIR)/compress.c $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c $(COREDIR)/bp_codepage.c $(BOLTLZDIR)/boltlz.c
	mkdir -p $(LINUX_GUI_BUILDDIR)
	$(CC) -std=c89 -Wall -Wextra -Wno-unused-function \
	    -I$(COREDIR) test/haiku_name_conflict_fixture.c \
	    $(COREDIR)/archiver.c $(COREDIR)/compress.c \
	    $(COREDIR)/boltlite.c $(COREDIR)/fluff.c \
	    $(COREDIR)/bp_codepage.c $(BOLTLZDIR)/boltlz.c -lm -o $@

linux-gui-interoperability-test: modern linuxmini linux-gui $(LINUX_GUI_PREFIX_FIXTURE)
	python3 test/check_linux_gui_interop.py \
	    --gui $(LINUX_GUI_BUILDDIR)/boltpack-gui \
	    --cli build/linux/boltpack --mini $(LINUXMINI_EXE) \
	    --mini-runner "$(LINUXMINI_RUNNER)" --prefix-fixture $(LINUX_GUI_PREFIX_FIXTURE)

.PHONY: linux-cli-extract-failure-test
linux-cli-extract-failure-test: modern
	python3 test/check_linux_extract_failures.py --boltpack build/linux/boltpack

# ---- Install Linux GUI to desktop (XDG / freedesktop.org) ----
# Usage: sudo make install-linux-gui
#        PREFIX=/usr sudo make install-linux-gui   (system-wide under /usr)
#        make install-linux-gui                    (user install, no sudo needed)
#
# The icon is installed as "boltpack" in the hicolor scalable theme so the
# .desktop file's "Icon=boltpack" resolves correctly on any GTK/Qt desktop.
# After install, run: cinnamon --replace &   (or log out/in) to refresh.

PREFIX     ?= /usr/local
DESTDIR    ?=
ICON_DIR    = $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
APP_DIR     = $(DESTDIR)$(PREFIX)/share/applications
BIN_DIR     = $(DESTDIR)$(PREFIX)/bin
ICON_SRC    = resources/pics/boltpack_magenta.svg
DESKTOP_SRC = packaging/linux/boltpack-gui.desktop

.PHONY: install-linux-gui uninstall-linux-gui

install-linux-gui: linux-gui
	install -d $(BIN_DIR) $(ICON_DIR) $(APP_DIR)
	install -m 755 $(LINUX_GUI_BUILDDIR)/boltpack-gui $(BIN_DIR)/boltpack-gui
	install -m 644 $(ICON_SRC) $(ICON_DIR)/boltpack.svg
	install -m 644 $(DESKTOP_SRC) $(APP_DIR)/boltpack-gui.desktop
	@gtk-update-icon-cache -f -t $(DESTDIR)$(PREFIX)/share/icons/hicolor 2>/dev/null || true
	@update-desktop-database $(APP_DIR) 2>/dev/null || true
	@echo "  OK    BoltPack GUI installed ($(BIN_DIR)/boltpack-gui)"
	@echo "  OK    Icon  -> $(ICON_DIR)/boltpack.svg"
	@echo "  OK    Menu  -> $(APP_DIR)/boltpack-gui.desktop"

uninstall-linux-gui:
	rm -f $(BIN_DIR)/boltpack-gui
	rm -f $(ICON_DIR)/boltpack.svg
	rm -f $(APP_DIR)/boltpack-gui.desktop
	@gtk-update-icon-cache -f -t $(DESTDIR)$(PREFIX)/share/icons/hicolor 2>/dev/null || true
	@update-desktop-database $(APP_DIR) 2>/dev/null || true
	@echo "  OK    BoltPack GUI uninstalled"

# ---- Win32 GUI (32-bit, mingw) ----
WIN32_GUI_BUILDDIR = build/win32-gui

.PHONY: win32-gui
win32-gui:
	mkdir -p $(WIN32_GUI_BUILDDIR)
	i686-w64-mingw32-windres $(GUI_RC_FLAGS) src/frontend/win_gui/resource.rc \
	    -o $(WIN32_GUI_BUILDDIR)/resource.o
	i686-w64-mingw32-gcc $(GUI_CORE_FLAGS) -m32 -mwindows \
	    $(GUI_CORE_SRCS) \
	    src/frontend/win_gui/main.c \
	    $(WIN32_GUI_BUILDDIR)/resource.o \
	    -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luser32 -lgdi32 \
	    -o $(WIN32_GUI_BUILDDIR)/boltpack-gui.exe
	@echo "  OK    $(WIN32_GUI_BUILDDIR)/boltpack-gui.exe"

# ---- Win64 GUI (64-bit, mingw) ----
WIN64_GUI_BUILDDIR = build/win64-gui

.PHONY: win64-gui
win64-gui:
	mkdir -p $(WIN64_GUI_BUILDDIR)
	x86_64-w64-mingw32-windres $(GUI_RC_FLAGS) src/frontend/win_gui/resource.rc \
	    -o $(WIN64_GUI_BUILDDIR)/resource.o
	x86_64-w64-mingw32-gcc $(GUI_CORE_FLAGS) -mwindows \
	    $(GUI_CORE_SRCS) \
	    src/frontend/win_gui/main.c \
	    $(WIN64_GUI_BUILDDIR)/resource.o \
	    -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luser32 -lgdi32 \
	    -o $(WIN64_GUI_BUILDDIR)/boltpack-gui.exe
	@echo "  OK    $(WIN64_GUI_BUILDDIR)/boltpack-gui.exe"

# ---- FLTK for Windows cross-compilation ----
FLTK_VERSION      = 1.4.4
FLTK_WIN64_DIR    = compilation/fltk-win64
FLTK_WIN64_LIB    = $(FLTK_WIN64_DIR)/lib/libfltk.a
FLTK_SRC_DIR      = compilation/fltk-src/fltk-$(FLTK_VERSION)
FLTK_DOWNLOAD_URL = https://github.com/fltk/fltk/releases/download/release-$(FLTK_VERSION)/fltk-$(FLTK_VERSION)-source.tar.gz
MINGW64_TOOLCHAIN = compilation/mingw64-toolchain.cmake

# Write the cmake MinGW-w64 toolchain file into compilation/
$(MINGW64_TOOLCHAIN):
	mkdir -p compilation
	@printf 'set(CMAKE_SYSTEM_NAME Windows)\nset(CMAKE_SYSTEM_PROCESSOR x86_64)\n\nset(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)\nset(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)\nset(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)\n\nset(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)\nset(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\nset(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\nset(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n' > $(MINGW64_TOOLCHAIN)
	@echo "  OK    $(MINGW64_TOOLCHAIN)"

# Build FLTK $(FLTK_VERSION) for Windows via MinGW-w64 cross-compilation.
# Downloaded into compilation/fltk-src/, installed into compilation/fltk-win64/.
# Once built, subsequent runs of win64-fltk-gui skip this entirely.
.PHONY: fltk-win64
fltk-win64: $(FLTK_WIN64_LIB)

$(FLTK_WIN64_LIB): $(MINGW64_TOOLCHAIN)
	@echo "--- Building FLTK $(FLTK_VERSION) for Windows (x86-64) ---"
	@which cmake             > /dev/null 2>&1 || { echo "ERROR: cmake is required but not found";                     exit 1; }
	@which x86_64-w64-mingw32-gcc > /dev/null 2>&1 || { echo "ERROR: x86_64-w64-mingw32-gcc not found (install mingw-w64)"; exit 1; }
	@{ which curl > /dev/null 2>&1 || which wget > /dev/null 2>&1; } || { echo "ERROR: curl or wget is required to download FLTK"; exit 1; }
	mkdir -p compilation/fltk-src
	@if [ ! -d "$(FLTK_SRC_DIR)" ]; then \
		echo "Downloading FLTK $(FLTK_VERSION) from GitHub..."; \
		if which curl > /dev/null 2>&1; then \
			curl -L "$(FLTK_DOWNLOAD_URL)" | tar xz -C compilation/fltk-src; \
		else \
			wget -qO- "$(FLTK_DOWNLOAD_URL)" | tar xz -C compilation/fltk-src; \
		fi; \
	else \
		echo "FLTK source already present in compilation/fltk-src/ — skipping download."; \
	fi
	cmake -B "$(FLTK_SRC_DIR)/build-win64" \
	    -S "$(FLTK_SRC_DIR)" \
	    -DCMAKE_TOOLCHAIN_FILE="$(abspath $(MINGW64_TOOLCHAIN))" \
	    -DCMAKE_BUILD_TYPE=Release \
	    -DBUILD_SHARED_LIBS=OFF \
	    -DFLTK_BUILD_SHARED_LIBS=OFF \
	    -DFLTK_USE_SYSTEM_ZLIB=OFF \
	    -DFLTK_USE_SYSTEM_LIBPNG=OFF \
	    -DFLTK_USE_SYSTEM_LIBJPEG=OFF \
	    -DFLTK_BUILD_EXAMPLES=OFF \
	    -DFLTK_BUILD_TEST=OFF \
	    -DFLTK_BUILD_FLUID=OFF \
	    -DFLTK_BUILD_FLTK_OPTIONS=OFF \
	    --install-prefix "$(abspath $(FLTK_WIN64_DIR))"
	cmake --build "$(FLTK_SRC_DIR)/build-win64" --parallel
	cmake --install "$(FLTK_SRC_DIR)/build-win64"
	@echo "  OK    FLTK $(FLTK_VERSION) → $(FLTK_WIN64_DIR)/"

# ---- Win64 FLTK GUI (64-bit, mingw, FLTK 1.4) ----
WIN64_FLTK_GUI_BUILDDIR = build/win64-fltk-gui
WIN64_FLTK_GUI_CORE_OBJS = \
    $(WIN64_FLTK_GUI_BUILDDIR)/archiver.o    \
    $(WIN64_FLTK_GUI_BUILDDIR)/compress.o    \
    $(WIN64_FLTK_GUI_BUILDDIR)/boltlite.o        \
    $(WIN64_FLTK_GUI_BUILDDIR)/fluff.o       \
	$(WIN64_FLTK_GUI_BUILDDIR)/fluff_fast.o  \
    $(WIN64_FLTK_GUI_BUILDDIR)/bp_codepage.o \
    $(WIN64_FLTK_GUI_BUILDDIR)/boltlz.o     \
    $(WIN64_FLTK_GUI_BUILDDIR)/zstd.o        \
    $(WIN64_FLTK_GUI_BUILDDIR)/zstd_wrap.o   \
    $(WIN64_FLTK_GUI_BUILDDIR)/bp_net.o

.PHONY: win64-fltk-gui
win64-fltk-gui:
	@if [ ! -f "$(FLTK_WIN64_LIB)" ]; then \
		printf '\nFLTK $(FLTK_VERSION) not found in $(FLTK_WIN64_DIR)/\n'; \
		printf 'Download and compile it now? [y/N] '; \
		read ans; \
		if [ "$$ans" = "y" ] || [ "$$ans" = "Y" ]; then \
			$(MAKE) fltk-win64; \
		else \
			echo "Aborted. Run 'make fltk-win64' to build FLTK first."; \
			exit 1; \
		fi; \
	fi
	mkdir -p $(WIN64_FLTK_GUI_BUILDDIR)
	x86_64-w64-mingw32-gcc $(GUI_CORE_FLAGS) $(GUI_FAST_FLUFF_CPPFLAGS) $(GUI_FAST_FLUFF_LDFLAGS) -DBOLTPACK_ZSTD -I$(ZSTDDIR) \
	    -c $(COREDIR)/archiver.c    -o $(WIN64_FLTK_GUI_BUILDDIR)/archiver.o
	x86_64-w64-mingw32-gcc $(GUI_CORE_FLAGS) \
	    -c $(COREDIR)/compress.c    -o $(WIN64_FLTK_GUI_BUILDDIR)/compress.o
	x86_64-w64-mingw32-gcc $(GUI_CORE_FLAGS) \
	    -c $(COREDIR)/boltlite.c        -o $(WIN64_FLTK_GUI_BUILDDIR)/boltlite.o
	x86_64-w64-mingw32-gcc $(GUI_CORE_FLAGS) \
	    -c $(COREDIR)/fluff.c       -o $(WIN64_FLTK_GUI_BUILDDIR)/fluff.o
	x86_64-w64-mingw32-gcc $(GUI_FAST_FLUFF_CFLAGS) \
	    -c $(COREDIR)/fluff_fast.c  -o $(WIN64_FLTK_GUI_BUILDDIR)/fluff_fast.o
	x86_64-w64-mingw32-gcc $(GUI_CORE_FLAGS) \
	    -c $(COREDIR)/bp_codepage.c -o $(WIN64_FLTK_GUI_BUILDDIR)/bp_codepage.o
	x86_64-w64-mingw32-gcc $(GUI_CORE_FLAGS) \
	    -O2 -c $(BOLTLZDIR)/boltlz.c      -o $(WIN64_FLTK_GUI_BUILDDIR)/boltlz.o
	x86_64-w64-mingw32-gcc $(ZSTD_CC_FLAGS) -I$(ZSTDDIR) \
	    -c $(ZSTDDIR)/zstd.c        -o $(WIN64_FLTK_GUI_BUILDDIR)/zstd.o
	x86_64-w64-mingw32-gcc $(ZSTD_CC_FLAGS) -DBOLTPACK_ZSTD -I$(COREDIR) -I$(ZSTDDIR) \
	    -c $(COREDIR)/zstd_wrap.c   -o $(WIN64_FLTK_GUI_BUILDDIR)/zstd_wrap.o
	x86_64-w64-mingw32-gcc $(GUI_CORE_FLAGS) -DBOLTPACK_NET -I$(COREDIR) \
	    -c src/frontend/win/bp_net_win.c -o $(WIN64_FLTK_GUI_BUILDDIR)/bp_net.o
	x86_64-w64-mingw32-windres $(GUI_RC_FLAGS) src/frontend/win_gui_fltk/resource.rc \
	    -o $(WIN64_FLTK_GUI_BUILDDIR)/resource.o
	x86_64-w64-mingw32-g++ $(GUI_CORE_FLAGS) $(GUI_FAST_FLUFF_CPPFLAGS) -DBOLTPACK_ZSTD -DBOLTPACK_NET -I$(ZSTDDIR) \
	    -mwindows \
	    -I$(FLTK_WIN64_DIR)/include \
	    $(WIN64_FLTK_GUI_CORE_OBJS) \
	    $(WIN64_FLTK_GUI_BUILDDIR)/resource.o \
	    src/frontend/win_gui_fltk/main.cpp \
	    -L$(FLTK_WIN64_DIR)/lib \
	    $(GUI_FAST_FLUFF_LDFLAGS) \
	    -lfltk_images -lfltk_png -lfltk_jpeg -lfltk_z -lfltk \
	    -lgdi32 -lole32 -luuid -lcomctl32 -lcomdlg32 -lshell32 \
	    -lgdiplus -lwinmm -lwinspool -lws2_32 -liphlpapi \
	    -static \
	    -o $(WIN64_FLTK_GUI_BUILDDIR)/boltpack-gui.exe
	@echo "  OK    $(WIN64_FLTK_GUI_BUILDDIR)/boltpack-gui.exe"

# ---- Win64 FLTK GUI installer (NSIS) ----
WIN64_FLTK_NSI = packaging/windows-installer/win64-fltk/boltpack-setup.nsi

.PHONY: win64-fltk-installer
win64-fltk-installer: win64-fltk-gui win64-exe win64-totalcmd-plugin
	@which makensis > /dev/null 2>&1 || { echo "ERROR: makensis not found (install NSIS)"; exit 1; }
	mkdir -p staging/win64/package
	cp LICENSE.md staging/win64/package/LICENSE
	cp templates/README-windows.txt staging/win64/package/README.txt
	cp RELEASE-NOTES.md staging/win64/package/RELEASE-NOTES.txt
	makensis \
	    -DBUILD_DIR="$(abspath $(WIN64_FLTK_GUI_BUILDDIR))" \
	    -DCLI_BUILD_DIR="$(abspath $(WIN64_BUILDDIR))" \
	    -DPACKAGE_DIR="$(abspath staging/win64/package)" \
	    -DRESOURCES_DIR="$(abspath resources/pics)" \
	    -DBP_VERSION="$(BP_VERSION)" \
	    $(WIN64_FLTK_NSI)
	@echo "  OK    $(WIN64_FLTK_GUI_BUILDDIR)/boltpack-$(BP_VERSION)-win64-setup.exe"

# ---- FLTK for Windows 32-bit cross-compilation ----
FLTK_WIN32_DIR    = compilation/fltk-win32
FLTK_WIN32_LIB    = $(FLTK_WIN32_DIR)/lib/libfltk.a
MINGW32_TOOLCHAIN = compilation/mingw32-toolchain.cmake

# Write the cmake MinGW i686 toolchain file into compilation/
$(MINGW32_TOOLCHAIN):
	mkdir -p compilation
	@printf 'set(CMAKE_SYSTEM_NAME Windows)\nset(CMAKE_SYSTEM_PROCESSOR x86)\n\nset(CMAKE_C_COMPILER i686-w64-mingw32-gcc)\nset(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)\nset(CMAKE_RC_COMPILER i686-w64-mingw32-windres)\n\nset(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)\nset(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\nset(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\nset(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n' > $(MINGW32_TOOLCHAIN)
	@echo "  OK    $(MINGW32_TOOLCHAIN)"

# Build FLTK $(FLTK_VERSION) for Windows 32-bit via MinGW i686 cross-compilation.
# Reuses the same source tree as fltk-win64; builds into a separate build-win32/ dir.
.PHONY: fltk-win32
fltk-win32: $(FLTK_WIN32_LIB)

$(FLTK_WIN32_LIB): $(MINGW32_TOOLCHAIN)
	@echo "--- Building FLTK $(FLTK_VERSION) for Windows (x86-32) ---"
	@which cmake                > /dev/null 2>&1 || { echo "ERROR: cmake is required but not found";                    exit 1; }
	@which i686-w64-mingw32-gcc > /dev/null 2>&1 || { echo "ERROR: i686-w64-mingw32-gcc not found (install mingw-w64)"; exit 1; }
	@{ which curl > /dev/null 2>&1 || which wget > /dev/null 2>&1; } || { echo "ERROR: curl or wget is required to download FLTK"; exit 1; }
	mkdir -p compilation/fltk-src
	@if [ ! -d "$(FLTK_SRC_DIR)" ]; then \
		echo "Downloading FLTK $(FLTK_VERSION) from GitHub..."; \
		if which curl > /dev/null 2>&1; then \
			curl -L "$(FLTK_DOWNLOAD_URL)" | tar xz -C compilation/fltk-src; \
		else \
			wget -qO- "$(FLTK_DOWNLOAD_URL)" | tar xz -C compilation/fltk-src; \
		fi; \
	else \
		echo "FLTK source already present in compilation/fltk-src/ — skipping download."; \
	fi
	cmake -B "$(FLTK_SRC_DIR)/build-win32" \
	    -S "$(FLTK_SRC_DIR)" \
	    -DCMAKE_TOOLCHAIN_FILE="$(abspath $(MINGW32_TOOLCHAIN))" \
	    -DCMAKE_BUILD_TYPE=Release \
	    -DBUILD_SHARED_LIBS=OFF \
	    -DFLTK_BUILD_SHARED_LIBS=OFF \
	    -DFLTK_USE_SYSTEM_ZLIB=OFF \
	    -DFLTK_USE_SYSTEM_LIBPNG=OFF \
	    -DFLTK_USE_SYSTEM_LIBJPEG=OFF \
	    -DFLTK_BUILD_EXAMPLES=OFF \
	    -DFLTK_BUILD_TEST=OFF \
	    -DFLTK_BUILD_FLUID=OFF \
	    -DFLTK_BUILD_FLTK_OPTIONS=OFF \
	    --install-prefix "$(abspath $(FLTK_WIN32_DIR))"
	cmake --build "$(FLTK_SRC_DIR)/build-win32" --parallel
	cmake --install "$(FLTK_SRC_DIR)/build-win32"
	@echo "  OK    FLTK $(FLTK_VERSION) → $(FLTK_WIN32_DIR)/"

# ---- Win32 FLTK GUI (32-bit, mingw, FLTK 1.4) ----
WIN32_FLTK_GUI_BUILDDIR = build/win32-fltk-gui
WIN32_FLTK_GUI_CORE_OBJS = \
    $(WIN32_FLTK_GUI_BUILDDIR)/archiver.o    \
    $(WIN32_FLTK_GUI_BUILDDIR)/compress.o    \
    $(WIN32_FLTK_GUI_BUILDDIR)/boltlite.o        \
    $(WIN32_FLTK_GUI_BUILDDIR)/fluff.o       \
	$(WIN32_FLTK_GUI_BUILDDIR)/fluff_fast.o  \
    $(WIN32_FLTK_GUI_BUILDDIR)/bp_codepage.o \
    $(WIN32_FLTK_GUI_BUILDDIR)/boltlz.o     \
    $(WIN32_FLTK_GUI_BUILDDIR)/bp_net.o

.PHONY: win32-fltk-gui
win32-fltk-gui:
	@if [ ! -f "$(FLTK_WIN32_LIB)" ]; then \
		printf '\nFLTK $(FLTK_VERSION) not found in $(FLTK_WIN32_DIR)/\n'; \
		printf 'Download and compile it now? [y/N] '; \
		read ans; \
		if [ "$$ans" = "y" ] || [ "$$ans" = "Y" ]; then \
			$(MAKE) fltk-win32; \
		else \
			echo "Aborted. Run 'make fltk-win32' to build FLTK first."; \
			exit 1; \
		fi; \
	fi
	mkdir -p $(WIN32_FLTK_GUI_BUILDDIR)
	i686-w64-mingw32-gcc $(GUI_CORE_FLAGS) $(GUI_FAST_FLUFF_CPPFLAGS) $(GUI_FAST_FLUFF_LDFLAGS) -m32 \
	    -c $(COREDIR)/archiver.c    -o $(WIN32_FLTK_GUI_BUILDDIR)/archiver.o
	i686-w64-mingw32-gcc $(GUI_CORE_FLAGS) -m32 \
	    -c $(COREDIR)/compress.c    -o $(WIN32_FLTK_GUI_BUILDDIR)/compress.o
	i686-w64-mingw32-gcc $(GUI_CORE_FLAGS) -m32 \
	    -c $(COREDIR)/boltlite.c        -o $(WIN32_FLTK_GUI_BUILDDIR)/boltlite.o
	i686-w64-mingw32-gcc $(GUI_CORE_FLAGS) -m32 \
	    -c $(COREDIR)/fluff.c       -o $(WIN32_FLTK_GUI_BUILDDIR)/fluff.o
	i686-w64-mingw32-gcc $(GUI_FAST_FLUFF_CFLAGS) -m32 \
	    -c $(COREDIR)/fluff_fast.c  -o $(WIN32_FLTK_GUI_BUILDDIR)/fluff_fast.o
	i686-w64-mingw32-gcc $(GUI_CORE_FLAGS) -m32 \
	    -c $(COREDIR)/bp_codepage.c -o $(WIN32_FLTK_GUI_BUILDDIR)/bp_codepage.o
	i686-w64-mingw32-gcc $(GUI_CORE_FLAGS) -m32 \
	    -O2 -c $(BOLTLZDIR)/boltlz.c      -o $(WIN32_FLTK_GUI_BUILDDIR)/boltlz.o
	i686-w64-mingw32-gcc $(GUI_CORE_FLAGS) -m32 -DBOLTPACK_NET -I$(COREDIR) \
	    -c src/frontend/win/bp_net_win.c -o $(WIN32_FLTK_GUI_BUILDDIR)/bp_net.o
	i686-w64-mingw32-windres $(GUI_RC_FLAGS) src/frontend/win_gui_fltk/resource.rc \
	    -o $(WIN32_FLTK_GUI_BUILDDIR)/resource.o
	i686-w64-mingw32-g++ $(GUI_CORE_FLAGS) $(GUI_FAST_FLUFF_CPPFLAGS) -m32 -DBOLTPACK_NET \
	    -mwindows \
	    -I$(FLTK_WIN32_DIR)/include \
	    $(WIN32_FLTK_GUI_CORE_OBJS) \
	    $(WIN32_FLTK_GUI_BUILDDIR)/resource.o \
	    src/frontend/win_gui_fltk/main.cpp \
	    -L$(FLTK_WIN32_DIR)/lib \
	    $(GUI_FAST_FLUFF_LDFLAGS) \
	    -lfltk_images -lfltk_png -lfltk_jpeg -lfltk_z -lfltk \
	    -lgdi32 -lole32 -luuid -lcomctl32 -lcomdlg32 -lshell32 \
	    -lgdiplus -lwinmm -lwinspool -lws2_32 -liphlpapi \
	    -static \
	    -o $(WIN32_FLTK_GUI_BUILDDIR)/boltpack-gui.exe
	@echo "  OK    $(WIN32_FLTK_GUI_BUILDDIR)/boltpack-gui.exe"

# ---- Win32 FLTK GUI installer (NSIS) ----
WIN32_FLTK_NSI = packaging/windows-installer/win32-fltk/boltpack-setup.nsi

.PHONY: win32-fltk-installer
win32-fltk-installer: win32-fltk-gui win32-exe win32-totalcmd-plugin
	@which makensis > /dev/null 2>&1 || { echo "ERROR: makensis not found (install NSIS)"; exit 1; }
	mkdir -p staging/win32/package
	cp LICENSE.md staging/win32/package/LICENSE
	cp templates/README-windows.txt staging/win32/package/README.txt
	cp RELEASE-NOTES.md staging/win32/package/RELEASE-NOTES.txt
	makensis \
	    -DBUILD_DIR="$(abspath $(WIN32_FLTK_GUI_BUILDDIR))" \
	    -DCLI_BUILD_DIR="$(abspath $(WIN32_BUILDDIR))" \
	    -DPACKAGE_DIR="$(abspath staging/win32/package)" \
	    -DRESOURCES_DIR="$(abspath resources/pics)" \
	    -DBP_VERSION="$(BP_VERSION)" \
	    $(WIN32_FLTK_NSI)
	@echo "  OK    $(WIN32_FLTK_GUI_BUILDDIR)/boltpack-$(BP_VERSION)-win32-setup.exe"

# ---- Win32 GUI DLL (for C# WinForms P/Invoke) ----
WIN32_DLL_BUILDDIR = build/win32-gui-dll

.PHONY: win32-gui-dll
win32-gui-dll:
	mkdir -p $(WIN32_DLL_BUILDDIR)
	i686-w64-mingw32-gcc $(GUI_CORE_FLAGS) -m32 -shared \
	    $(GUI_CORE_SRCS) \
	    $(COREDIR)/boltpack_dll.c \
	    -o $(WIN32_DLL_BUILDDIR)/boltpack_core.dll
	@echo "  OK    $(WIN32_DLL_BUILDDIR)/boltpack_core.dll"

# ---- Win64 GUI DLL (for C# WinForms P/Invoke) ----
WIN64_DLL_BUILDDIR = build/win64-gui-dll

.PHONY: win64-gui-dll
win64-gui-dll:
	mkdir -p $(WIN64_DLL_BUILDDIR)
	x86_64-w64-mingw32-gcc $(GUI_CORE_FLAGS) -shared \
	    $(GUI_CORE_SRCS) \
	    $(COREDIR)/boltpack_dll.c \
	    -o $(WIN64_DLL_BUILDDIR)/boltpack_core.dll
	@echo "  OK    $(WIN64_DLL_BUILDDIR)/boltpack_core.dll"

# ---- macOS Cocoa GUI (ARM64 via osxcross) ----
MACOSX_GUI_BUILDDIR = build/macosx-gui

# ---- macOS legacy Cocoa GUI (10.5 PPC / 10.5-10.8 Intel) ----
MACOSX_LEGACY_GUI_PPC_BUILDDIR = build/macosx-legacy-gui-ppc
MACOSX_LEGACY_GUI_INTEL_BUILDDIR = build/macosx-legacy-gui-intel
MACOSX_LEGACY_GUI_FLAGS = $(GUI_CORE_FLAGS) -ObjC -mmacosx-version-min=10.5
MACOSX_LEGACY_PPC_CC ?= powerpc-apple-darwin9-gcc
MACOSX_LEGACY_INTEL_CC ?= i686-apple-darwin9-gcc
MACOSX_LEGACY_INTEL_ARCH ?= i386

ifeq ($(MACOSX_LEGACY_INTEL_ARCH),x86_64)
MACOSX_LEGACY_INTEL_CPU = BP_CPU_X86_64
else
MACOSX_LEGACY_INTEL_CPU = BP_CPU_X86
endif

.PHONY: macosx-legacy-gui-ppc macosx-legacy-gui-intel
macosx-legacy-gui-ppc:
	OBJC="$(MACOSX_LEGACY_PPC_CC)" \
	LEGACY_GUI_CFLAGS="$(MACOSX_LEGACY_GUI_FLAGS)" \
	LEGACY_PPC_GUI_OUTDIR="$(MACOSX_LEGACY_GUI_PPC_BUILDDIR)" \
	sh scripts/macos/build-native.sh legacy-ppc
	@echo "  OK    $(MACOSX_LEGACY_GUI_PPC_BUILDDIR)/BoltPack.app"

macosx-legacy-gui-intel:
	OBJC="$(MACOSX_LEGACY_INTEL_CC)" \
	LEGACY_GUI_CFLAGS="$(MACOSX_LEGACY_GUI_FLAGS)" \
	LEGACY_INTEL_ARCH="$(MACOSX_LEGACY_INTEL_ARCH)" \
	LEGACY_INTEL_GUI_OUTDIR="$(MACOSX_LEGACY_GUI_INTEL_BUILDDIR)" \
	sh scripts/macos/build-native.sh legacy-intel
	@echo "  OK    $(MACOSX_LEGACY_GUI_INTEL_BUILDDIR)/BoltPack.app"

.PHONY: macosx-gui
macosx-gui:
	mkdir -p $(MACOSX_GUI_BUILDDIR)
	arm64-apple-macosx11-clang $(GUI_FAST_FLUFF_CFLAGS) \
	    -c $(COREDIR)/fluff_fast.c \
	    -o $(MACOSX_GUI_BUILDDIR)/fluff_fast.o
	arm64-apple-macosx11-clang -ObjC -fobjc-arc \
	    $(GUI_CORE_FLAGS) $(GUI_FAST_FLUFF_CPPFLAGS) \
	    $(GUI_CORE_SRCS) \
	    $(MACOSX_GUI_BUILDDIR)/fluff_fast.o \
	    src/frontend/macosx_gui/main.m \
	    -framework Cocoa $(GUI_FAST_FLUFF_LDFLAGS) \
	    -o $(MACOSX_GUI_BUILDDIR)/BoltPack
	@echo "  OK    $(MACOSX_GUI_BUILDDIR)/BoltPack"

# ---- Atari GEM GUI (68k via Riviere's cross-toolchain) ----
ATARI_GUI_BUILDDIR = $(ATARI_BUILDDIR)

.PHONY: atari-gui
atari-gui:
	mkdir -p $(ATARI_GUI_BUILDDIR)
	rm -f $(ATARI_GUI_BUILDDIR)/boltpack.app
	$(ATARI_CC) \
	    -Wall -Wextra -Wno-unused-function -O2 \
	    -I$(COREDIR) -I$(BOLTLZDIR) -I$(GUI_COMMON) \
	    $(GUI_CORE_SRCS) \
	    src/frontend/atari_gui/main.c \
	    -lgem \
	    -o $(ATARI_GUI_BUILDDIR)/boltpack.prg
	@echo "  OK    $(ATARI_GUI_BUILDDIR)/boltpack.prg"

atari-gui-create-test: modern linuxmini atari-gui
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	python3 test/check_hatari_gui_create.py --tos "$(HATARI_TOS)" \
	    --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg \
	    --linux-cli $(TARGET) --mini-cli build/linux32mini/bpmini

atari-gui-create-fat12-test: modern linuxmini atari-gui
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	python3 test/check_hatari_gui_create.py --fat12 --tos "$(HATARI_TOS)" \
	    --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg \
	    --linux-cli $(TARGET) --mini-cli build/linux32mini/bpmini

atari-gui-unicode-refusal-test: modern atari-gui
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	python3 test/check_hatari_gui_unicode_refusal.py --tos "$(HATARI_TOS)" \
	    --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)

atari-gui-menu-pointer-test: modern atari-gui
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	xvfb-run -a python3 test/check_hatari_menu_pointer.py --tos "$(HATARI_TOS)" \
	    --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg
	xvfb-run -a python3 test/check_hatari_menu_pointer.py --action help-about \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg
	xvfb-run -a python3 test/check_hatari_menu_pointer.py --action help-documentation \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg
	xvfb-run -a python3 test/check_hatari_menu_pointer.py --action archive-validate \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)
	xvfb-run -a python3 test/check_hatari_menu_pointer.py --action archive-information \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)
	xvfb-run -a python3 test/check_hatari_menu_pointer.py --action archive-close \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)
	xvfb-run -a python3 test/check_hatari_menu_pointer.py --action archive-extract \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)
	xvfb-run -a python3 test/check_hatari_menu_pointer.py --action archive-quit \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)
	xvfb-run -a python3 test/check_hatari_menu_pointer.py --action compression-stored \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg
	xvfb-run -a python3 test/check_hatari_menu_pointer.py --action compression-boltlite \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg
	xvfb-run -a python3 test/check_hatari_menu_pointer.py --action compression-boltlz \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg
	xvfb-run -a python3 test/check_hatari_menu_pointer.py --action compression-fluff \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg
	xvfb-run -a python3 test/check_hatari_menu_pointer.py --action compression-profile \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg

atari-gui-cancel-test: modern atari-gui
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	xvfb-run -a python3 test/check_hatari_gui_cancel.py --tos "$(HATARI_TOS)" \
	    --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)

atari-gui-time-range-test: modern atari-gui
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	xvfb-run -a python3 test/check_hatari_gui_mtime.py --tos "$(HATARI_TOS)" \
	    --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)

atari-gui-active-cancel-test: modern atari-gui
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	xvfb-run -a python3 test/check_hatari_gui_cancel.py --active-extraction \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)

atari-gui-interruption-test: modern atari-gui
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	xvfb-run -a python3 test/check_hatari_gui_cancel.py --interrupt-extraction \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)

atari-gui-stage-substitution-test: modern atari-gui
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	xvfb-run -a python3 test/check_hatari_gui_cancel.py --substitute-stage \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)

atari-gui-parent-substitution-test: modern atari-gui
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	xvfb-run -a python3 test/check_hatari_gui_cancel.py --substitute-parent \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)

ATARI_GUI_ROLLBACK_BIN = $(ATARI_GUI_BUILDDIR)/boltpack-rollback.prg
atari-gui-publish-rollback-test: modern
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	mkdir -p $(ATARI_GUI_BUILDDIR)
	$(ATARI_CC) -Wall -Wextra -Wno-unused-function -O2 \
	    -DBP_ATARI_GUI_TEST_FAIL_AFTER_BACKUP \
	    -I$(COREDIR) -I$(BOLTLZDIR) -I$(GUI_COMMON) \
	    $(GUI_CORE_SRCS) src/frontend/atari_gui/main.c -lgem \
	    -o $(ATARI_GUI_ROLLBACK_BIN)
	xvfb-run -a python3 test/check_hatari_gui_rollback.py \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_ROLLBACK_BIN) --linux-cli $(TARGET)

ATARI_GUI_RECOVERY_BIN = $(ATARI_GUI_BUILDDIR)/boltpack-recovery-test.prg
atari-gui-recovery-path-test: modern
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	mkdir -p $(ATARI_GUI_BUILDDIR)
	$(ATARI_CC) -Wall -Wextra -Wno-unused-function -O2 \
	    -DBP_ATARI_GUI_TEST_FAIL_AFTER_BACKUP -DBP_ATARI_GUI_TEST_BLOCK_ROLLBACK \
	    -I$(COREDIR) -I$(BOLTLZDIR) -I$(GUI_COMMON) \
	    $(GUI_CORE_SRCS) src/frontend/atari_gui/main.c -lgem \
	    -o $(ATARI_GUI_RECOVERY_BIN)
	xvfb-run -a python3 test/check_hatari_gui_rollback.py --expect-recovery \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_RECOVERY_BIN) --linux-cli $(TARGET)

ATARI_GUI_INTERRUPT_BIN = $(ATARI_GUI_BUILDDIR)/boltpack-interrupt-backup.prg
ATARI_GUI_RECOVER_RECORD_BIN = $(ATARI_GUI_BUILDDIR)/boltpack-recovery-record.prg
atari-gui-interrupt-after-backup-test: modern atari-gui
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	mkdir -p $(ATARI_GUI_BUILDDIR)
	$(ATARI_CC) -Wall -Wextra -Wno-unused-function -O2 \
	    -DBP_ATARI_GUI_TEST_PAUSE_AFTER_BACKUP \
	    -I$(COREDIR) -I$(BOLTLZDIR) -I$(GUI_COMMON) \
	    $(GUI_CORE_SRCS) src/frontend/atari_gui/main.c -lgem \
	    -o $(ATARI_GUI_INTERRUPT_BIN)
	$(ATARI_CC) -Wall -Wextra -Wno-unused-function -O2 \
	    -DBP_ATARI_GUI_TEST_RECORD_RECOVERY \
	    -I$(COREDIR) -I$(BOLTLZDIR) -I$(GUI_COMMON) \
	    $(GUI_CORE_SRCS) src/frontend/atari_gui/main.c -lgem \
	    -o $(ATARI_GUI_RECOVER_RECORD_BIN)
	xvfb-run -a python3 test/check_hatari_gui_interruption.py \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_INTERRUPT_BIN) \
	    --recovery-gui $(ATARI_GUI_RECOVER_RECORD_BIN) \
	    --final-gui $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)

ATARI_GUI_MEDIA_ERROR_BIN = $(ATARI_GUI_BUILDDIR)/boltpack-media-error.prg
ATARI_GUI_MEDIA_ERROR_KEYS ?= a 28
ATARI_GUI_MEDIA_ERROR_EVENTS ?=
atari-gui-media-error-test: modern
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	mkdir -p $(ATARI_GUI_BUILDDIR)
	$(ATARI_CC) -Wall -Wextra -Wno-unused-function -O2 \
	    -DBP_ATARI_GUI_TEST_PAUSE_AFTER_BACKUP \
	    -DBP_ATARI_GUI_TEST_WAIT_FOR_RELEASE \
	    -I$(COREDIR) -I$(BOLTLZDIR) -I$(GUI_COMMON) \
	    $(GUI_CORE_SRCS) src/frontend/atari_gui/main.c -lgem \
	    -o $(ATARI_GUI_MEDIA_ERROR_BIN)
	xvfb-run -a python3 test/check_hatari_gui_media_error.py \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_MEDIA_ERROR_BIN) --linux-cli $(TARGET) \
	    --prompt-keys $(ATARI_GUI_MEDIA_ERROR_KEYS) \
	    $(if $(ATARI_GUI_MEDIA_ERROR_EVENTS),--prompt-events $(ATARI_GUI_MEDIA_ERROR_EVENTS),)

atari-gui-stage-recovery-test: modern atari-gui
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	xvfb-run -a python3 test/check_hatari_stage_recovery.py \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --final-gui $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)

atari-gui-journal-path-test: modern atari-gui
	@test -n "$(HATARI_TOS)" || { echo "Set HATARI_TOS to a TOS ROM image" >&2; exit 1; }
	xvfb-run -a python3 test/check_hatari_journal_path.py \
	    --tos "$(HATARI_TOS)" --hatari "$(HATARI)" --hconsole "$(HCONSOLE)" \
	    --gui-prg $(ATARI_GUI_BUILDDIR)/boltpack.prg --linux-cli $(TARGET)

EMUTOS_ROM ?=
FREEMINT_ROOT ?= build/freemint-probe/diskroot
ATARI_GUI_FREEMINT_PROBE_BIN = $(ATARI_GUI_BUILDDIR)/freemint-cleanup-test.prg
atari-gui-freemint-cleanup-test:
	@test -n "$(EMUTOS_ROM)" || { echo "Set EMUTOS_ROM to an EmuTOS ROM image" >&2; exit 1; }
	@test -d "$(FREEMINT_ROOT)/AUTO" -a -d "$(FREEMINT_ROOT)/MINT" || { echo "Set FREEMINT_ROOT to a FreeMiNT boot tree" >&2; exit 1; }
	mkdir -p $(ATARI_GUI_BUILDDIR)
	$(ATARI_CC) $(ATARI_CFLAGS) -std=gnu99 -Wno-unused-variable \
	    -DBP_ATARI_GUI_FREEMINT_CLEANUP_TEST $(BP_VER_DEFS) \
	    -I$(COREDIR) -I$(BOLTLZDIR) -I$(GUI_COMMON) \
	    $(GUI_CORE_SRCS) src/frontend/atari_gui/main.c -lgem \
	    -o $(ATARI_GUI_FREEMINT_PROBE_BIN)
	python3 test/check_hatari_freemint_cleanup.py \
	    --emutos "$(EMUTOS_ROM)" --freemint-root "$(FREEMINT_ROOT)" \
	    --probe-prg $(ATARI_GUI_FREEMINT_PROBE_BIN) \
	    --hatari "$(HATARI)" --hconsole "$(HCONSOLE)"


# ---- Amiga GadTools GUI (68k via bebbo's amiga-gcc) ----
AMIGA_GUI_BUILDDIR = $(AMIGA68K_BUILDDIR)

.PHONY: amiga-gui
amiga-gui:
	mkdir -p $(AMIGA_GUI_BUILDDIR)
	rm -f $(AMIGA_GUI_BUILDDIR)/BoltPack
	/opt/amiga/bin/m68k-amigaos-gcc -noixemul \
	    -Wall -Wextra -Wno-unused-function -O2 \
	    -I$(COREDIR) -I$(BOLTLZDIR) -I$(GUI_COMMON) \
	    $(GUI_CORE_SRCS) \
	    src/frontend/amiga_gui/main.c \
	    -lm \
	    -o $(AMIGA_GUI_BUILDDIR)/boltpack-gui
	@echo "  OK    $(AMIGA_GUI_BUILDDIR)/boltpack-gui"

# ---- Haiku BeAPI GUI (x86_64) ----
HAIKU_GUI_BUILDDIR = build/haiku-gui
HAIKU_GUI_CORE_OBJS = \
	$(HAIKU_GUI_BUILDDIR)/archiver.o    \
	$(HAIKU_GUI_BUILDDIR)/compress.o    \
	$(HAIKU_GUI_BUILDDIR)/boltlite.o        \
	$(HAIKU_GUI_BUILDDIR)/fluff.o       \
	$(HAIKU_GUI_BUILDDIR)/bp_codepage.o \
	$(HAIKU_GUI_BUILDDIR)/boltlz.o
HAIKU_GUI_FAST_FLUFF_OBJ = $(HAIKU_GUI_BUILDDIR)/fluff_fast.o
HAIKU_GUI_PRIVATE_INCLUDE ?= /opt/sil/x86_64-unknown-haiku/include/private/interface

.PHONY: haiku-gui
haiku-gui:
	set -e
	mkdir -p $(HAIKU_GUI_BUILDDIR)
	x86_64-unknown-haiku-gcc $(GUI_CORE_FLAGS) $(GUI_FAST_FLUFF_CPPFLAGS) $(GUI_FAST_FLUFF_LDFLAGS) \
	    -c $(COREDIR)/archiver.c -o $(HAIKU_GUI_BUILDDIR)/archiver.o
	x86_64-unknown-haiku-gcc $(GUI_CORE_FLAGS) \
	    -c $(COREDIR)/compress.c -o $(HAIKU_GUI_BUILDDIR)/compress.o
	x86_64-unknown-haiku-gcc $(GUI_CORE_FLAGS) \
	    -c $(COREDIR)/boltlite.c -o $(HAIKU_GUI_BUILDDIR)/boltlite.o
	x86_64-unknown-haiku-gcc $(GUI_CORE_FLAGS) $(GUI_FAST_FLUFF_CPPFLAGS) \
	    -c $(COREDIR)/fluff.c -o $(HAIKU_GUI_BUILDDIR)/fluff.o
	x86_64-unknown-haiku-gcc $(GUI_CORE_FLAGS) \
	    -c $(COREDIR)/bp_codepage.c -o $(HAIKU_GUI_BUILDDIR)/bp_codepage.o
	x86_64-unknown-haiku-gcc $(GUI_CORE_FLAGS) \
	    -O2 -c $(BOLTLZDIR)/boltlz.c -o $(HAIKU_GUI_BUILDDIR)/boltlz.o
	x86_64-unknown-haiku-gcc $(GUI_FAST_FLUFF_CFLAGS) \
	    -c $(COREDIR)/fluff_fast.c \
	    -o $(HAIKU_GUI_FAST_FLUFF_OBJ)
	x86_64-unknown-haiku-g++ \
	    $(GUI_CORE_FLAGS) $(GUI_FAST_FLUFF_CPPFLAGS) -I$(HAIKU_GUI_PRIVATE_INCLUDE) \
	    $(HAIKU_GUI_CORE_OBJS) \
	    $(HAIKU_GUI_FAST_FLUFF_OBJ) \
	    src/frontend/haiku_gui/main.cpp \
	    $(GUI_FAST_FLUFF_LDFLAGS) \
	    -lbe -lcolumnlistview -ltracker \
	    -o $(HAIKU_GUI_BUILDDIR)/BoltPack
	@echo "  OK    $(HAIKU_GUI_BUILDDIR)/BoltPack"

# ---- All GUI frontends ----
.PHONY: gui-all
gui-all: linux-gui win32-gui win64-gui win32-gui-dll win64-gui-dll \
         macosx-gui atari-gui amiga-gui haiku-gui
	@echo "All GUI builds complete."
# ============================================================================
# Unit tests (C89, run on Linux host)
# ============================================================================
#
# Four standalone test binaries, each linked with the core modules it exercises.
# Run all with: make unit-test
# Run with AddressSanitizer+UBSan: make unit-test-asan

UNIT_TEST_DIR   = test/unit
UNIT_BUILD_DIR  = build/unit-test
# Adler-32 is retired from the shipping tools (CRC-32 is the only compiled
# archive checksum) but its known-answer unit tests still exercise the code,
# so the unit-test builds define BP_ENABLE_ADLER32 to compile bp_checksum /
# bp_mini_checksum.  The frontends never define it.
UNIT_CFLAGS     = $(CFLAGS) $(BP_VER_DEFS) -DBP_ENABLE_ADLER32  -I$(COREDIR) -I$(UNIT_TEST_DIR)
UNIT_ASAN_FLAGS = $(filter-out -O0 -O1 -O2 -O3,$(DEBUG_CFLAGS)) -O2 $(BP_VER_DEFS) -DBP_ENABLE_ADLER32 -I$(COREDIR) -I$(UNIT_TEST_DIR)

# Source lists shared between normal and ASAN variants
UNIT_COMMON_SRCS  = $(BOLTLZDIR)/boltlz.c $(COREDIR)/boltlite.c
UNIT_CORE_SRCS    = $(COREDIR)/compress.c $(COREDIR)/bp_codepage.c \
                    $(COREDIR)/archiver.c  $(COREDIR)/fluff.c
UNIT_MINI_SRCS    = $(COREDIR)/mini/compress.c $(COREDIR)/mini/archiver.c \
                    $(COREDIR)/mini/boltlz_decomp.c $(COREDIR)/boltlite.c

.PHONY: unit-test unit-test-asan

unit-test: $(UNIT_BUILD_DIR)/test_checksum \
           $(UNIT_BUILD_DIR)/test_fluff \
           $(UNIT_BUILD_DIR)/test_compress \
           $(UNIT_BUILD_DIR)/test_codepage \
           $(UNIT_BUILD_DIR)/test_mini_cp437 \
           $(UNIT_BUILD_DIR)/test_mini_cp850 \
           $(UNIT_BUILD_DIR)/test_archive_format \
           $(UNIT_BUILD_DIR)/mini_test_compress \
           $(BOLTLZ_BUILD_DIR)/test_boltlz
	@ok=1; \
	for t in $^; do \
	    printf '\nRunning %s...\n' "$$t"; \
	    "$$t" || ok=0; \
	done; \
	test "$$ok" -eq 1

unit-test-asan: $(UNIT_BUILD_DIR)/test_checksum_asan \
                $(UNIT_BUILD_DIR)/test_fluff_asan \
                $(UNIT_BUILD_DIR)/test_compress_asan \
                $(UNIT_BUILD_DIR)/test_codepage_asan \
                $(UNIT_BUILD_DIR)/test_archive_format_asan \
                $(UNIT_BUILD_DIR)/mini_test_compress_asan \
                $(BOLTLZ_BUILD_DIR)/test_boltlz_asan
	@ok=1; \
	for t in $^; do \
	    printf '\nRunning %s...\n' "$$t"; \
	    "$$t" || ok=0; \
	done; \
	test "$$ok" -eq 1

# --- test_fluff ---
$(UNIT_BUILD_DIR)/test_fluff: $(UNIT_TEST_DIR)/test_fluff.c \
    $(COREDIR)/fluff.c $(COREDIR)/fluff.h $(UNIT_TEST_DIR)/test_runner.h \
    test/reference/fluff_legacy_fixtures.h | $(UNIT_BUILD_DIR)
	$(CC) $(UNIT_CFLAGS) -O3 $(FLUFF_NATIVE_FLAGS) -o $@ $(UNIT_TEST_DIR)/test_fluff.c

$(UNIT_BUILD_DIR)/test_fluff_asan: $(UNIT_TEST_DIR)/test_fluff.c \
    $(COREDIR)/fluff.c $(COREDIR)/fluff.h $(UNIT_TEST_DIR)/test_runner.h \
    test/reference/fluff_legacy_fixtures.h | $(UNIT_BUILD_DIR)
	$(CC) $(UNIT_ASAN_FLAGS) -o $@ $(UNIT_TEST_DIR)/test_fluff.c

# --- test_checksum ---
$(UNIT_BUILD_DIR)/test_checksum: \
    $(UNIT_TEST_DIR)/test_checksum.c \
    $(COREDIR)/compress.c \
    $(BOLTLZDIR)/boltlz.c \
    $(COREDIR)/boltlite.c \
    | $(UNIT_BUILD_DIR)
	$(CC) $(UNIT_CFLAGS) -o $@ \
	    $(UNIT_TEST_DIR)/test_checksum.c \
	    $(COREDIR)/compress.c $(BOLTLZDIR)/boltlz.c $(COREDIR)/boltlite.c

$(UNIT_BUILD_DIR)/test_checksum_asan: \
    $(UNIT_TEST_DIR)/test_checksum.c \
    $(COREDIR)/compress.c \
    $(COREDIR)/boltlite.c \
    $(BOLTLZDIR)/boltlz.c \
    | $(UNIT_BUILD_DIR)
	$(CC) $(UNIT_ASAN_FLAGS) $(DEBUG_LDFLAGS) -o $@ \
	    $(UNIT_TEST_DIR)/test_checksum.c \
	    $(COREDIR)/compress.c $(COREDIR)/boltlite.c $(BOLTLZDIR)/boltlz.c

# --- test_compress ---
$(UNIT_BUILD_DIR)/test_compress: \
    $(UNIT_TEST_DIR)/test_compress.c \
    $(COREDIR)/compress.c \
    $(BOLTLZDIR)/boltlz.c \
    $(COREDIR)/boltlite.c \
    | $(UNIT_BUILD_DIR)
	$(CC) $(UNIT_CFLAGS) -o $@ \
	    $(UNIT_TEST_DIR)/test_compress.c \
	    $(COREDIR)/compress.c $(BOLTLZDIR)/boltlz.c $(COREDIR)/boltlite.c

$(UNIT_BUILD_DIR)/test_compress_asan: \
    $(UNIT_TEST_DIR)/test_compress.c \
    $(COREDIR)/compress.c \
    $(BOLTLZDIR)/boltlz.c \
    $(COREDIR)/boltlite.c \
    | $(UNIT_BUILD_DIR)
	$(CC) $(UNIT_ASAN_FLAGS) $(DEBUG_LDFLAGS) -o $@ \
	    $(UNIT_TEST_DIR)/test_compress.c \
	    $(COREDIR)/compress.c $(BOLTLZDIR)/boltlz.c $(COREDIR)/boltlite.c

# --- test_codepage ---
$(UNIT_BUILD_DIR)/test_codepage: \
    $(UNIT_TEST_DIR)/test_codepage.c \
    $(COREDIR)/bp_codepage.c \
    $(COREDIR)/bp_sjis_table.h \
    | $(UNIT_BUILD_DIR)
	$(CC) $(UNIT_CFLAGS) -o $@ \
	    $(UNIT_TEST_DIR)/test_codepage.c \
	    $(COREDIR)/bp_codepage.c

$(UNIT_BUILD_DIR)/test_codepage_asan: \
    $(UNIT_TEST_DIR)/test_codepage.c \
    $(COREDIR)/bp_codepage.c \
    $(COREDIR)/bp_sjis_table.h \
    | $(UNIT_BUILD_DIR)
	$(CC) $(UNIT_ASAN_FLAGS) $(DEBUG_LDFLAGS) -o $@ \
	    $(UNIT_TEST_DIR)/test_codepage.c \
	    $(COREDIR)/bp_codepage.c

# --- configured mini codepage profiles ---
$(UNIT_BUILD_DIR)/test_mini_cp437: $(UNIT_TEST_DIR)/test_mini_codepage.c \
    $(COREDIR)/bp_codepage.c $(COREDIR)/bp_codepage.h | $(UNIT_BUILD_DIR)
	$(CC) $(UNIT_CFLAGS) -DBP_MINI -DBP_CREATOR_OS=BP_OS_DOS -o $@ \
	    $(UNIT_TEST_DIR)/test_mini_codepage.c $(COREDIR)/bp_codepage.c

$(UNIT_BUILD_DIR)/test_mini_cp850: $(UNIT_TEST_DIR)/test_mini_codepage.c \
    $(COREDIR)/bp_codepage.c $(COREDIR)/bp_codepage.h | $(UNIT_BUILD_DIR)
	$(CC) $(UNIT_CFLAGS) -DBP_MINI -DBP_CREATOR_OS=BP_OS_OS2 -o $@ \
	    $(UNIT_TEST_DIR)/test_mini_codepage.c $(COREDIR)/bp_codepage.c

# --- test_archive_format ---
ARCHIVE_TEST_HEADERS = $(COREDIR)/archiver.h $(COREDIR)/bp_types.h \
    $(COREDIR)/bp_metadata.h $(COREDIR)/archive_name.h \
    $(COREDIR)/compress.h $(COREDIR)/boltlite.h $(COREDIR)/fluff.h \
    $(COREDIR)/bp_codepage.h $(COREDIR)/mini/archiver.h $(COREDIR)/mini/compress.h \
    $(BOLTLZDIR)/boltlz.h src/frontend/mini_shared/dos_time.h \
    src/frontend/mini_shared/mini_common.h src/frontend/atari/atari_path.h

$(UNIT_BUILD_DIR)/test_archive_format: \
    $(UNIT_TEST_DIR)/test_archive_format.c \
    $(ARCHIVE_TEST_HEADERS) \
    $(COREDIR)/archiver.c \
    $(COREDIR)/compress.c \
    $(COREDIR)/boltlite.c \
    $(COREDIR)/fluff.c \
    $(COREDIR)/bp_codepage.c \
    $(COREDIR)/mini/archiver.c $(COREDIR)/mini/compress.c $(COREDIR)/mini/boltlz_decomp.c \
    src/frontend/mini_shared/mini_common.c \
    $(BOLTLZDIR)/boltlz.c \
    | $(UNIT_BUILD_DIR)
	$(CC) $(UNIT_CFLAGS) -DBP_MINI -O2 $(FLUFF_NATIVE_FLAGS) -o $@ \
	    $(UNIT_TEST_DIR)/test_archive_format.c \
	    $(COREDIR)/archiver.c $(COREDIR)/compress.c $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c $(COREDIR)/bp_codepage.c $(BOLTLZDIR)/boltlz.c \
	    $(COREDIR)/mini/archiver.c $(COREDIR)/mini/compress.c $(COREDIR)/mini/boltlz_decomp.c \
	    src/frontend/mini_shared/mini_common.c

$(UNIT_BUILD_DIR)/test_archive_format_asan: \
    $(UNIT_TEST_DIR)/test_archive_format.c \
    $(ARCHIVE_TEST_HEADERS) \
    $(COREDIR)/archiver.c \
    $(COREDIR)/compress.c \
    $(COREDIR)/boltlite.c \
    $(COREDIR)/fluff.c \
    $(COREDIR)/bp_codepage.c \
    $(COREDIR)/mini/archiver.c $(COREDIR)/mini/compress.c $(COREDIR)/mini/boltlz_decomp.c \
    src/frontend/mini_shared/mini_common.c \
    $(BOLTLZDIR)/boltlz.c \
    | $(UNIT_BUILD_DIR)
	$(CC) $(UNIT_ASAN_FLAGS) -DBP_MINI $(DEBUG_LDFLAGS) -o $@ \
	    $(UNIT_TEST_DIR)/test_archive_format.c \
	    $(COREDIR)/archiver.c $(COREDIR)/compress.c $(COREDIR)/boltlite.c \
	    $(COREDIR)/fluff.c $(COREDIR)/bp_codepage.c $(BOLTLZDIR)/boltlz.c \
	    $(COREDIR)/mini/archiver.c $(COREDIR)/mini/compress.c $(COREDIR)/mini/boltlz_decomp.c \
	    src/frontend/mini_shared/mini_common.c

# --- mini_test_compress (full-core API on Linux; Open Watcom variant below) ---
$(UNIT_BUILD_DIR)/mini_test_compress: \
    $(UNIT_TEST_DIR)/mini_test_compress.c \
    $(COREDIR)/mini/compress.c \
    $(COREDIR)/mini/boltlz_decomp.c \
    $(COREDIR)/boltlite.c \
    $(BOLTLZDIR)/boltlz.c \
    | $(UNIT_BUILD_DIR)
	$(CC) $(UNIT_CFLAGS) -DBP_MINI -o $@ \
	    $(UNIT_TEST_DIR)/mini_test_compress.c \
	    $(COREDIR)/mini/compress.c $(COREDIR)/mini/boltlz_decomp.c \
	    $(COREDIR)/boltlite.c $(BOLTLZDIR)/boltlz.c

$(UNIT_BUILD_DIR)/mini_test_compress_asan: \
    $(UNIT_TEST_DIR)/mini_test_compress.c \
    $(COREDIR)/mini/compress.c \
    $(COREDIR)/mini/boltlz_decomp.c \
    $(COREDIR)/boltlite.c \
    $(BOLTLZDIR)/boltlz.c \
    | $(UNIT_BUILD_DIR)
	$(CC) $(UNIT_ASAN_FLAGS) $(DEBUG_LDFLAGS) -DBP_MINI -o $@ \
	    $(UNIT_TEST_DIR)/mini_test_compress.c \
	    $(COREDIR)/mini/compress.c $(COREDIR)/mini/boltlz_decomp.c \
	    $(COREDIR)/boltlite.c $(BOLTLZDIR)/boltlz.c

$(UNIT_BUILD_DIR):
	mkdir -p $@

# ============================================================================
# unit-test-dos16: compile mini compression and codepage tests with Open Watcom
# ============================================================================
#
# This confirms the mini compression layer and CP437/CP850 conversion fixtures
# compile and run inside DOSBox.
# Prerequisite: Open Watcom 2.0 at /opt/watcom and DOSBox on PATH.
#
# Build only (no run): make unit-build-dos16
# Build + run in DOSBox: make unit-test-dos16

UNIT_DOS16_BDIR = build/unit-test-dos16
UNIT_DOS16_EXE  = $(UNIT_DOS16_BDIR)/minitst.exe
UNIT_DOS16_CODEPAGE_EXE = $(UNIT_DOS16_BDIR)/minicp.exe
UNIT_DOS16_CP850_EXE = $(UNIT_DOS16_BDIR)/minicp85.exe
UNIT_DOS16_RESOURCE_EXE = $(UNIT_DOS16_BDIR)/minires.exe
UNIT_DOS16_ALLOC_EXE = $(UNIT_DOS16_BDIR)/miniallo.exe
UNIT_DOS16_ALLOC_CLI_EXE = $(UNIT_DOS16_BDIR)/miniacli.exe
UNIT_DOS16_ALLOC_DIR = $(UNIT_DOS16_BDIR)/alloc
UNIT_DOS16_ALLOC_FLAGS = -Dmalloc=bp_test_malloc -Dcalloc=bp_test_calloc \
	-Drealloc=bp_test_realloc -Dfree=bp_test_free

.PHONY: unit-build-dos16 unit-test-dos16

unit-build-dos16: dos16
	mkdir -p $(UNIT_DOS16_BDIR)
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) -DBP_ENABLE_ADLER32 \
	    -i=$(WATCOM_ROOT)/h -i=$(COREDIR) -i=$(UNIT_TEST_DIR) \
	    -fo=$(UNIT_DOS16_BDIR)/minitst.obj \
	    $(UNIT_TEST_DIR)/mini_test_compress.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) -DBP_ENABLE_ADLER32 \
	    -i=$(WATCOM_ROOT)/h -i=$(COREDIR) \
	    -fo=$(UNIT_DOS16_BDIR)/compress.obj \
	    $(COREDIR)/mini/compress.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) \
	    -i=$(WATCOM_ROOT)/h -i=$(COREDIR) \
	    -fo=$(UNIT_DOS16_BDIR)/boltlz_decomp.obj \
	    $(COREDIR)/mini/boltlz_decomp.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) \
	    -i=$(WATCOM_ROOT)/h -i=$(COREDIR) \
	    -fo=$(UNIT_DOS16_BDIR)/boltlite.obj \
	    $(COREDIR)/boltlite.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) \
	    -i=$(WATCOM_ROOT)/h -i=$(COREDIR) \
	    -fo=$(UNIT_DOS16_BDIR)/bp_codepage.obj \
	    $(COREDIR)/bp_codepage.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) \
	    -i=$(WATCOM_ROOT)/h -i=$(COREDIR) -i=$(UNIT_TEST_DIR) \
	    -fo=$(UNIT_DOS16_BDIR)/mini_codepage.obj \
	    $(UNIT_TEST_DIR)/test_mini_codepage.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) -DBP_MINI_CODEPAGE=BP_CP_850 \
	    -i=$(WATCOM_ROOT)/h -i=$(COREDIR) \
	    -fo=$(UNIT_DOS16_BDIR)/bp_codepage850.obj \
	    $(COREDIR)/bp_codepage.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) -DBP_MINI_CODEPAGE=BP_CP_850 \
	    -i=$(WATCOM_ROOT)/h -i=$(COREDIR) -i=$(UNIT_TEST_DIR) \
	    -fo=$(UNIT_DOS16_BDIR)/mini_codepage850.obj \
	    $(UNIT_TEST_DIR)/test_mini_codepage.c
	$(MINI_ENV) WATCOM=$(WATCOM_ROOT) $(WLINK16) @$(WATCOM_BIN)/wlink.lnk \
	    system dos \
	    file $(UNIT_DOS16_BDIR)/minitst.obj \
	    file $(UNIT_DOS16_BDIR)/compress.obj \
	    file $(UNIT_DOS16_BDIR)/boltlz_decomp.obj \
	    file $(UNIT_DOS16_BDIR)/boltlite.obj \
	    libpath $(WATCOM_ROOT)/lib286 \
	    libpath $(WATCOM_ROOT)/lib286/dos \
	    library clibc \
	    name $(UNIT_DOS16_EXE)
	$(MINI_ENV) WATCOM=$(WATCOM_ROOT) $(WLINK16) @$(WATCOM_BIN)/wlink.lnk \
	    system dos \
	    file $(UNIT_DOS16_BDIR)/mini_codepage.obj \
	    file $(UNIT_DOS16_BDIR)/bp_codepage.obj \
	    libpath $(WATCOM_ROOT)/lib286 \
	    libpath $(WATCOM_ROOT)/lib286/dos \
	    library clibc \
	    name $(UNIT_DOS16_CODEPAGE_EXE)
	$(MINI_ENV) WATCOM=$(WATCOM_ROOT) $(WLINK16) @$(WATCOM_BIN)/wlink.lnk \
	    system dos \
	    file $(UNIT_DOS16_BDIR)/mini_codepage850.obj \
	    file $(UNIT_DOS16_BDIR)/bp_codepage850.obj \
	    libpath $(WATCOM_ROOT)/lib286 \
	    libpath $(WATCOM_ROOT)/lib286/dos \
	    library clibc \
	    name $(UNIT_DOS16_CP850_EXE)
	$(MINI_ENV) $(WCC16) $(DOS16_FRONT) $(MINI_INCS) \
	    -fo=$(UNIT_DOS16_BDIR)/mini_resource16.obj \
	    test/unit/mini_test_resource16.c
	$(MINI_ENV) WATCOM=$(WATCOM_ROOT) $(WLINK16) @$(WATCOM_BIN)/wlink.lnk \
	    system dos option eliminate \
	    file $(DOS16_BDIR)/archiver.obj \
	    file $(DOS16_BDIR)/compress.obj \
	    file $(DOS16_BDIR)/boltlite.obj \
	    file $(DOS16_BDIR)/boltlz_decomp.obj \
	    file $(DOS16_BDIR)/mini_common.obj \
	    file $(DOS16_BDIR)/bp_codepage.obj \
	    file $(UNIT_DOS16_BDIR)/mini_resource16.obj \
	    name $(UNIT_DOS16_RESOURCE_EXE)
	mkdir -p $(UNIT_DOS16_ALLOC_DIR)
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) $(UNIT_DOS16_ALLOC_FLAGS) $(MINI_INCS) \
	    -fo=$(UNIT_DOS16_ALLOC_DIR)/archiver.obj $(MINI_COREDIR)/archiver.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) $(UNIT_DOS16_ALLOC_FLAGS) $(MINI_INCS) \
	    -fo=$(UNIT_DOS16_ALLOC_DIR)/compress.obj $(MINI_COREDIR)/compress.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) $(UNIT_DOS16_ALLOC_FLAGS) $(MINI_INCS) \
	    -fo=$(UNIT_DOS16_ALLOC_DIR)/boltlite.obj $(COREDIR)/boltlite.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) $(UNIT_DOS16_ALLOC_FLAGS) $(MINI_INCS) \
	    -fo=$(UNIT_DOS16_ALLOC_DIR)/boltlz_decomp.obj $(MINI_COREDIR)/boltlz_decomp.c
	$(MINI_ENV) $(WCC16) $(DOS16_FRONT) $(UNIT_DOS16_ALLOC_FLAGS) $(MINI_INCS) \
	    -fo=$(UNIT_DOS16_ALLOC_DIR)/mini_common.obj $(MINI_SHARED)/mini_common.c
	$(MINI_ENV) $(WCC16) $(DOS16_CORE) $(UNIT_DOS16_ALLOC_FLAGS) $(MINI_INCS) \
	    -fo=$(UNIT_DOS16_ALLOC_DIR)/bp_codepage.obj $(COREDIR)/bp_codepage.c
	$(MINI_ENV) $(WCC16) $(DOS16_FRONT) $(MINI_INCS) \
	    -fo=$(UNIT_DOS16_ALLOC_DIR)/mini_test_alloc_fail.obj \
	    test/unit/mini_test_alloc_fail.c
	$(MINI_ENV) $(WCC16) $(DOS16_FRONT) $(MINI_INCS) \
	    -fo=$(UNIT_DOS16_ALLOC_DIR)/mini_test_allocator.obj \
	    test/unit/mini_test_allocator.c
	$(MINI_ENV) WATCOM=$(WATCOM_ROOT) $(WLINK16) @$(WATCOM_BIN)/wlink.lnk \
	    system dos option eliminate \
	    file $(UNIT_DOS16_ALLOC_DIR)/archiver.obj \
	    file $(UNIT_DOS16_ALLOC_DIR)/compress.obj \
	    file $(UNIT_DOS16_ALLOC_DIR)/boltlite.obj \
	    file $(UNIT_DOS16_ALLOC_DIR)/boltlz_decomp.obj \
	    file $(UNIT_DOS16_ALLOC_DIR)/mini_common.obj \
	    file $(UNIT_DOS16_ALLOC_DIR)/bp_codepage.obj \
	    file $(UNIT_DOS16_ALLOC_DIR)/mini_test_alloc_fail.obj \
	    file $(UNIT_DOS16_ALLOC_DIR)/mini_test_allocator.obj \
	    name $(UNIT_DOS16_ALLOC_EXE)
	$(MINI_ENV) $(WCC16) $(DOS16_FRONT) $(UNIT_DOS16_ALLOC_FLAGS) \
	    -DBP_TEST_ALLOC_CLI $(MINI_INCS) \
	    -fo=$(UNIT_DOS16_ALLOC_DIR)/dos16_main.obj src/frontend/dos16/main.c
	$(MINI_ENV) WATCOM=$(WATCOM_ROOT) $(WLINK16) @$(WATCOM_BIN)/wlink.lnk \
	    system dos option eliminate \
	    file $(UNIT_DOS16_ALLOC_DIR)/archiver.obj \
	    file $(UNIT_DOS16_ALLOC_DIR)/compress.obj \
	    file $(UNIT_DOS16_ALLOC_DIR)/boltlite.obj \
	    file $(UNIT_DOS16_ALLOC_DIR)/boltlz_decomp.obj \
	    file $(UNIT_DOS16_ALLOC_DIR)/mini_common.obj \
	    file $(UNIT_DOS16_ALLOC_DIR)/bp_codepage.obj \
	    file $(UNIT_DOS16_ALLOC_DIR)/dos16_main.obj \
	    file $(UNIT_DOS16_ALLOC_DIR)/mini_test_allocator.obj \
	    name $(UNIT_DOS16_ALLOC_CLI_EXE)
	@echo "  OK    $(UNIT_DOS16_EXE)"
	@echo "  OK    $(UNIT_DOS16_CODEPAGE_EXE)"
	@echo "  OK    $(UNIT_DOS16_CP850_EXE)"
	@echo "  OK    $(UNIT_DOS16_RESOURCE_EXE)"
	@echo "  OK    $(UNIT_DOS16_ALLOC_EXE)"
	@echo "  OK    $(UNIT_DOS16_ALLOC_CLI_EXE)"

unit-test-dos16: unit-build-dos16
	mkdir -p $(UNIT_DOS16_BDIR)/ALLOC_DIR
	rm -f $(UNIT_DOS16_BDIR)/RESULT.TXT $(UNIT_DOS16_BDIR)/CODEPAGE.TXT \
	    $(UNIT_DOS16_BDIR)/CP850.TXT $(UNIT_DOS16_BDIR)/RESOURCE.TXT \
	    $(UNIT_DOS16_BDIR)/ALLOCFL.TXT $(UNIT_DOS16_BDIR)/ALLOC.DAT \
	    $(UNIT_DOS16_BDIR)/AF0.TXT $(UNIT_DOS16_BDIR)/AF1.TXT \
	    $(UNIT_DOS16_BDIR)/AF2.TXT $(UNIT_DOS16_BDIR)/AF0.BLT \
	    $(UNIT_DOS16_BDIR)/AF1.BLT $(UNIT_DOS16_BDIR)/AF2.BLT \
	    $(UNIT_DOS16_BDIR)/AF0.LOG $(UNIT_DOS16_BDIR)/AF1.LOG \
	    $(UNIT_DOS16_BDIR)/AF2.LOG \
	    $(UNIT_DOS16_BDIR)/BPK????.TMP $(UNIT_DOS16_BDIR)/BK????.BAK \
	    $(UNIT_DOS16_BDIR)/READ.BLT $(UNIT_DOS16_BDIR)/OUT.DAT \
	    $(UNIT_DOS16_BDIR)/LIMIT.BLT $(UNIT_DOS16_BDIR)/FAIL0.BLT \
	    $(UNIT_DOS16_BDIR)/FAIL1.BLT $(UNIT_DOS16_BDIR)/FAIL2.BLT \
	    $(UNIT_DOS16_BDIR)/FAIL3.BLT $(UNIT_DOS16_BDIR)/FAIL4.BLT \
	    $(UNIT_DOS16_BDIR)/FAIL5.BLT
	printf 'preserve original archive bytes\n' > $(UNIT_DOS16_BDIR)/AF0.BLT
	printf 'preserve original archive bytes\n' > $(UNIT_DOS16_BDIR)/AF1.BLT
	printf 'preserve original archive bytes\n' > $(UNIT_DOS16_BDIR)/AF2.BLT
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy dosbox \
	    -c "mount c $(abspath $(UNIT_DOS16_BDIR))" \
	    -c "c:" \
	    -c "minitst.exe > result.txt" \
	    -c "minicp.exe > codepage.txt" \
	    -c "minicp85.exe > cp850.txt" \
	    -c "minires.exe > resource.txt" \
	    -c "miniallo.exe > allocfl.log" \
	    -c "miniacli.exe --fail-path-buffer 0 c AF0.BLT ALLOC_DIR > AF0.LOG" \
	    -c "miniacli.exe --fail-path-buffer 1 c AF1.BLT ALLOC_DIR > AF1.LOG" \
	    -c "miniacli.exe --fail-path-buffer 2 c AF2.BLT ALLOC_DIR > AF2.LOG" \
	    -c "exit"
	@cat $(UNIT_DOS16_BDIR)/RESULT.TXT
	@grep -Eq '^Results: [0-9]+ passed, 0 failed' \
	    $(UNIT_DOS16_BDIR)/RESULT.TXT
	@grep -Fq 'mini codepage mapping and refusal checks passed' \
	    $(UNIT_DOS16_BDIR)/CODEPAGE.TXT
	@grep -Fq 'mini codepage mapping and refusal checks passed' \
	    $(UNIT_DOS16_BDIR)/CP850.TXT
	@cat $(UNIT_DOS16_BDIR)/RESOURCE.TXT
	@grep -Fq 'PASS: size_t=2 entry_size=54 max_entries=1213 table overflow refused' \
	    $(UNIT_DOS16_BDIR)/RESOURCE.TXT
	@grep -Fq 'PASS: projected_index_entries=128 projected_key_bytes=4096' \
	    $(UNIT_DOS16_BDIR)/RESOURCE.TXT
	@grep -Fq 'PASS: deferred_directory_queue_entries=256 overflow refused' \
	    $(UNIT_DOS16_BDIR)/RESOURCE.TXT
	@cat $(UNIT_DOS16_BDIR)/ALLOCFL.TXT
	@grep -Fq 'PASS: 26 writer/reader/extraction/index/queue/path-buffer/name allocation failures; temporary allocations released' \
	    $(UNIT_DOS16_BDIR)/ALLOCFL.TXT
	@grep -Fq 'result=1 error=-1 live=0' $(UNIT_DOS16_BDIR)/AF0.TXT
	@grep -Fq 'result=1 error=-1 live=0' $(UNIT_DOS16_BDIR)/AF1.TXT
	@grep -Fq 'result=1 error=-1 live=0' $(UNIT_DOS16_BDIR)/AF2.TXT
	@grep -Fxq 'preserve original archive bytes' $(UNIT_DOS16_BDIR)/AF0.BLT
	@grep -Fxq 'preserve original archive bytes' $(UNIT_DOS16_BDIR)/AF1.BLT
	@grep -Fxq 'preserve original archive bytes' $(UNIT_DOS16_BDIR)/AF2.BLT
	@test ! -e $(UNIT_DOS16_BDIR)/BPK0001.TMP

# Experimental BoltLZ: isolated builds, unchanged BoltLZ baseline.
BOLTLZ_CFLAGS ?= -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2
# Upstream LZ4 uses C99 extensions and C99 enables its forced-inline paths.
# Keep the same optimization flags; our own codec is still checked as C89.
BOLTLZ_LZ4_CFLAGS = $(filter-out -ansi -std=c89 -std=c90,$(BOLTLZ_CFLAGS)) -std=c99
BOLTLZ_FUZZ_CC ?= clang

.PHONY: boltlz-test boltlz-test-asan boltlz-bench boltlz-fuzz

$(BOLTLZ_BUILD_DIR):
	mkdir -p $@

$(BOLTLZ_BUILD_DIR)/.config: FORCE_CONFIG | $(BOLTLZ_BUILD_DIR)
	@printf '%s\n' 'CC=$(CC)' 'BOLTLZ_CFLAGS=$(BOLTLZ_CFLAGS)' \
	    'BOLTLZ_LZ4_CFLAGS=$(BOLTLZ_LZ4_CFLAGS)' \
	    | cmp -s - $@ || printf '%s\n' \
	    'CC=$(CC)' 'BOLTLZ_CFLAGS=$(BOLTLZ_CFLAGS)' \
	    'BOLTLZ_LZ4_CFLAGS=$(BOLTLZ_LZ4_CFLAGS)' > $@

$(BOLTLZ_BUILD_DIR)/%.o: src/core/%.c $(BOLTLZ_BUILD_DIR)/.config
	$(CC) $(BOLTLZ_CFLAGS) -Isrc/core -MMD -MP -c $< -o $@

$(BOLTLZ_BUILD_DIR)/boltlz.o: src/core/boltlz.c $(BOLTLZ_BUILD_DIR)/.config
	$(CC) $(BOLTLZ_CFLAGS) -Isrc/core -MMD -MP -c $< -o $@

$(BOLTLZ_BUILD_DIR)/bench_boltlz.o: test/bench_boltlz.c $(BOLTLZ_BUILD_DIR)/.config
	$(CC) $(BOLTLZ_CFLAGS) -Isrc/core -MMD -MP -c $< -o $@

$(BOLTLZ_BUILD_DIR)/test_boltlz.o: test/unit/test_boltlz.c $(BOLTLZ_BUILD_DIR)/.config
	$(CC) $(BOLTLZ_CFLAGS) -Isrc/core -MMD -MP -c $< -o $@

$(BOLTLZ_BUILD_DIR)/boltlz_reference.o: test/reference/boltlz_v2.c $(BOLTLZ_BUILD_DIR)/.config
	$(CC) $(BOLTLZ_CFLAGS) -Isrc/core -MMD -MP -c $< -o $@

$(BOLTLZ_BUILD_DIR)/boltlz_speed_reference.o: test/reference/boltlz_speed_base.c $(BOLTLZ_BUILD_DIR)/.config
	$(CC) $(BOLTLZ_CFLAGS) -Isrc/core -MMD -MP -c $< -o $@

$(BOLTLZ_BUILD_DIR)/boltlz_review_reference.o: test/reference/boltlz_review_base.c $(BOLTLZ_BUILD_DIR)/.config
	$(CC) $(BOLTLZ_CFLAGS) -Isrc/core -MMD -MP -c $< -o $@

$(BOLTLZ_BUILD_DIR)/boltlz_loop_reference.o: test/reference/boltlz_loop_base.c $(BOLTLZ_BUILD_DIR)/.config
	$(CC) $(BOLTLZ_CFLAGS) -Isrc/core -MMD -MP -c $< -o $@

$(BOLTLZ_BUILD_DIR)/lz4.o: test/reference/lz4/lz4.c $(BOLTLZ_BUILD_DIR)/.config
	$(CC) $(BOLTLZ_LZ4_CFLAGS) -MMD -MP -c $< -o $@

$(BOLTLZ_BUILD_DIR)/lz4hc.o: test/reference/lz4/lz4hc.c $(BOLTLZ_BUILD_DIR)/.config
	$(CC) $(BOLTLZ_LZ4_CFLAGS) -MMD -MP -c $< -o $@

$(BOLTLZ_BUILD_DIR)/bench_boltlz: $(BOLTLZ_BUILD_DIR)/bench_boltlz.o \
    $(BOLTLZ_BUILD_DIR)/boltlz.o \
    $(BOLTLZ_BUILD_DIR)/boltlz_reference.o $(BOLTLZ_BUILD_DIR)/lz4.o \
    $(BOLTLZ_BUILD_DIR)/lz4hc.o $(BOLTLZ_BUILD_DIR)/boltlz_speed_reference.o \
    $(BOLTLZ_BUILD_DIR)/boltlz_review_reference.o $(BOLTLZ_BUILD_DIR)/boltlz_loop_reference.o Makefile
	$(CC) $(BOLTLZ_CFLAGS) $(filter %.o,$^) -o $@
	cp $(BOLTLZ_BUILD_DIR)/.config $@.config

$(BOLTLZ_BUILD_DIR)/test_boltlz: $(BOLTLZ_BUILD_DIR)/test_boltlz.o \
    $(BOLTLZ_BUILD_DIR)/boltlz.o $(BOLTLZ_BUILD_DIR)/boltlz_loop_reference.o
	$(CC) $(BOLTLZ_CFLAGS) $^ -o $@

$(BOLTLZ_BUILD_DIR)/test_boltlz_asan: test/unit/test_boltlz.c \
    src/core/boltlz.c src/core/boltlz.h src/core/boltlz_config.h src/core/bp_types.h \
    test/reference/boltlz_scalar_decoder.h \
    test/reference/boltlz_loop_base.c test/reference/boltlz_loop_base.h \
    test/reference/boltlz_loop_base_config.h \
    test/unit/test_runner.h $(BOLTLZ_BUILD_DIR)/.config Makefile
	$(CC) $(BOLTLZ_CFLAGS) $(filter-out -O0 -O1 -O2 -O3,$(DEBUG_CFLAGS)) \
	    -Isrc/core test/unit/test_boltlz.c \
	    src/core/boltlz.c test/reference/boltlz_loop_base.c $(DEBUG_LDFLAGS) -o $@

$(BOLTLZ_BUILD_DIR)/fuzz_boltlz: test/fuzz_boltlz.c \
    src/core/boltlz.c src/core/boltlz.h src/core/boltlz_config.h src/core/bp_types.h \
    test/reference/boltlz_scalar_decoder.h \
    test/reference/boltlz_loop_base.c test/reference/boltlz_loop_base.h \
    test/reference/boltlz_loop_base_config.h \
    $(BOLTLZ_BUILD_DIR)/.config Makefile
	$(BOLTLZ_FUZZ_CC) $(BOLTLZ_CFLAGS) -Werror -g \
	    -Isrc/core -fsanitize=fuzzer,address,undefined \
	    test/fuzz_boltlz.c src/core/boltlz.c test/reference/boltlz_loop_base.c -o $@

boltlz-test: $(BOLTLZ_BUILD_DIR)/test_boltlz
	$<

boltlz-test-asan: $(BOLTLZ_BUILD_DIR)/test_boltlz_asan
	$<

boltlz-bench: $(BOLTLZ_BUILD_DIR)/bench_boltlz

boltlz-fuzz: $(BOLTLZ_BUILD_DIR)/fuzz_boltlz

-include $(wildcard $(BOLTLZ_BUILD_DIR)/*.d)
