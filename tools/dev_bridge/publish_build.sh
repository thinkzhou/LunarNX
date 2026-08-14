#!/bin/zsh

set -eu
set +x

SCRIPT_DIR=${0:A:h}
PROJECT_DIR=${SCRIPT_DIR:h:h}
CLOUDFLARE_DIR="$SCRIPT_DIR/cloudflare"
NRO_INPUT=${1:-build/switch/LunarNX.nro}
VERSION_INPUT=${2:-}
NOTES=${3:-Development build}
if [[ "$NRO_INPUT" = /* ]]; then
    NRO_PATH=$NRO_INPUT
else
    NRO_PATH="$PROJECT_DIR/$NRO_INPUT"
fi

if [[ ! -f "$NRO_PATH" || "${NRO_PATH:e:l}" != "nro" ]]; then
    print -u2 -- "Expected a regular .nro file: $NRO_PATH"
    exit 2
fi
if [[ -z "${LUNARNX_DEV_BRIDGE_URL:-}" ]]; then
    print -u2 -- 'Set LUNARNX_DEV_BRIDGE_URL to the deployed Worker URL'
    exit 3
fi
if [[ ! -d "$CLOUDFLARE_DIR/node_modules/wrangler" ]]; then
    print -u2 -- "Run npm install in $CLOUDFLARE_DIR first"
    exit 4
fi

SHA256=$(/usr/bin/shasum -a 256 "$NRO_PATH" | /usr/bin/awk '{print $1}')
SIZE=$(/usr/bin/stat -f '%z' "$NRO_PATH")
KV_MAX_VALUE_BYTES=$((25 * 1024 * 1024))
if (( SIZE > KV_MAX_VALUE_BYTES )); then
    print -u2 -- "NRO exceeds the Workers KV 25 MiB value limit: $SIZE bytes"
    exit 6
fi
COMMIT=$(/usr/bin/git -C "$PROJECT_DIR" rev-parse --short HEAD 2>/dev/null || print unknown)
if [[ -n "$(/usr/bin/git -C "$PROJECT_DIR" status --short --untracked-files=no 2>/dev/null)" ]]; then
    COMMIT="${COMMIT}-dirty"
fi
BUILD_ID="$(date -u +%Y%m%d-%H%M%S)-$COMMIT"
VERSION=${VERSION_INPUT:-$BUILD_ID}
if [[ ! "$VERSION" =~ '^[A-Za-z0-9._-]{1,64}$' ]]; then
    print -u2 -- "Version must contain only letters, digits, dots, underscores, or hyphens: $VERSION"
    exit 5
fi
BASE_URL=${LUNARNX_DEV_BRIDGE_URL%/}
TMP_DIR=$(mktemp -d /tmp/lunarnx-dev-publish-XXXXXX)
trap '/bin/rm -rf "$TMP_DIR"' EXIT

/usr/bin/jq -n \
    --arg build_id "$BUILD_ID" \
    --arg version "$VERSION" \
    --arg notes "$NOTES" \
    --arg git_commit "$COMMIT" \
    --arg sha256 "$SHA256" \
    --arg download_url "$BASE_URL/dev/builds/$SHA256.nro" \
    --arg published_at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    --argjson size "$SIZE" \
    '{schema:2,version:$version,notes:$notes,build_id:$build_id,git_commit:$git_commit,published_at:$published_at,size:$size,sha256:$sha256,download_url:$download_url}' \
    > "$TMP_DIR/latest.json"

INDEX_STATUS=$(curl -sS -o "$TMP_DIR/index.json" -w '%{http_code}' "$BASE_URL/dev/versions.json") || exit 7
case "$INDEX_STATUS" in
    200) ;;
    404) print '{"schema":1,"versions":[]}' > "$TMP_DIR/index.json" ;;
    *)
        print -u2 -- "Could not read version index: HTTP $INDEX_STATUS"
        exit 7
        ;;
esac
/usr/bin/jq --slurpfile release "$TMP_DIR/latest.json" \
    --arg version "$VERSION" \
    '{schema:1,versions:([$release[0]] + [.versions[] | select(.version != $version)])}' \
    "$TMP_DIR/index.json" > "$TMP_DIR/index-next.json"

cd "$CLOUDFLARE_DIR"
npx wrangler kv key put "builds/$SHA256.nro" \
    --remote --binding ARTIFACTS --path "$NRO_PATH"
npx wrangler kv key put "builds/versions/$VERSION.json" \
    --remote --binding ARTIFACTS --path "$TMP_DIR/latest.json"
npx wrangler kv key put "builds/index.json" \
    --remote --binding ARTIFACTS --path "$TMP_DIR/index-next.json"
npx wrangler kv key put "builds/latest.json" \
    --remote --binding ARTIFACTS --path "$TMP_DIR/latest.json"

print -- "Published LunarNX development build"
print -- "Version: $VERSION"
print -- "Notes: $NOTES"
print -- "Build: $BUILD_ID"
print -- "Size: $SIZE bytes"
print -- "SHA-256: $SHA256"
print -- "Manifest: $BASE_URL/dev/latest.json"
print -- "Versions: $BASE_URL/dev/versions.json"
