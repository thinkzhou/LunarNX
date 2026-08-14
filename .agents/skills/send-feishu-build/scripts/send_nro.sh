#!/bin/zsh

set -eu
set +x

SCRIPT_DIR=${0:A:h}
PROJECT_DIR=${SCRIPT_DIR:h:h:h:h}
MESSAGE_TEMPLATE=${SCRIPT_DIR:h}/assets/message-template.txt
DRY_RUN=0
CHANGE_SUMMARY=''
NRO_INPUT=''
while (( $# )); do
    case "$1" in
    --dry-run)
        DRY_RUN=1
        ;;
    --change-summary)
        shift
        if (( $# == 0 )); then
            print -u2 -- '--change-summary requires text'
            exit 1
        fi
        CHANGE_SUMMARY=$1
        ;;
    --*)
        print -u2 -- "Unknown option: $1"
        exit 1
        ;;
    *)
        if [[ -n "$NRO_INPUT" ]]; then
            print -u2 -- "Usage: ${0:t} [--dry-run] [--change-summary text] [path/to/build.nro]"
            exit 1
        fi
        NRO_INPUT=$1
        ;;
    esac
    shift
done
NRO_INPUT=${NRO_INPUT:-build/switch/LunarNX.nro}

if [[ "$NRO_INPUT" = /* ]]; then
    NRO_PATH=$NRO_INPUT
else
    NRO_PATH="$PROJECT_DIR/$NRO_INPUT"
fi

if [[ ! -f "$NRO_PATH" ]]; then
    print -u2 -- "NRO not found: $NRO_PATH"
    exit 2
fi

if [[ "${NRO_PATH:e:l}" != "nro" ]]; then
    print -u2 -- "Refusing to send a non-NRO file: $NRO_PATH"
    exit 3
fi

FILE_SIZE=$(/usr/bin/stat -f '%z' "$NRO_PATH")
MAX_FILE_SIZE=30000000
if (( FILE_SIZE <= 0 || FILE_SIZE > MAX_FILE_SIZE )); then
    print -u2 -- "NRO size $FILE_SIZE is outside Feishu's 1-$MAX_FILE_SIZE byte limit"
    exit 4
fi

FILE_NAME=${NRO_PATH:t}
FILE_SHA256=$(/usr/bin/shasum -a 256 "$NRO_PATH" | /usr/bin/awk '{print $1}')
BUILD_TIME=$(/usr/bin/stat -f '%Sm' -t '%Y-%m-%d %H:%M:%S %z' "$NRO_PATH")
GIT_COMMIT=$(/usr/bin/git -C "$PROJECT_DIR" rev-parse --short HEAD 2>/dev/null || print unknown)
if [[ -n "$(/usr/bin/git -C "$PROJECT_DIR" status --short --untracked-files=no 2>/dev/null)" ]]; then
    GIT_COMMIT="${GIT_COMMIT}-dirty"
fi
if [[ -z "$CHANGE_SUMMARY" ]] && (( ! DRY_RUN )); then
    print -u2 -- 'A release-note summary is required: use --change-summary'
    exit 7
fi
if [[ -z "$CHANGE_SUMMARY" ]]; then
    COMMIT_SUBJECT=$(/usr/bin/git -C "$PROJECT_DIR" log -1 --pretty=format:%s 2>/dev/null || print 'Build update')
    CHANGE_SUMMARY="- $COMMIT_SUBJECT"
fi

if [[ ! -f "$MESSAGE_TEMPLATE" ]]; then
    print -u2 -- "Message template not found: $MESSAGE_TEMPLATE"
    exit 5
fi
SUMMARY=$(
    while IFS= read -r template_line || [[ -n "$template_line" ]]; do
        if [[ "$template_line" == '{{CHANGE_SUMMARY}}' ]]; then
            print -r -- "$CHANGE_SUMMARY"
        else
            print -r -- "${template_line//\{\{FILE_NAME\}\}/$FILE_NAME}"
        fi
    done < "$MESSAGE_TEMPLATE"
)
if [[ -z "$SUMMARY" ]]; then
    print -u2 -- "Message template rendered an empty message"
    exit 6
fi

if (( DRY_RUN )); then
    print -- "Dry run: $FILE_NAME is eligible for Feishu delivery"
    print -- "Size: $FILE_SIZE bytes"
    print -- "SHA-256: $FILE_SHA256"
    print -- "Git: $GIT_COMMIT"
    print -- "Built: $BUILD_TIME"
    print -- 'Message preview:'
    print -r -- "$SUMMARY"
    exit 0
fi

keychain_value() {
    local service=$1
    local value
    value=$(/usr/bin/security find-generic-password -s "$service" -w 2>/dev/null) || {
        print -u2 -- "Missing or unreadable macOS Keychain item: $service"
        return 1
    }
    [[ -n "$value" ]] || {
        print -u2 -- "Empty macOS Keychain item: $service"
        return 1
    }
    print -r -- "$value"
}

CHAT_ID=$(keychain_value feishu-chat-id)
if ! command -v lark-cli >/dev/null 2>&1; then
    print -u2 -- 'lark-cli is required; install and authenticate it first'
    exit 10
fi

NOTE_IDEMPOTENCY_KEY="lunarnx-${FILE_SHA256[1,32]}-note"
FILE_IDEMPOTENCY_KEY="lunarnx-${FILE_SHA256[1,32]}-file"

LARKSUITE_CLI_NO_UPDATE_NOTIFIER=1 \
LARKSUITE_CLI_NO_SKILLS_NOTIFIER=1 \
lark-cli im +messages-send \
    --as bot \
    --chat-id "$CHAT_ID" \
    --markdown "$SUMMARY" \
    --idempotency-key "$NOTE_IDEMPOTENCY_KEY" \
    --format json >/dev/null || {
        unset CHAT_ID
        print -u2 -- 'Feishu release-note message failed; the NRO was not sent'
        exit 11
    }

NRO_DIR=${NRO_PATH:h}
NRO_BASENAME=${NRO_PATH:t}
(
    cd "$NRO_DIR"
    LARKSUITE_CLI_NO_UPDATE_NOTIFIER=1 \
    LARKSUITE_CLI_NO_SKILLS_NOTIFIER=1 \
    lark-cli im +messages-send \
        --as bot \
        --chat-id "$CHAT_ID" \
        --file "./$NRO_BASENAME" \
        --idempotency-key "$FILE_IDEMPOTENCY_KEY" \
        --format json >/dev/null
) || {
    unset CHAT_ID
    print -u2 -- 'Feishu NRO message failed after the release note was sent'
    exit 12
}
unset CHAT_ID

print -- "Sent $FILE_NAME to Feishu"
print -- "Size: $FILE_SIZE bytes"
print -- "SHA-256: $FILE_SHA256"
print -- "Git: $GIT_COMMIT"
print -- "Built: $BUILD_TIME"
