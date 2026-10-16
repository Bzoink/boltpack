#!/bin/bash

set -e

# BoltPack Release Helper
# Usage: ./scripts/release.sh create|upload|checksums|publish|status

PROJROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.."; pwd)"
cd "$PROJROOT"
VERSION=$(cat "$PROJROOT/VERSION" 2>/dev/null | tr -d '[:space:]')
[ -n "$VERSION" ] || { printf 'Error: VERSION file not found\n' >&2; exit 1; }
PROJECT_URL="https://api.github.com/repos/Bzoink/boltpack"

# Persists the numeric release ID between commands so draft releases can be
# found before a git tag exists (GitHub's /releases/tags/ endpoint returns 404
# for drafts that have no corresponding git tag yet).
RELEASE_ID_FILE="$PROJROOT/.release-id"

print_help() {
    cat << 'EOF'
BoltPack Release Manager

Usage:
  ./scripts/release.sh create      Create a draft release for the VERSION value
  ./scripts/release.sh upload      Generate checksums and upload packages from dist/
  ./scripts/release.sh checksums   Generate dist/SHA256SUMS.txt only
  ./scripts/release.sh publish     Publish the draft release
  ./scripts/release.sh status      Show release status and asset list

Notes:
  - Uses GITHUB_TOKEN or an authenticated GitHub CLI session
  - Draft releases are invisible to users until published
  - Assets can be added or replaced incrementally; run upload as many times as needed
  - Release ID is saved to .release-id so upload/publish work before a git tag exists

To get started:
  1. Authenticate with gh auth login, or set GITHUB_TOKEN for this shell
  2. Create draft:             ./scripts/release.sh create
  3. Build and package:        make && ./scripts/create-packages.sh
  4. Upload packages:          ./scripts/release.sh upload
  5. Repeat 3-4 for more platforms, then:
  6. Publish:                  ./scripts/release.sh publish
EOF
}

check_token() {
    if [[ -z "$GITHUB_TOKEN" ]] && command -v gh >/dev/null 2>&1; then
        if gh auth status -h github.com >/dev/null 2>&1; then
            GITHUB_TOKEN=$(gh auth token -h github.com 2>/dev/null || true)
            export GITHUB_TOKEN
        fi
    fi
    if [[ -z "$GITHUB_TOKEN" ]]; then
        echo "Error: no GitHub token found in GITHUB_TOKEN or gh authentication"
        echo "Log in with: gh auth login"
        echo ""
        echo "Create a token at: https://github.com/settings/tokens/new"
        echo "Needs scopes: repo (full control of private/public repos)"
        exit 1
    fi
}

# Return the numeric release ID for v${VERSION}.
# Reads .release-id first (works for drafts before a git tag exists),
# then falls back to a tag-based lookup (for already-published releases).
get_release_id() {
    if [[ -f "$RELEASE_ID_FILE" ]]; then
        cat "$RELEASE_ID_FILE"
        return
    fi
    curl -s \
        -H "Authorization: token $GITHUB_TOKEN" \
        "${PROJECT_URL}/releases/tags/v${VERSION}" \
        | grep -o '"id": [0-9]*' | head -1 | cut -d' ' -f2
}

create_release() {
    check_token

    if [[ -f "$RELEASE_ID_FILE" ]]; then
        echo "Draft already exists (ID: $(cat "$RELEASE_ID_FILE"))"
        echo "To start fresh, delete .release-id and delete the draft on GitHub."
        exit 1
    fi

    echo "Creating draft release v${VERSION}..."

    NOTES=$(cat RELEASE-NOTES.md 2>/dev/null || echo "BoltPack v${VERSION} Release")
    NOTES_JSON=$(printf '%s' "$NOTES" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')

    RELEASE_JSON=$(cat <<EOF
{
  "tag_name": "v${VERSION}",
  "name": "BoltPack v${VERSION}",
  "body": ${NOTES_JSON},
  "draft": true,
  "prerelease": false
}
EOF
)

    RESPONSE=$(curl -s -X POST \
        -H "Authorization: token $GITHUB_TOKEN" \
        -H "Content-Type: application/json" \
        -d "$RELEASE_JSON" \
        "${PROJECT_URL}/releases")

    RELEASE_ID=$(echo "$RESPONSE" | grep -o '"id": [0-9]*' | head -1 | cut -d' ' -f2)

    if [[ -z "$RELEASE_ID" ]]; then
        echo "Error creating release:"
        echo "$RESPONSE" | jq . 2>/dev/null || echo "$RESPONSE"
        exit 1
    fi

    echo "$RELEASE_ID" > "$RELEASE_ID_FILE"
    echo "✓ Draft release created (ID: $RELEASE_ID)"
    echo "  ID saved to .release-id"
    echo ""
    echo "Next steps:"
    echo "  make && ./scripts/create-packages.sh"
    echo "  ./scripts/release.sh upload"
}

generate_checksums() {
    local outfile="dist/SHA256SUMS.txt"
    local -a files=()

    for f in dist/boltpack-"$VERSION"-*.tar.gz dist/boltpack-"$VERSION"-*.zip dist/boltpack-"$VERSION"-*.exe dist/boltpack-"$VERSION"-*.lha \
              dist/boltpack-"$VERSION"-*.hqx dist/boltpack-"$VERSION"-*.img dist/boltpack-"$VERSION"-*.tar; do
        [[ -f "$f" ]] && files+=("$f")
    done

    if [[ ${#files[@]} -eq 0 ]]; then
        echo "No packages found in dist/ — skipping checksums"
        return
    fi

    echo "Generating checksums..."
    if command -v sha256sum > /dev/null 2>&1; then
        sha256sum "${files[@]}" > "$outfile"
    elif command -v shasum > /dev/null 2>&1; then
        shasum -a 256 "${files[@]}" > "$outfile"
    else
        echo "Warning: sha256sum/shasum not found — skipping checksums"
        return
    fi
    echo "  ✓ $outfile (${#files[@]} files)"
}

upload_packages() {
    check_token

    RELEASE_ID=$(get_release_id)
    if [[ -z "$RELEASE_ID" ]]; then
        echo "Error: No draft release found for v${VERSION}"
        echo "Run: ./scripts/release.sh create"
        exit 1
    fi

    generate_checksums

    echo ""
    echo "Uploading packages to release ${RELEASE_ID}..."
    echo ""

    UPLOAD_URL="https://uploads.github.com/repos/Bzoink/boltpack/releases/${RELEASE_ID}/assets"

    # Fetch existing assets once to avoid an O(n²) loop of API calls
    EXISTING_ASSETS=$(curl -s \
        -H "Authorization: token $GITHUB_TOKEN" \
        "${PROJECT_URL}/releases/${RELEASE_ID}/assets?per_page=100")

    for file in "$PROJROOT"/dist/boltpack-"$VERSION"-*.tar.gz "$PROJROOT"/dist/boltpack-"$VERSION"-*.zip "$PROJROOT"/dist/boltpack-"$VERSION"-*.exe "$PROJROOT"/dist/boltpack-"$VERSION"-*.lha \
                "$PROJROOT"/dist/boltpack-"$VERSION"-*.hqx "$PROJROOT"/dist/boltpack-"$VERSION"-*.img "$PROJROOT"/dist/boltpack-"$VERSION"-*.tar "$PROJROOT"/dist/SHA256SUMS.txt; do
        [[ -f "$file" ]] || continue

        FILENAME=$(basename "$file")
        FILESIZE=$(stat -c%s "$file" 2>/dev/null || stat -f%z "$file")
        FILESIZE_MB=$(awk "BEGIN {printf \"%.1f\", $FILESIZE/1048576}")

        echo "  Uploading: $FILENAME (${FILESIZE_MB} MB)..."

        # Replace existing asset if present
        ASSET_ID=$(echo "$EXISTING_ASSETS" \
            | grep -B4 "\"name\": \"$FILENAME\"" \
            | grep '"id"' | head -1 | awk -F': ' '{print $2}' | tr -d ',')

        if [[ -n "$ASSET_ID" ]]; then
            curl -s -X DELETE \
                -H "Authorization: token $GITHUB_TOKEN" \
                "${PROJECT_URL}/releases/assets/${ASSET_ID}" > /dev/null
            echo "    (replaced existing)"
        fi

        HTTP_STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST \
            -H "Authorization: token $GITHUB_TOKEN" \
            -H "Content-Type: application/octet-stream" \
            --data-binary "@$file" \
            "${UPLOAD_URL}?name=${FILENAME}")

        if [[ "$HTTP_STATUS" == "201" ]]; then
            echo "    ✓ done"
        else
            echo "    ✗ FAILED (HTTP $HTTP_STATUS)"
        fi
    done

    echo ""
    echo "✓ Upload complete"
    echo "  ./scripts/release.sh status    — check what's uploaded"
    echo "  ./scripts/release.sh publish   — make it public when ready"
}

publish_release() {
    check_token

    RELEASE_ID=$(get_release_id)
    if [[ -z "$RELEASE_ID" ]]; then
        echo "Error: No release found for v${VERSION}"
        echo "Run: ./scripts/release.sh create"
        exit 1
    fi

    echo "Publishing release v${VERSION} (ID: $RELEASE_ID)..."

    RESPONSE=$(curl -s -X PATCH \
        -H "Authorization: token $GITHUB_TOKEN" \
        -H "Content-Type: application/json" \
        -d '{"draft": false}' \
        "${PROJECT_URL}/releases/${RELEASE_ID}")

    if echo "$RESPONSE" | grep -q '"draft": false'; then
        rm -f "$RELEASE_ID_FILE"
        echo "✓ Release v${VERSION} is now public!"
        echo "  https://github.com/Bzoink/boltpack/releases/tag/v${VERSION}"
    else
        echo "Error publishing release:"
        echo "$RESPONSE" | jq . 2>/dev/null || echo "$RESPONSE"
        exit 1
    fi
}

status_release() {
    check_token

    RELEASE_ID=$(get_release_id)
    if [[ -z "$RELEASE_ID" ]]; then
        echo "✗ No release found for v${VERSION}"
        echo "  Run: ./scripts/release.sh create"
        exit 1
    fi

    RELEASE=$(curl -s \
        -H "Authorization: token $GITHUB_TOKEN" \
        "${PROJECT_URL}/releases/${RELEASE_ID}")

    if echo "$RELEASE" | grep -q '"message": "Not Found"'; then
        echo "✗ Release ID $RELEASE_ID not found on GitHub"
        echo "  Delete .release-id and run: ./scripts/release.sh create"
        exit 1
    fi

    IS_DRAFT=$(echo "$RELEASE" | grep '"draft"' | head -1 | grep -o 'true\|false')
    ASSET_COUNT=$(echo "$RELEASE" | grep -c '"name": "boltpack-' || true)

    echo "Release: v${VERSION} (ID: $RELEASE_ID)"
    echo "Status:  $([ "$IS_DRAFT" = "true" ] && echo 'DRAFT (not yet public)' || echo 'PUBLISHED')"
    echo "Assets:  $ASSET_COUNT packages"
    echo ""
    echo "Assets:"
    echo "$RELEASE" | grep '"name":' \
        | grep -v '"tag_name"\|"target_commitish"' \
        | sed 's/.*"name": "\(.*\)".*/  - \1/'
}

case "$1" in
    create)     create_release ;;
    upload)     upload_packages ;;
    checksums)  generate_checksums ;;
    publish)    publish_release ;;
    status)     status_release ;;
    help|"")    print_help ;;
    *)          echo "Unknown command: $1"; echo ""; print_help; exit 1 ;;
esac
