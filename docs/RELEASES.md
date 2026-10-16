# Release Workflow

BoltPack releases use the version in `VERSION` for the GitHub tag and asset
names. `RELEASE-NOTES.md` supplies the release description. The release helper
supports draft releases, incremental asset uploads, checksums, and publication.

## Authenticate

The helper accepts `GITHUB_TOKEN` or an authenticated GitHub CLI session. For a
local session, use:

```sh
gh auth login
```

Alternatively, set `GITHUB_TOKEN` for the current shell to a token with release
write access. Avoid saving tokens in shell startup files.

## Release steps

1. Update `VERSION` and prepare the matching section in `RELEASE-NOTES.md`.
2. Create a draft release:

   ```sh
   ./scripts/release.sh create
   ```

3. Build the desired Makefile targets, create packages, and generate the download
   page:

   ```sh
   make modern arm64 win32 win64
   bash scripts/create-packages.sh
   python3 scripts/generate-download-page.py
   ```

   Add or omit targets according to available toolchains. Native source kits are
   created separately with `bash scripts/create-source-kits.sh`.
4. Upload packages and review the draft:

   ```sh
   ./scripts/release.sh upload
   ./scripts/release.sh status
   ```

   Uploading replaces an asset with the same filename. Upload again after adding
   packages or correcting an asset.
5. Publish only after checking the draft and its assets:

   ```sh
   ./scripts/release.sh publish
   ```

`./scripts/build-and-release.sh <targets...>` combines building, packaging,
download-page generation, and upload when `GITHUB_TOKEN` is set. Without that
environment variable it still builds and packages, then skips upload; unlike
`release.sh`, the combined helper does not use an authenticated `gh` session.

## Other commands

```sh
./scripts/release.sh status      # Show draft or published release and assets
./scripts/release.sh checksums   # Write dist/SHA256SUMS.txt
./scripts/release.sh upload      # Refresh checksums and upload/replace assets
```

The helper stores a draft's numeric ID in `.release-id` so upload and publish
work before a git tag exists. It removes that file after publishing. The upload
step includes the generated checksum file and supported package formats; the
helper does not upload the generated HTML download page at `dist/index.html`.
