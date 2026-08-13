#!/bin/sh
set -eu

output=${1:-/tmp/ps_media_replay.mp4}
duration=${PS_MEDIA_REPLAY_DURATION:-30}
codec=${PS_MEDIA_REPLAY_CODEC:-h264}

if [ "$codec" != "h264" ] && [ "$codec" != "hevc" ]; then
  echo "Unsupported PS media replay codec: $codec (use h264 or hevc)" >&2
  exit 1
fi

if [ -n "${PS_MEDIA_REPLAY_VIDEO_SOURCE:-}" ]; then
  if [ ! -f "$PS_MEDIA_REPLAY_VIDEO_SOURCE" ]; then
    echo "PS media replay source not found: $PS_MEDIA_REPLAY_VIDEO_SOURCE" >&2
    exit 1
  fi

  # Preserve the known-good H.264 bytes used by the Xbox/Rho player while
  # adapting only its audio track to Chiaki's Opus callback contract.
  ffmpeg -hide_banner -loglevel error -y \
    -stream_loop -1 -i "$PS_MEDIA_REPLAY_VIDEO_SOURCE" -t "$duration" \
    -map 0:v:0 -map 0:a:0 -c:v copy \
    -c:a libopus -application lowdelay -frame_duration 10 \
    -b:a 64k -ar 48000 -ac 2 \
    -movflags +faststart "$output"
else
  if [ "$codec" = "hevc" ]; then
    ffmpeg -hide_banner -loglevel error -y \
      -f lavfi -i "testsrc2=size=1280x720:rate=60:duration=${duration}" \
      -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=${duration}" \
      -c:v libx265 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
      -x265-params "keyint=60:min-keyint=60:scenecut=0:repeat-headers=1" \
      -tag:v hvc1 -b:v 1600k -maxrate 2200k -bufsize 3200k \
      -c:a libopus -application lowdelay -frame_duration 10 -b:a 64k -ar 48000 -ac 2 \
      -shortest -movflags +faststart "$output"
  else
    ffmpeg -hide_banner -loglevel error -y \
      -f lavfi -i "testsrc2=size=1280x720:rate=60:duration=${duration}" \
      -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=${duration}" \
      -c:v libx264 -preset ultrafast -tune zerolatency -profile:v baseline \
      -level:v 3.2 -pix_fmt yuv420p -g 60 -keyint_min 60 -sc_threshold 0 \
      -b:v 2200k -maxrate 3000k -bufsize 4400k \
      -c:a libopus -application lowdelay -frame_duration 10 -b:a 64k -ar 48000 -ac 2 \
      -shortest -movflags +faststart "$output"
  fi
fi

ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,profile,width,height,r_frame_rate \
  -of default=noprint_wrappers=1 "$output"
ffprobe -v error -select_streams a:0 \
  -show_entries stream=codec_name,sample_rate,channels \
  -of default=noprint_wrappers=1 "$output"
echo "PS media replay fixture: $output"
