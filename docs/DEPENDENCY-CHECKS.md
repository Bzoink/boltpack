# BoltPack Dependency Checks

All primary build and packaging scripts have been enhanced with automatic dependency checking at startup.

## Scripts with Dependency Checks

### 1. `build.sh` — Build Orchestrator

**Required Commands:**

- `make` — GNU Make (used to compile binaries for all platforms)

**Optional Commands:**

- `python3` — Python 3.x (needed for `./build.sh download-page` to generate the HTML download index)

**Behavior:**

- On success: Prints `All necessary commands are installed` and continues
- On missing required commands: Exits with code 1 and shows error list
- Checks run before any compilation

### 2. `scripts/create-packages.sh` — Package Creator

**Required Commands:**

- `tar` — GNU tar (creates `.tar` and `.tar.gz` packages for Linux, BSD, Solaris, etc.)
- `zip` — Info-ZIP (creates `.zip` packages for Windows, DOS, WASM, etc.)

**Optional Commands:**

- `iconv` — GNU libiconv (converts README to target platform encoding; falls back to UTF-8 if missing)
- `lha` — LHA archiver (creates `.lha` Amiga packages; falls back to `.zip` if missing)

**Behavior:**

- On success: Prints `All necessary commands are installed` and continues
- On missing required commands: Exits with code 1 and shows error list
- On missing optional commands: Continues with warning message (reduced functionality)
- Checks run before any packaging

### 3. `scripts/generate-download-page.py` — Download Page Generator

**Required:**

- Python 3.6+ (uses only standard library: `os`, `sys`, `argparse`, `datetime`, `html`)

**Behavior:**

- On success: Prints `All necessary commands are installed` and continues
- On Python < 3.6: Exits with error and version details
- Check runs before any page generation

## Example Outputs

### All Dependencies Present

```bash
$ ./build.sh all
All necessary commands are installed
Building all targets...
  BUILD linux (cc)
  OK    build/linux/boltpack
  ...
```

### Missing Optional Dependencies (Warning)

```bash
$ bash scripts/create-packages.sh
[WARN] Optional commands not found:
  • iconv: GNU libiconv - used for character encoding conversion
  • lha: LHA archiver - used for Amiga packages

All necessary commands are installed
==========================================
BoltPack Distribution Package Creator
...
```

### Missing Required Dependencies (Error)

```bash
$ bash create-packages.sh
[ERROR] Missing required commands:
  • tar: GNU tar - used to create .tar and .tar.gz packages
  • zip: Info-ZIP - used to create .zip packages

(script exits with code 1)
```

## Installation Guide

### Linux (Ubuntu/Debian)

```bash
# Install required commands
sudo apt-get install build-essential tar zip python3

# Install optional packages (recommended)
sudo apt-get install libc-bin lha
```

### Linux (Fedora/RHEL)

```bash
# Install required commands
sudo dnf groupinstall "Development Tools"
sudo dnf install tar zip python3

# Install optional packages
sudo dnf install glibc-common lha
```

### macOS

```bash
# Install Xcode Command Line Tools (includes make, tar, zip)
xcode-select --install

# Install optional packages via Homebrew
brew install lha
```

### Windows (WSL/Cygwin)

```bash
# Linux commands above apply within WSL
# For Cygwin, install: make, tar, zip, python3, iconv, lha
```

## Design Rationale

- **Early Detection:** Checks run at script startup, before any time-consuming operations
- **Clear Feedback:** Each missing command includes a description of its purpose
- **Graceful Degradation:** Optional commands trigger warnings but don't halt execution
- **Uniform Pattern:** All three scripts use consistent formatting and exit behavior
- **No External Dependencies:** Checks use only bash built-ins and `command -v`

## Testing the Checks

To verify dependency checks are working, run scripts with invalid arguments:

```bash
# These will print dependency check results before error messages
bash build.sh invalid-target
bash scripts/create-packages.sh invalid-platform
python3 scripts/generate-download-page.py --invalid-option
```

All three should print dependency confirmation at the beginning of their output.
