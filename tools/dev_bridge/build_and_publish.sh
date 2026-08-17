#!/bin/zsh

set -eu
set +x

SCRIPT_DIR=${0:A:h}
SCRIPT_NAME=${0:t}
PROJECT_DIR=${SCRIPT_DIR:h:h}
BASE_URL=${LUNARNX_DEV_BRIDGE_URL:-https://lunarnx.tooyang.qzz.io}
BASE_VERSION=${LUNARNX_BASE_VERSION:-}
if [[ -z "$BASE_VERSION" ]]; then
    BASE_VERSION=$(/usr/bin/awk '
        $1 == "APP_VERSION" && $2 == "?=" { print $3; exit }
    ' "$PROJECT_DIR/Makefile.switch")
    if [[ -z "$BASE_VERSION" ]]; then
        print -u2 -- 'Could not read APP_VERSION from Makefile.switch'
        exit 3
    fi
fi
VERSION=''
NOTES='Development build'
NOTIFY_FEISHU=1
DROP_DIAG=${DROP_DIAG:-0}
APP_DIAG=${APP_DIAG:-0}
CURL_PROVIDER=${CURL_PROVIDER:-moonlight}
MANIFEST_PATH="$PROJECT_DIR/build/switch/build-manifest.json"

DEVICE_UPLOAD_TOKEN=${LUNARNX_DEV_BRIDGE_DEVICE_TOKEN:-}
if [[ -z "$DEVICE_UPLOAD_TOKEN" ]]; then
    DEVICE_UPLOAD_TOKEN=$(/usr/bin/security find-generic-password \
        -s lunarnx-dev-bridge-device-token -w 2>/dev/null) || {
        print -u2 -- 'Missing Keychain item: lunarnx-dev-bridge-device-token'
        exit 4
    }
fi

usage() {
    print -- "Usage: $SCRIPT_NAME [--version VERSION] [--notes TEXT] [--no-feishu]"
    print -- "                    [--drop-diag 0|1] [--app-diag 0|1]"
    print -- "Builds, verifies, records, and publishes one consistently-versioned NRO."
}

while (( $# > 0 )); do
    case "$1" in
        --version)
            (( $# >= 2 )) || { usage >&2; exit 2; }
            VERSION=$2
            shift 2
            ;;
        --notes)
            (( $# >= 2 )) || { usage >&2; exit 2; }
            NOTES=$2
            shift 2
            ;;
        --notify-feishu)
            NOTIFY_FEISHU=1
            shift
            ;;
        --no-feishu)
            NOTIFY_FEISHU=0
            shift
            ;;
        --drop-diag)
            (( $# >= 2 )) || { usage >&2; exit 2; }
            DROP_DIAG=$2
            shift 2
            ;;
        --app-diag)
            (( $# >= 2 )) || { usage >&2; exit 2; }
            APP_DIAG=$2
            shift 2
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

for toggle in "$DROP_DIAG" "$APP_DIAG"; do
    if [[ "$toggle" != 0 && "$toggle" != 1 ]]; then
        print -u2 -- 'Diagnostic switches must be 0 or 1'
        exit 2
    fi
done

if [[ -z "$VERSION" ]]; then
    VERSION_PREFIX="${BASE_VERSION}-d$(date -u +%y%m%d)."
    INDEX_JSON=$(curl --fail --silent --show-error --location \
        --max-time 15 "${BASE_URL%/}/dev/versions.json")
    NEXT_SEQUENCE=$(print -r -- "$INDEX_JSON" | /usr/bin/jq -r \
        --arg prefix "$VERSION_PREFIX" '
            [.versions[]?.version
             | select(startswith($prefix))
             | ltrimstr($prefix)
             | select(test("^[0-9]+$"))
             | tonumber]
            | (max // 0) + 1')
    VERSION="${VERSION_PREFIX}${NEXT_SEQUENCE}"
fi

if [[ ! "$VERSION" =~ '^[A-Za-z0-9._-]+$' ]] || (( ${#VERSION} > 15 )); then
    print -u2 -- "Version must use letters, digits, dots, underscores, or hyphens and fit NACP's 15-character display field: $VERSION"
    exit 3
fi

GIT_COMMIT=$(/usr/bin/git -C "$PROJECT_DIR" rev-parse --short HEAD 2>/dev/null || print unknown)
if [[ -n "$(/usr/bin/git -C "$PROJECT_DIR" status --short --untracked-files=no 2>/dev/null)" ]]; then
    GIT_COMMIT="${GIT_COMMIT}-dirty"
fi

print -- "Unified build version: $VERSION"
print -- "Git commit: $GIT_COMMIT"
print -- "DROP_DIAG: $DROP_DIAG"
print -- "APP_DIAG: $APP_DIAG"
print -- "CURL_PROVIDER: $CURL_PROVIDER"

cd "$PROJECT_DIR"
DROP_DIAG="$DROP_DIAG" \
APP_DIAG="$APP_DIAG" \
CURL_PROVIDER="$CURL_PROVIDER" \
APP_VERSION="$VERSION" \
GIT_COMMIT="$GIT_COMMIT" \
DEV_BRIDGE_UPLOAD_TOKEN="$DEVICE_UPLOAD_TOKEN" \
    "$PROJECT_DIR/scripts/docker_build_full.sh"

python3 tests/switch_nro_bss_test.py
python3 tests/chiaki_transport_diagnostics_test.py
python3 tests/drop_diagnostics_test.py
python3 tests/ps_release_logging_test.py
python3 tests/stream_input_cadence_test.py
git diff --check

NRO_PATH="$PROJECT_DIR/build/switch/LunarNX.nro"
SIZE=$(/usr/bin/stat -f '%z' "$NRO_PATH")
SHA256=$(/usr/bin/shasum -a 256 "$NRO_PATH" | /usr/bin/awk '{print $1}')
BUILT_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
/usr/bin/jq -n \
    --arg version "$VERSION" \
    --arg git_commit "$GIT_COMMIT" \
    --arg notes "$NOTES" \
    --arg built_at "$BUILT_AT" \
    --arg sha256 "$SHA256" \
    --arg curl_provider "$CURL_PROVIDER" \
    --argjson size "$SIZE" \
    --argjson drop_diag "$DROP_DIAG" \
    --argjson app_diag "$APP_DIAG" \
    '{schema:1,version:$version,git_commit:$git_commit,notes:$notes,
      built_at:$built_at,size:$size,sha256:$sha256,
      build:{drop_diag:($drop_diag == 1),app_diag:($app_diag == 1),
             curl_provider:$curl_provider},published:false}' \
    > "$MANIFEST_PATH"

PUBLISH_ARGS=("$NRO_PATH" "$VERSION" "$NOTES")
(( NOTIFY_FEISHU )) || PUBLISH_ARGS=(--no-feishu "${PUBLISH_ARGS[@]}")
LUNARNX_DEV_BRIDGE_URL="$BASE_URL" \
    "$SCRIPT_DIR/publish_build.sh" "${PUBLISH_ARGS[@]}"

PUBLISHED_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
/usr/bin/jq --arg published_at "$PUBLISHED_AT" \
    '.published = true | .published_at = $published_at' \
    "$MANIFEST_PATH" > "${MANIFEST_PATH}.tmp"
/bin/mv "${MANIFEST_PATH}.tmp" "$MANIFEST_PATH"

print -- "Local manifest: $MANIFEST_PATH"
