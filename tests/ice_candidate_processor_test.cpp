#include "../src/app/ice_candidate_processor.h"
#include "../src/app/stream_profile.h"
#include "../src/app/web_rtc_transport.h"
#include "../src/app/xbox_channel_manager.h"
#include "../src/app/xbox_session_client.h"
#include "../src/app/xbox_stream_session.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

} // namespace

int main() {
    lunar::app::StreamProfile profile =
        lunar::app::makeHomeStreamProfile("console-1", 1280, 720);
    lunar::app::IceCandidateProcessor processor;

    auto candidates = processor.parseRemotePayload(
        R"({"iceCandidates":[{"candidate":"candidate:1 1 UDP 2130706431 10.0.0.2 9002 typ host","sdpMid":"0","sdpMLineIndex":0}]})",
        profile);

    require(candidates.size() == 2, "expected one parsed candidate and end marker");
    require(candidates[0].candidate ==
                "a=candidate:1 1 UDP 2130706431 10.0.0.2 9002 typ host",
            "candidate should be normalized with a= prefix");

    auto raw = processor.parseRemotePayload(
        "candidate:2 1 UDP 1 192.168.1.4 9002 typ host",
        profile);
    require(raw.size() == 2, "raw candidate should parse with end marker");
    require(raw[0].candidate.rfind("a=candidate:", 0) == 0,
            "raw candidate should gain a= prefix");
    require(raw[0].candidate.find(" 2130706431 ") != std::string::npos,
            "XStreaming should assign the first raw candidate the highest priority");

    auto array = processor.parseRemotePayload(
        R"([{"candidate":"a=candidate:3 1 UDP 1 2001:db8::1 9002 typ host","sdpMid":"0","sdpMLineIndex":0},{"candidate":"a=end-of-candidates","sdpMid":"0","sdpMLineIndex":0}])",
        profile);
    require(array.size() == 2, "array payload should parse candidates and end marker");

    auto teredo = processor.parseRemotePayload(
        R"([{"candidate":"a=candidate:1 1 UDP 100 192.168.1.10 9002 typ host ","messageType":"iceCandidate","sdpMLineIndex":0,"sdpMid":"0"},{"candidate":"a=candidate:2 1 UDP 1 2001:db8:1::11 9002 typ host ","messageType":"iceCandidate","sdpMLineIndex":0,"sdpMid":"0"},{"candidate":"a=candidate:3 1 UDP 1 2001:0000:4136:e378:8000:6657:34ff:8ee7 9002 typ host ","messageType":"iceCandidate","sdpMLineIndex":0,"sdpMid":"0"},{"candidate":"a=end-of-candidates","messageType":"iceCandidate","sdpMLineIndex":0,"sdpMid":"0"}])",
        profile);
    require(teredo.size() == 6,
            "XStreaming-compatible Teredo ICE should contain five candidates and the end marker");
    require(teredo[0].candidate ==
                "a=candidate:1 1 UDP 2130706431 192.168.1.10 9002 typ host",
            "LAN private IPv4 should be preferred for home streaming");
    require(teredo[1].candidate ==
                "a=candidate:2 1 UDP 1 203.0.113.24 9002 typ host",
            "public IPv4 derived from Teredo should follow LAN candidates");
    require(teredo[2].candidate ==
                "a=candidate:3 1 UDP 1 203.0.113.24 39336 typ host",
            "Teredo mapped-port public IPv4 should stay with other public IPv4 candidates");
    require(teredo[3].candidate ==
                "a=candidate:4 1 UDP 1 2001:db8:1::11 9002 typ host",
            "native IPv6 should follow IPv4 candidates");
    require(teredo[4].candidate ==
                "a=candidate:5 1 UDP 1 2001:0000:4136:e378:8000:6657:34ff:8ee7 9002 typ host",
            "original Teredo IPv6 should remain available");
    require(teredo[5].candidate == "a=end-of-candidates",
            "XStreaming should append the end-of-candidates marker");

    auto compressed_teredo = processor.parseRemotePayload(
        R"([{"candidate":"a=candidate:2 1 UDP 1 2001:0000:4136:e378:8000:f3fd:34ff:8ee7 9002 typ host","messageType":"iceCandidate","sdpMLineIndex":0,"sdpMid":"0"},{"candidate":"a=end-of-candidates","messageType":"iceCandidate","sdpMLineIndex":0,"sdpMid":"0"}])",
        profile);
    require(compressed_teredo.size() == 4,
            "compressed Xbox Teredo ICE should include two derived IPv4 candidates");
    require(compressed_teredo[0].candidate ==
                "a=candidate:1 1 UDP 2130706431 203.0.113.24 9002 typ host",
            "compressed Teredo should derive the Xbox default-port candidate");
    require(compressed_teredo[1].candidate ==
                "a=candidate:2 1 UDP 1 203.0.113.24 3074 typ host",
            "compressed Teredo should derive the mapped-port candidate");
    require(compressed_teredo[2].candidate.find(
                "2001:0000:4136:e378:8000:f3fd:34ff:8ee7") != std::string::npos,
            "compressed Teredo source candidate should remain available");

    auto invalid = processor.parseRemotePayload(
        R"({"candidates":[{"candidate":"a=candidate:4 1 UDP 1 10.0.0.3 9 typ host tcptype active","sdpMid":"0","sdpMLineIndex":0}]})",
        profile);
    require(invalid.size() == 1 &&
                invalid[0].candidate == "a=end-of-candidates",
            "invalid UDP candidate should be filtered before the end marker");

    auto xstreaming = processor.parseRemotePayload(
        R"({"messageType":"iceCandidate","candidate":[{"candidate":"candidate:6 1 UDP 1 10.0.0.6 9002 typ host","sdpMid":"0","sdpMLineIndex":0}]})",
        profile);
    require(xstreaming.size() == 2,
            "XStreaming candidate array should parse with end marker");
    require(xstreaming[0].candidate ==
                "a=candidate:1 1 UDP 2130706431 10.0.0.6 9002 typ host",
            "XStreaming candidate should be normalized and reprioritized");

    lunar::webrtc::IceCandidate local;
    local.sdp = "candidate:5 1 UDP 1 10.0.0.4 9002 typ host";
    require(lunar::app::IceCandidateProcessor::usernameFragmentFromSdp(
                "v=0\r\na=ice-ufrag:local-ufrag\r\na=ice-pwd:secret\r\n") ==
                "local-ufrag",
            "SDP ICE username fragment should be extracted without CRLF");
    auto api = processor.fromLocal({local}, "local-ufrag");
    require(api.size() == 2, "local candidate should include end marker");
    require(api[0].candidate == "candidate:5 1 UDP 1 10.0.0.4 9002 typ host",
            "API candidate should not include a= prefix");
    require(api[0].username_fragment == "local-ufrag",
            "local candidate should carry the SDP ICE username fragment");
    require(api[1].candidate == "a=end-of-candidates",
            "local ICE should explicitly terminate trickle candidates");
    require(api[1].username_fragment == "local-ufrag",
            "local end marker should carry the SDP ICE username fragment");

    std::string json = processor.toApiJson(api);
    require(json.find("\"messageType\":\"iceCandidate\"") != std::string::npos,
            "API JSON should include XStreaming messageType");
    require(json.find("\"candidate\"") != std::string::npos,
            "API JSON should use XStreaming candidate field");
    require(json.find("\"iceCandidates\"") == std::string::npos,
            "API JSON should not use legacy iceCandidates field");
    require(json.find("\\\"usernameFragment\\\":\\\"local-ufrag\\\"") !=
                std::string::npos,
            "API JSON should stringify candidates with usernameFragment");
    require(json.find("a=end-of-candidates") != std::string::npos,
            "API JSON should include the local end marker");

    auto round_trip = processor.parseRemotePayload(json, profile);
    require(round_trip.size() == 2,
            "stringified XStreaming candidate objects should parse");

    const std::string placeholder =
        R"([{"candidate":"candidate:1 1 UDP 100 13.104.10.20 9002 typ host"}])";
    const std::string real =
        R"([{"candidate":"candidate:2 1 UDP 2130706431 192.168.1.5 9002 typ host"},{"candidate":"a=end-of-candidates"}])";
    require(!processor.hasRealCandidate(placeholder),
            "low-priority placeholder ICE must not count as a real candidate");
    require(processor.hasRealCandidate(real),
            "high-priority trickled ICE should count as a real candidate");
    auto aggregated = processor.parseRemotePayloads({placeholder, real}, profile);
    require(aggregated.size() == 3,
            "remote ICE batches should be aggregated before rewriting");
    require(aggregated[0].candidate.find("192.168.1.5") != std::string::npos,
            "aggregated home ICE should prefer the later LAN candidate");
    require(processor.parseRemotePayloads({}, profile).empty(),
            "no remote ICE payload must remain a signaling failure");

    auto lines = processor.toLibPeerLines(array);
    require(lines.size() == 1, "libpeer lines should skip end-of-candidates");
    require(lines[0].rfind("a=candidate:", 0) == 0,
            "libpeer line should include a= prefix");

    std::cout << "ice candidate processor tests passed\n";
    return 0;
}
