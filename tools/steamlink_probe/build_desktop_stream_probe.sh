#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${1:-"$root_dir/build/steamlink-desktop-stream-probe"}
mkdir -p "$(dirname -- "$out")"

ihs_src="$root_dir/vendor/ihslib/src"
ihs_inc="$root_dir/vendor/ihslib/include"
pb_src="$root_dir/vendor/protobuf-c/protobuf-c/protobuf-c.c"
pb_inc="$root_dir/vendor/protobuf-c"
mbedtls_source="$root_dir/lib/libpeer/third_party/mbedtls"
mbedtls_root="$mbedtls_source/build_host"

set -- \
  "$root_dir/tools/steamlink_probe/desktop_stream_probe.c" \
  "$root_dir/tools/steamlink_probe/posix_thread.c" \
  "$ihs_src/base.c" "$ihs_src/crc32.c" "$ihs_src/crc32c.c" \
  "$ihs_src/ihs_timer.c" "$ihs_src/ihs_ip.c" "$ihs_src/ihs_buffer.c" \
  "$ihs_src/ihs_queue.c" "$ihs_src/ihs_arraylist.c" "$ihs_src/ihs_enumeration.c" \
  "$ihs_src/ihs_enumeration_ll.c" "$ihs_src/ihs_enumeration_array.c" \
  "$ihs_src/client/client.c" "$ihs_src/client/discovery.c" \
  "$ihs_src/client/authorization.c" "$ihs_src/client/streaming.c" \
  "$ihs_src/session/session.c" "$ihs_src/session/packet.c" "$ihs_src/session/frame.c" \
  "$ihs_src/session/window.c" "$ihs_src/session/frame_crypto.c" \
  "$ihs_src/session/frame_stats.c" "$ihs_src/session/callbacks.c" \
  "$ihs_src/session/retransmission.c" "$ihs_src/session/channels/channel.c" \
  "$ihs_src/session/channels/ch_discovery.c" "$ihs_src/session/channels/ch_control.c" \
  "$ihs_src/session/channels/ch_control_authentication.c" \
  "$ihs_src/session/channels/ch_control_negotiation.c" \
  "$ihs_src/session/channels/ch_control_keepalive.c" \
  "$ihs_src/session/channels/ch_control_audio.c" \
  "$ihs_src/session/channels/ch_control_microphone.c" \
  "$ihs_src/session/channels/ch_control_video.c" \
  "$ihs_src/session/channels/ch_data.c" "$ihs_src/session/channels/ch_data_audio.c" \
  "$ihs_src/session/channels/ch_data_microphone.c" "$ihs_src/session/channels/ch_stats.c" \
  "$ihs_src/session/channels/video/ch_data_video.c" \
  "$ihs_src/session/channels/video/frame_h264.c" \
  "$ihs_src/session/channels/video/frame_hevc.c" \
  "$ihs_src/session/channels/video/partial_frames.c" \
  "$ihs_src/session/channels/control/control_cursor.c" \
  "$ihs_src/session/channels/control/control_hid.c" \
  "$ihs_src/session/channels/control/control_input_kbd.c" \
  "$ihs_src/session/channels/control/control_input_mouse.c" \
  "$ihs_src/session/channels/control/control_input_touch.c" \
  "$ihs_src/hid/device.c" "$ihs_src/hid/provider.c" "$ihs_src/hid/manager.c" \
  "$ihs_src/hid/report.c" "$ihs_src/protobuf/discovery.pb-c.c" \
  "$ihs_src/protobuf/hiddevices.pb-c.c" "$ihs_src/protobuf/pb_utils.c" \
  "$ihs_src/protobuf/remoteplay.pb-c.c" "$pb_src" \
  "$ihs_src/platforms/ihs_ip_posix.c" "$ihs_src/platforms/ihs_udp_posix.c" \
  "$ihs_src/crypto/impl_mbedtls.c"

clang -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
  -D_DARWIN_C_SOURCE \
  -include math.h \
  -I"$ihs_inc" -I"$ihs_src" -I"$pb_inc" -I"$mbedtls_source/include" \
  "$@" -o "$out" \
  -L"$mbedtls_root/library" -lmbedcrypto -lmbedtls -lmbedx509 \
  -lpthread -lm

echo "$out"
