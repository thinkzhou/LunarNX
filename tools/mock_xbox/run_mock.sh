#!/bin/bash
# Start the mock Xbox WebRTC streaming server.
# Generates a test video if it doesn't exist.

set -e
cd "$(dirname "$0")"

VIDEO_FILE="${VIDEO_FILE:-/tmp/test_stream.mp4}"
HTTP_PORT="${HTTP_PORT:-8080}"
CLOUD_TITLES="${CLOUD_TITLES:-80}"

# Generate test video if needed
if [ ! -f "$VIDEO_FILE" ]; then
    echo "Generating test video: $VIDEO_FILE"
    ffmpeg -f lavfi -i "testsrc=duration=120:size=1280x720:rate=30" \
        -f lavfi -i "sine=frequency=440:duration=120:sample_rate=48000" \
        -c:v libx264 -preset ultrafast -tune zerolatency -profile:v baseline \
        -pix_fmt yuv420p -g 30 \
        -c:a libopus -b:a 64k -ar 48000 -ac 2 \
        -shortest -f mp4 "$VIDEO_FILE" -y
    echo "Test video created."
fi

echo "Starting mock Xbox server on http://0.0.0.0:${HTTP_PORT}"
exec python3 mock_xbox_server.py --video "$VIDEO_FILE" --http-port "$HTTP_PORT" --cloud-titles "$CLOUD_TITLES"
