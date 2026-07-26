#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtp.h"

static int callback_count;
static uint8_t callback_data[4096];
static size_t callback_size;
static uint16_t callback_sequences[8];
static uint32_t callback_timestamps[8];

static void on_packet(uint8_t* packet,
                      size_t bytes,
                      uint16_t sequence,
                      uint32_t timestamp,
                      void* user_data) {
  (void)user_data;
  if (callback_count < 8) {
    callback_sequences[callback_count] = sequence;
    callback_timestamps[callback_count] = timestamp;
  }
  callback_count++;
  callback_size = bytes < sizeof(callback_data) ? bytes : sizeof(callback_data);
  memcpy(callback_data, packet, callback_size);
}

static void reset_callback(void) {
  callback_count = 0;
  callback_size = 0;
  memset(callback_data, 0, sizeof(callback_data));
  memset(callback_sequences, 0, sizeof(callback_sequences));
  memset(callback_timestamps, 0, sizeof(callback_timestamps));
}

static size_t make_rtp(uint8_t* out,
                       uint16_t seq,
                       uint32_t timestamp,
                       int marker,
                       const uint8_t* payload,
                       size_t payload_size) {
  out[0] = 0x80;
  out[1] = (uint8_t)(96 | (marker ? 0x80 : 0));
  out[2] = (uint8_t)(seq >> 8);
  out[3] = (uint8_t)seq;
  out[4] = (uint8_t)(timestamp >> 24);
  out[5] = (uint8_t)(timestamp >> 16);
  out[6] = (uint8_t)(timestamp >> 8);
  out[7] = (uint8_t)timestamp;
  out[8] = 0;
  out[9] = 0;
  out[10] = 0;
  out[11] = 1;
  memcpy(out + 12, payload, payload_size);
  return 12 + payload_size;
}

static int require_int(int condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
  }
  return 0;
}

static int test_marker_flushes_complete_access_unit(void) {
  RtpDecoder decoder;
  uint8_t packet[128];
  rtp_decoder_init(&decoder, CODEC_H264, on_packet, NULL);
  reset_callback();

  const uint8_t sps[] = {0x67, 0x42};
  size_t packet_size = make_rtp(packet, 1, 1000, 0, sps, sizeof(sps));
  rtp_decoder_decode(&decoder, packet, packet_size);
  if (require_int(callback_count == 0, "H264 should not callback before marker")) return 1;

  const uint8_t pps[] = {0x68, 0xce};
  packet_size = make_rtp(packet, 2, 1000, 1, pps, sizeof(pps));
  rtp_decoder_decode(&decoder, packet, packet_size);

  const uint8_t expected[] = {
      0x00, 0x00, 0x00, 0x01, 0x67, 0x42,
      0x00, 0x00, 0x00, 0x01, 0x68, 0xce,
  };
  if (require_int(callback_count == 1, "H264 should callback once at marker")) return 1;
  if (require_int(callback_size == sizeof(expected), "H264 callback size should include both NALUs")) return 1;
  if (require_int(memcmp(callback_data, expected, sizeof(expected)) == 0,
                  "H264 callback should contain a complete Annex B access unit")) return 1;
  if (require_int(callback_sequences[0] == 2,
                  "H264 callback should expose the last contributing sequence")) return 1;
  if (require_int(callback_timestamps[0] == 1000,
                  "H264 callback should expose the frame RTP timestamp")) return 1;
  return 0;
}

static int test_timestamp_change_flushes_previous_access_unit(void) {
  RtpDecoder decoder;
  uint8_t packet[128];
  rtp_decoder_init(&decoder, CODEC_H264, on_packet, NULL);
  reset_callback();

  const uint8_t first[] = {0x67, 0x42};
  size_t packet_size = make_rtp(packet, 1, 1000, 0, first, sizeof(first));
  rtp_decoder_decode(&decoder, packet, packet_size);

  const uint8_t second[] = {0x68, 0xce};
  packet_size = make_rtp(packet, 2, 2000, 1, second, sizeof(second));
  rtp_decoder_decode(&decoder, packet, packet_size);

  if (require_int(callback_count == 2,
                  "H264 timestamp change should flush the previous access unit")) return 1;
  if (require_int(callback_sequences[0] == 1 && callback_timestamps[0] == 1000,
                  "Timestamp flush should retain previous frame metadata")) return 1;
  if (require_int(callback_sequences[1] == 2 && callback_timestamps[1] == 2000,
                  "Marker flush should expose current frame metadata")) return 1;
  return 0;
}

static int test_fu_a_gap_drops_corrupt_access_unit(void) {
  RtpDecoder decoder;
  uint8_t packet[128];
  rtp_decoder_init(&decoder, CODEC_H264, on_packet, NULL);
  reset_callback();

  const uint8_t fua_start[] = {0x7c, 0x85, 0xaa, 0xbb};
  size_t packet_size = make_rtp(packet, 1, 1000, 0, fua_start, sizeof(fua_start));
  rtp_decoder_decode(&decoder, packet, packet_size);

  const uint8_t fua_end[] = {0x7c, 0x45, 0xcc, 0xdd};
  packet_size = make_rtp(packet, 3, 1000, 1, fua_end, sizeof(fua_end));
  rtp_decoder_decode(&decoder, packet, packet_size);

  if (require_int(callback_count == 0,
                  "H264 FU-A sequence gap should drop the corrupt access unit")) return 1;
  return 0;
}

static int test_timestamp_change_drops_unfinished_fu_a(void) {
  RtpDecoder decoder;
  uint8_t packet[128];
  rtp_decoder_init(&decoder, CODEC_H264, on_packet, NULL);
  reset_callback();

  const uint8_t fua_start[] = {0x7c, 0x85, 0xaa, 0xbb};
  size_t packet_size = make_rtp(packet, 1, 1000, 0, fua_start, sizeof(fua_start));
  rtp_decoder_decode(&decoder, packet, packet_size);

  const uint8_t next_frame[] = {0x61, 0x11, 0x22};
  packet_size = make_rtp(packet, 2, 2000, 1, next_frame, sizeof(next_frame));
  rtp_decoder_decode(&decoder, packet, packet_size);

  const uint8_t expected[] = {
      0x00, 0x00, 0x00, 0x01, 0x61, 0x11, 0x22,
  };
  if (require_int(callback_count == 1,
                  "H264 timestamp change should drop unfinished FU-A and emit only the next frame")) return 1;
  if (require_int(callback_size == sizeof(expected),
                  "H264 callback should contain only the complete next frame")) return 1;
  if (require_int(memcmp(callback_data, expected, sizeof(expected)) == 0,
                  "H264 callback should not contain a partial FU-A")) return 1;
  if (require_int(callback_sequences[0] == 2 && callback_timestamps[0] == 2000,
                  "Complete next frame should expose its RTP metadata")) return 1;
  return 0;
}

static int test_generic_opus_preserves_rtp_metadata(void) {
  RtpDecoder decoder;
  uint8_t packet[128];
  rtp_decoder_init(&decoder, CODEC_OPUS, on_packet, NULL);
  reset_callback();

  const uint8_t opus[] = {0xf8, 0xff, 0xfe};
  size_t packet_size = make_rtp(packet, 0x1234, 0x01020304, 0,
                                opus, sizeof(opus));
  rtp_decoder_decode(&decoder, packet, packet_size);

  if (require_int(callback_count == 1, "Opus should callback once")) return 1;
  if (require_int(callback_size == sizeof(opus), "Opus callback size should match payload")) return 1;
  if (require_int(memcmp(callback_data, opus, sizeof(opus)) == 0,
                  "Opus callback should contain the RTP payload")) return 1;
  if (require_int(callback_sequences[0] == 0x1234,
                  "Opus callback should expose RTP sequence")) return 1;
  if (require_int(callback_timestamps[0] == 0x01020304,
                  "Opus callback should expose RTP timestamp")) return 1;
  return 0;
}

static int test_h264_passthrough_preserves_raw_rtp(void) {
  RtpDecoder decoder;
  uint8_t packet[128];
  rtp_decoder_init(&decoder, CODEC_H264, on_packet, NULL);
  rtp_decoder_set_passthrough(&decoder, 1);
  reset_callback();

  const uint8_t idr[] = {0x65, 0xaa, 0xbb};
  size_t packet_size = make_rtp(packet, 0x3456, 0x10203040, 1,
                                idr, sizeof(idr));
  rtp_decoder_decode(&decoder, packet, packet_size);

  if (require_int(callback_count == 1,
                  "H264 passthrough should callback once per raw RTP packet")) return 1;
  if (require_int(callback_size == packet_size,
                  "H264 passthrough should preserve the complete RTP packet")) return 1;
  if (require_int(memcmp(callback_data, packet, packet_size) == 0,
                  "H264 passthrough must not depacketize before the jitter buffer")) return 1;
  if (require_int(callback_sequences[0] == 0x3456 &&
                  callback_timestamps[0] == 0x10203040,
                  "H264 passthrough should preserve RTP metadata")) return 1;
  return 0;
}

int main(void) {
  if (test_marker_flushes_complete_access_unit()) return 1;
  if (test_timestamp_change_flushes_previous_access_unit()) return 1;
  if (test_fu_a_gap_drops_corrupt_access_unit()) return 1;
  if (test_timestamp_change_drops_unfinished_fu_a()) return 1;
  if (test_generic_opus_preserves_rtp_metadata()) return 1;
  if (test_h264_passthrough_preserves_raw_rtp()) return 1;
  printf("H264 RTP depacketizer tests passed\n");
  return 0;
}
