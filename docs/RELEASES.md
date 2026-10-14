# GitHub Releases Setup

BoltPack uses GitHub Releases to distribute pre-built binaries incrementally. You can build packages for available platforms now, then add emulation-based builds later without notifying users until the release is complete.

## Quick Start

### 1. Generate GitHub Personal Access Token

1. Go to [GitHub Settings → Personal Access Tokens](https://github.com/settings/tokens/new)
2. Create **New token (classic)**
3. Select scope: `repo` (full control)
4. Copy the token (starts with `ghp_`)
5. Set in your shell:

   ```bash
   export GITHUB_TOKEN='ghp_xxxxxxxxxxxxxxxxxxxx'
   ```

**Optional:** Add to `~/.bashrc` or `~/.zshrc` to persist:

```bash
echo "export GITHUB_TOKEN='ghp_xxxxxxxxxxxxxxxxxxxx'" >> ~/.bashrc
source ~/.bashrc
```

### 2. Build & Release Workflow

#### Step 1: Create draft release

```bash
./scripts/release.sh create
```

- Creates `v1.0` as a **draft** (invisible to users)
- Saves the release ID to `.release-id` so subsequent commands work without a git tag
- Uses description from `RELEASE-NOTES.md` in the project root

#### Step 2: Build available packages

```bash
make modern arm64 musl riscv haiku wasm
./scripts/create-packages.sh
python3 scripts/generate-download-page.py
```

#### Step 3: Upload packages to draft

```bash
./scripts/release.sh upload
```

- Generates `dist/SHA256SUMS.txt` first, then uploads all packages
- Uploads `.tar.gz`, `.zip`, `.lha`, `.hqx` (Classic Mac BinHex), `.img` (macOS 9 HFS), and `.tar` (Plan 9)
- Can be run multiple times — existing assets are replaced

#### Step 4: Add more platforms later

```bash
make amiga atari dos win32 win64
./scripts/create-packages.sh
./scripts/release.sh upload  # add newly-built packages
```

#### Step 5: Publish when ready

```bash
./scripts/release.sh publish
```

- Makes the release public and notifies followers
- Deletes `.release-id` (no longer needed)
- Appears on the GitHub Releases page

### 3. Commands

```bash
# View current status
./scripts/release.sh status

# Create a draft (one-time, at release start)
./scripts/release.sh create

# Generate checksums only (without uploading)
./scripts/release.sh checksums

# Upload/update packages (can be run multiple times)
./scripts/release.sh upload

# Make public (final step)
./scripts/release.sh publish
```

## Workflow Example

### Week 1: Initial Release (Linux, ARM, WASM)

```bash
# Create draft
./scripts/release.sh create

# Build and package currently available platforms
make
./scripts/create-packages.sh
./scripts/release.sh upload

# Check status
./scripts/release.sh status
# Output:
# Release: v1.0
# Status:  DRAFT
# Assets:  6 packages
```

### Week 2: Add Vintage Systems (via Emulation)

```bash
# Set up emulation for Amiga, Atari, DOS
make amiga atari dos
./scripts/create-packages.sh

# Upload additional packages
./scripts/release.sh upload

./scripts/release.sh status
# Output:
# Release: v1.0
# Status:  DRAFT
# Assets:  9 packages
```

### Week 3: Final Release

```bash
# All platforms done - publish
./scripts/release.sh publish

# Release is now live at:
# https://github.com/Bzoink/boltpack/releases/tag/v1.0
```

## GitHub Release Page Features

When you publish, users see:

- **Release title:** "BoltPack v1.0"
- **Release notes:** From `RELEASE-NOTES.md`
- **Assets:** All `.tar.gz`, `.zip`, `.lha` packages with download buttons
- **Auto-generated:** Source code `.zip` and `.tar.gz`

**Release URL:** `https://github.com/Bzoink/boltpack/releases/tag/v1.0`

## Troubleshooting

### "Error: GITHUB_TOKEN not set"

```bash
export GITHUB_TOKEN='ghp_xxxxxxxxxxxxxxxxxxxx'
./scripts/release.sh status
```

### "Error: Release v1.0 not found"

Run: `./scripts/release.sh create`

### Token permissions error

Make sure your token has `repo` scope. Create a new one if needed.

### Upload fails

Check:

- Token is valid: `./scripts/release.sh status`
- Packages exist: `ls -la dist/`
- Network: `curl -H "Authorization: token $GITHUB_TOKEN" https://api.github.com/user`

## Notes

- **Draft releases** are invisible—users won't see them until published
- **Assets are immutable**—uploading again replaces the old one
- **No user notification** until you call `publish`
- **Release notes** are read from `RELEASE-NOTES.md` at creation time
- **`.release-id`** stores the draft's numeric ID locally; this file is gitignored and deleted on publish
- Can create multiple releases by changing `VERSION` in `scripts/release.sh`
