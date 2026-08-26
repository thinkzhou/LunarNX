#!/usr/bin/env python3
import re
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    sctp = Path("lib/libpeer/src/sctp.c").read_text()
    peer_connection = Path("lib/libpeer/src/peer_connection.c").read_text()
    libpeer_config = Path("lib/libpeer/src/config.h").read_text()
    tracked_patch = Path("tools/libpeer_legacy/legacy-libpeer-switch.patch").read_text()
    rtp = Path("lib/libpeer/src/rtp.c").read_text()
    socket = Path("lib/libpeer/src/socket.c").read_text()
    socket_header = Path("lib/libpeer/src/socket.h").read_text()
    dtls_srtp = Path("lib/libpeer/src/dtls_srtp.c").read_text()
    agent = Path("lib/libpeer/src/agent.c").read_text()
    agent_header = Path("lib/libpeer/src/agent.h").read_text()
    peer_manager = Path("src/webrtc/peer_manager.cpp").read_text()
    web_rtc_transport = Path("src/app/web_rtc_transport.cpp").read_text()

    require("chunk->sid = htons(sid);" in sctp,
            "SCTP DATA chunks must use the requested WebRTC stream id")
    require("chunk->sid = htons(0);" not in sctp,
            "SCTP DATA chunks must not hard-code SID 0")

    require("ntohs(init_ack->common.length)" in sctp,
            "SCTP INIT_ACK parameter parsing must convert chunk length from network order")
    require("i < init_ack->common.length - 20" not in sctp,
            "SCTP INIT_ACK parser must not compare against network-order raw length")
    require("sctp_param_padded_length" in sctp,
            "SCTP INIT_ACK parser should advance by padded parameter length")
    require("static uint16_t sqn" not in sctp,
            "SCTP stream sequence numbers must not be global across streams")
    require("sctp_current_stream_sequence" in sctp and
            "sctp_advance_stream_sequence" in sctp and
            "outbound_stream_sequence" in sctp,
            "SCTP ordered DATA chunks should advance per-stream sequence numbers only after sending")
    require("const int write_result = dtls_srtp_write" in sctp and
            "if (write_result >= 0)" in sctp and
            "return EAGAIN;" in sctp and
            "return EIO;" in sctp and
            "if (write_ret < 0)" in sctp and
            "sctp->tsn++;" in sctp,
            "SCTP output must normalize usrsctp success and propagate DTLS failures before committing custom TSN state")
    require("sctp_send_sack" in sctp,
            "Custom SCTP should acknowledge inbound DATA chunks with an explicit SACK helper")
    require("sack_chunk->a_rwnd = htonl(0x100000);" in sctp,
            "Custom SCTP SACK should advertise a normal receive window, not a near-zero one")
    require("DATA_CHANNEL_ACK" in sctp and "DCEP ACK" in sctp,
            "Custom SCTP should recognize inbound DCEP ACK control messages")
    require("sctp_handle_data_channel_control" in sctp,
            "Custom SCTP should handle DCEP control messages separately from app payload")
    require("sack_chunk->blocks" not in sctp,
            "Custom SCTP should not append outbound DCEP ACK DATA inside the SACK gap block area")
    require("sctp_add_stream_mapping(&pc->sctp, label, sid," in peer_connection,
            "Locally opened data channels must register SID-to-label mapping before inbound app messages arrive")
    require("if (sctp->stream_table[i].sid == sid)" in sctp and
            "return;" in sctp[sctp.index("static SctpStreamEntry* sctp_find_stream"):sctp.index("void sctp_parse_data_channel_open")],
            "SCTP stream mapping should ignore duplicate SID registrations")
    require("msg[1] = (char)channel_type" in peer_connection and
            "msg[1] = (char)channel_type" in tracked_patch,
            "active and reproducible libpeer must serialize the DCEP channel type")

    require("pc->agent.binding_request_time > 0" in peer_connection,
            "WebRTC keepalive timeout must not close a fresh connection with timestamp 0")
    require("pc->agent.binding_request_time = ports_get_epoch_time();" in peer_connection,
            "WebRTC should initialize the keepalive timestamp when ICE connects")
    keepalive_match = re.search(
        r"#define\s+CONFIG_KEEPALIVE_TIMEOUT\s+(\d+)", libpeer_config)
    require(keepalive_match is not None and int(keepalive_match.group(1)) >= 30000,
            "WebRTC liveness timeout should tolerate normal WAN ICE jitter")
    require("static void peer_connection_note_remote_activity" in peer_connection,
            "WebRTC should centralize authenticated remote activity tracking")
    rtp_decode = peer_connection[
        peer_connection.index("static void peer_connection_decode_rtp_packet"):
        peer_connection.index("static int peer_connection_drain_rtp_queue")
    ]
    require("peer_connection_note_remote_activity(pc);" in rtp_decode,
            "Successfully authenticated SRTP media should refresh WebRTC liveness")
    loop_source = peer_connection[peer_connection.index("int peer_connection_loop"):]
    completed_loop = loop_source[
        loop_source.index("case PEER_CONNECTION_COMPLETED"):
        loop_source.index("case PEER_CONNECTION_FAILED")
    ]
    pending_dtls = completed_loop[
        completed_loop.index("while (dtls_srtp_has_pending(&pc->dtls_srtp))"):
        completed_loop.index("} else if (rtp_packet_validate")
    ]
    require(completed_loop.count("peer_connection_note_remote_activity(pc);") >= 2 and
            "peer_connection_note_remote_activity(pc);" in pending_dtls,
            "Authenticated SRTCP and both immediate/pending DTLS traffic should refresh WebRTC liveness")
    require("CONFIG_KEEPALIVE_TIMEOUT 30000" in tracked_patch and
            "peer_connection_note_remote_activity" in tracked_patch,
            "Tracked legacy libpeer patch must reproduce the WebRTC liveness fix")
    require("peer_connection_set_media_enabled" in peer_connection,
            "PeerConnection should expose a startup gate for media decoding")
    require("#define PEER_CONNECTION_ENABLE_DEBUG_LOGS 0" in peer_connection,
            "Verbose libpeer packet tracing should be compiled off by default")
    require("#define SCTP_ENABLE_DEBUG_LOGS 0" in sctp,
            "Verbose custom SCTP packet tracing should be compiled off by default")
    require("drop RTP while media disabled" not in peer_connection and
            "!pc->media_enabled || budget <= 0" in peer_connection,
            "Completed-state loop should retain startup RTP until media decoding is enabled")
    require("PEER_CONNECTION_MAX_PACKETS_PER_LOOP" in peer_connection and
            "while (packets_processed < PEER_CONNECTION_MAX_PACKETS_PER_LOOP)" in peer_connection,
            "WebRTC completed-state loop should drain multiple UDP packets so RTP cannot starve DTLS/SCTP data")
    require("loop_work = packets_processed + rtp_decoded_packets" in peer_connection and
            "return loop_work;" in peer_connection,
            "WebRTC pump should report queued media work to the bounded outer drain")
    require("PEER_CONNECTION_MAX_RTP_DECODE_PER_LOOP" not in peer_connection and
            "rtp_decoded_this_loop" not in peer_connection and
            "drop RTP after media budget" not in peer_connection,
            "Completed-state loop must not drop RTP after media is enabled because H.264 FU-A frames cannot survive missing fragments")
    require("peer_connection_enqueue_rtp" in peer_connection and
            "peer_connection_drain_rtp_queue" in peer_connection and
            "rtp_queue_count" in peer_connection,
            "Completed-state loop should queue RTP while draining DTLS/SCTP control packets, then decode queued media without packet loss")
    require("if (!pc->media_enabled)" in peer_connection and
            "pc->rtp_queue_head = (pc->rtp_queue_head + 1) %" in peer_connection,
            "Disabled-media overflow must retain the newest contiguous RTP tail instead of freezing an old full queue")
    require(peer_connection.count("peer_connection_drain_rtp_queue(") >= 3,
            "Completed-state media handling must drain startup RTP before reading another socket burst")
    budget_match = re.search(r"#define\s+PEER_CONNECTION_RTP_DECODE_BUDGET\s+(\d+)", peer_connection)
    max_packets_match = re.search(r"#define\s+PEER_CONNECTION_MAX_PACKETS_PER_LOOP\s+(\d+)", peer_connection)
    media_pipeline = Path("src/stream/media_pipeline.cpp").read_text()
    require("enqueueVideoPacket" in media_pipeline and
            "video_worker_ = std::thread" in media_pipeline and
            "enqueueAudioPacket" in media_pipeline and
            "audio_worker_ = std::thread" in media_pipeline,
            "Media decode must stay off the WebRTC pump before libpeer uses a high RTP drain budget")
    require(budget_match is not None and max_packets_match is not None and
            16 <= int(budget_match.group(1)) <= 128 and
            int(budget_match.group(1)) <= int(max_packets_match.group(1)) <= 128,
            "WebRTC pump should use bounded per-loop RTP budgets so STUN/SCTP consent traffic stays responsive")
    require("kDataChannelSettle" in web_rtc_transport and
            "peer_->processEvents();" in web_rtc_transport,
            "WebRTC transport should briefly pump events after SCTP association so DCEP ACKs are handled before media init")

    require("CONFIG_MAX_NALU_SIZE (2 * 1024 * 1024)" in libpeer_config,
            "H.264 RTP reassembly must allow large Xbox IDR frames")
    require("SO_RCVBUF" in socket and "4 * 1024 * 1024" in socket,
            "Legacy libpeer should request a large UDP receive buffer for 1080p bursts")
    require("uint8_t h264_buf[CONFIG_MAX_NALU_SIZE]" in Path("lib/libpeer/src/rtp.h").read_text(),
            "H.264 RTP reassembly buffer should be per decoder, not static global state")
    require("uint8_t h264_frame_buf[CONFIG_MAX_NALU_SIZE]" in Path("lib/libpeer/src/rtp.h").read_text() and
            "h264_frame_timestamp" in Path("lib/libpeer/src/rtp.h").read_text(),
            "H.264 RTP decoder should accumulate complete access units per RTP timestamp before callback")
    require("static uint8_t nalu_buf" not in rtp and "static size_t offset" not in rtp,
            "H.264 RTP decoder must not use global static reassembly state")
    require("static int rtp_marker(" in rtp and "static uint32_t rtp_timestamp(" in rtp,
            "H.264 RTP decoder should use marker bit and timestamp to detect complete video frames")
    require("h264_flush_frame(rtp_decoder)" in rtp and
            "timestamp != rtp_decoder->h264_frame_timestamp" in rtp and
            "if (marker)" in rtp,
            "H.264 RTP decoder should flush complete frames on timestamp change or marker bit")
    require("static int rtp_payload(" in rtp and
            "packet[0] & 0x10" in rtp and
            "padding_size = packet[size - 1]" in rtp,
            "RTP decoder should account for CSRCs, header extensions, and padding before reading payload")
    require("sizeof(nalu_start_4bytecode) + payload_size > CONFIG_MAX_NALU_SIZE" in rtp,
            "H.264 single NALU packets must be bounds checked before copying")
    require("h264_frame_offset + payload_size > CONFIG_MAX_NALU_SIZE" in rtp or
            "h264_append_bytes" in rtp,
            "H.264 frame aggregation must be bounds checked before copying")
    require("nalu_type == 24" in rtp and "STAP-A" in rtp,
            "H.264 RTP decoder should handle STAP-A packets used by WebRTC packetization-mode=1")
    require("payload_size -= 2;" in rtp,
            "H.264 FU-A decoder should validate and strip FU-A headers before appending payload")
    require("int sequence_gap = rtp_note_sequence(rtp_decoder, seq)" in rtp and
            "sequence_gap && rtp_decoder->h264_frame_started" in rtp,
            "H.264 FU-A decoder should drop fragmented NALUs when a sequence gap is detected")

    require("sendOutboundCommand" in peer_manager and
            "peer_connection_datachannel_send_sid(" in peer_manager and
            "peer_connection_is_transient_send_error" in peer_manager and
            "completeOutboundCommand" in peer_manager,
            "PeerManager should retain reliable commands across transient DTLS failures")
    require("last_send_error" in socket_header and
            "udp_socket_is_temporary_send_error" in socket_header and
            "udp_socket->last_send_error = send_error" in socket,
            "UDP sends must preserve errno and classify temporary socket pressure")
    require("agent_last_send_error" in agent and
            "agent_last_send_error" in agent_header,
            "the selected ICE socket must expose its last send errno")
    require("MBEDTLS_ERR_SSL_WANT_WRITE" in dtls_srtp and
            "udp_socket_is_temporary_send_error" in dtls_srtp and
            "agent_last_send_error" in peer_connection and
            "udp_socket_is_temporary_send_error" in peer_connection,
            "temporary UDP send pressure must propagate through DTLS as WANT_WRITE")
    require("last_send_error" in tracked_patch and
            "udp_socket_is_temporary_send_error" in tracked_patch and
            "agent_last_send_error" in tracked_patch,
            "tracked legacy libpeer patch must preserve UDP send error classification")
    require("void PeerManager::setMediaEnabled" in peer_manager and
            "peer_connection_set_media_enabled(pc_" in peer_manager,
            "PeerManager should allow the app to defer RTP decode until control channels are ready")
    ice_state_callback = peer_manager[
        peer_manager.index("void PeerManager::onIceStateChange"):
        peer_manager.index("void PeerManager::onVideoTrack")
    ]
    require("PEER_CONNECTION_CLOSED" in ice_state_callback and
            "self->connected_ = false;" in ice_state_callback,
            "PeerManager must report a closed ICE transport as disconnected so reconnect can run")
    require("peer_connection_datachannel_send_sid(pc_,\n        reinterpret_cast<char*>(const_cast<uint8_t*>(data)),\n        len, 2) == 0" not in peer_manager,
            "PeerManager input send must not require a zero return from libpeer")

    print("libpeer SCTP config tests passed")


if __name__ == "__main__":
    main()
