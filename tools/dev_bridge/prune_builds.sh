#!/bin/zsh

set -eu
set +x

SCRIPT_DIR=${0:A:h}
CLOUDFLARE_DIR="$SCRIPT_DIR/cloudflare"
BASE_URL=${LUNARNX_DEV_BRIDGE_URL:-https://lunarnx.tooyang.qzz.io}
KEEP=10
APPLY=0

usage() {
    print -- "Usage: $0 [--keep N] [--apply]"
    print -- "Defaults to a dry run that keeps the newest 10 versions."
}

while (( $# > 0 )); do
    case "$1" in
        --keep)
            (( $# >= 2 )) || { usage >&2; exit 2; }
            KEEP=$2
            shift 2
            ;;
        --apply)
            APPLY=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            print -u2 -- "Unknown argument: $1"
            usage >&2
            exit 2
            ;;
    esac
done

if [[ ! "$KEEP" =~ '^[0-9]+$' ]] || (( KEEP < 1 )); then
    print -u2 -- "--keep must be a positive integer"
    exit 2
fi
if [[ ! -d "$CLOUDFLARE_DIR/node_modules/wrangler" ]]; then
    print -u2 -- "Run npm install in $CLOUDFLARE_DIR first"
    exit 3
fi

TMP_DIR=$(mktemp -d /tmp/lunarnx-dev-prune-XXXXXX)
trap '/bin/rm -rf "$TMP_DIR"' EXIT

curl -fsS "${BASE_URL%/}/dev/versions.json" -o "$TMP_DIR/index.json"
/usr/bin/jq -e '.schema == 1 and (.versions | type == "array") and (.versions | length) > 0' \
    "$TMP_DIR/index.json" >/dev/null

/usr/bin/jq --argjson keep "$KEEP" \
    '{schema:1,versions:.versions[:$keep]}' \
    "$TMP_DIR/index.json" > "$TMP_DIR/index-next.json"
/usr/bin/jq '.versions[0]' "$TMP_DIR/index-next.json" > "$TMP_DIR/latest-next.json"
/usr/bin/jq -r --argjson keep "$KEEP" '.versions[$keep:][]?.version' \
    "$TMP_DIR/index.json" > "$TMP_DIR/remove-versions.txt"

cd "$CLOUDFLARE_DIR"
npx wrangler kv key list --binding ARTIFACTS --remote --prefix builds/ \
    > "$TMP_DIR/keys.json"
/usr/bin/jq -e 'type == "array"' "$TMP_DIR/keys.json" >/dev/null
/usr/bin/jq -r '.[].name | select(test("^builds/[0-9a-f]{64}\\.nro$"))' \
    "$TMP_DIR/keys.json" > "$TMP_DIR/binary-keys.txt"

: > "$TMP_DIR/remove-binaries.txt"
while IFS= read -r key; do
    [[ -n "$key" ]] || continue
    sha=${key#builds/}
    sha=${sha%.nro}
    if ! /usr/bin/jq -e --arg sha "$sha" \
        '.versions[] | select(.sha256 == $sha)' "$TMP_DIR/index-next.json" >/dev/null; then
        print -r -- "$key" >> "$TMP_DIR/remove-binaries.txt"
    fi
done < "$TMP_DIR/binary-keys.txt"

TOTAL=$(/usr/bin/jq '.versions | length' "$TMP_DIR/index.json")
KEPT=$(/usr/bin/jq '.versions | length' "$TMP_DIR/index-next.json")
REMOVE_VERSIONS=$(wc -l < "$TMP_DIR/remove-versions.txt" | tr -d ' ')
REMOVE_BINARIES=$(wc -l < "$TMP_DIR/remove-binaries.txt" | tr -d ' ')

print -- "LunarNX build cleanup plan"
print -- "Versions: $TOTAL total, $KEPT kept, $REMOVE_VERSIONS removed"
print -- "Unreferenced NRO objects to remove: $REMOVE_BINARIES"
if (( REMOVE_VERSIONS > 0 )); then
    print -- "Removed versions:"
    sed 's/^/  - /' "$TMP_DIR/remove-versions.txt"
fi

if (( APPLY == 0 )); then
    print -- "Dry run only. Re-run with --apply to perform these changes."
    exit 0
fi

# Publish the smaller index first. If a later delete fails, the remaining
# objects are harmless orphans rather than broken visible versions.
npx wrangler kv key put "builds/index.json" \
    --remote --binding ARTIFACTS --path "$TMP_DIR/index-next.json"
npx wrangler kv key put "builds/latest.json" \
    --remote --binding ARTIFACTS --path "$TMP_DIR/latest-next.json"

while IFS= read -r version; do
    [[ -n "$version" ]] || continue
    npx wrangler kv key delete "builds/versions/$version.json" \
        --remote --binding ARTIFACTS
done < "$TMP_DIR/remove-versions.txt"

while IFS= read -r key; do
    [[ -n "$key" ]] || continue
    npx wrangler kv key delete "$key" --remote --binding ARTIFACTS
done < "$TMP_DIR/remove-binaries.txt"

print -- "Cleanup complete. Kept the newest $KEPT version(s)."
