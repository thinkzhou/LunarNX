#include "../src/app/ice_candidate_processor.h"
#include "../src/app/stream_profile.h"
#include "../src/app/web_rtc_transport.h"
#include "../src/app/xbox_ice_preferences.h"
#include "../src/app/xbox_channel_manager.h"
#include "../src/app/xbox_session_client.h"
#include "../src/app/xbox_stream_session.h"

#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>

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

    auto preferred_profile = profile;
    preferred_profile.preferred_remote_ice_address = "203.0.113.50";
    preferred_profile.preferred_remote_ice_port = 3074;
    auto preferred = processor.parseRemotePayload(
        R"([{"candidate":"candidate:1 1 UDP 1 192.168.1.20 9002 typ host"},{"candidate":"candidate:2 1 UDP 1 203.0.113.50 3074 typ host"},{"candidate":"a=end-of-candidates"}])",
        preferred_profile);
    require(preferred[0].candidate.find("203.0.113.50 3074") !=
                std::string::npos,
            "a previously successful fresh Home endpoint should be tried first");
    require(preferred[0].candidate.find(" 2130706431 ") != std::string::npos,
            "the preferred fresh Home endpoint should receive top priority");

    preferred_profile.preferred_remote_ice_address = "198.51.100.99";
    preferred_profile.preferred_remote_ice_port = 3074;
    auto stale_preference = processor.parseRemotePayload(
        R"([{"candidate":"candidate:1 1 UDP 1 192.168.1.20 9002 typ host"},{"candidate":"candidate:2 1 UDP 1 203.0.113.50 3074 typ host"}])",
        preferred_profile);
    require(stale_preference[0].candidate.find("192.168.1.20 9002") !=
                std::string::npos,
            "a stale Home endpoint must fall back to the normal LAN-first order");

    preferred_profile.preferred_remote_ice_address = "2001:db8::1";
    preferred_profile.preferred_remote_ice_port = 9002;
    auto canonical_ipv6_preference = processor.parseRemotePayload(
        R"([{"candidate":"candidate:1 1 UDP 1 192.168.1.20 9002 typ host"},{"candidate":"candidate:2 1 UDP 1 2001:0db8:0:0:0:0:0:1 9002 typ host"}])",
        preferred_profile);
    require(canonical_ipv6_preference[0].candidate.find(
                "2001:0db8:0:0:0:0:0:1 9002") != std::string::npos,
            "equivalent IPv6 spellings should match a successful Home endpoint");

    auto cloud_profile = lunar::app::makeCloudStreamProfile("title-1", 1280, 720);
    cloud_profile.preferred_remote_ice_address = "192.168.1.20";
    cloud_profile.preferred_remote_ice_port = 9002;
    auto cloud_candidates = processor.parseRemotePayload(
        R"([{"candidate":"candidate:1 1 UDP 1 192.168.1.20 9002 typ host"},{"candidate":"candidate:2 1 UDP 1 203.0.113.50 3074 typ host"}])",
        cloud_profile);
    require(cloud_candidates[0].candidate.find("203.0.113.50 3074") !=
                std::string::npos,
            "Home endpoint reuse must not override Cloud public-first ordering");

    const std::string preference_path =
        "/tmp/lunarnx-xbox-ice-preferences-" +
        std::to_string(static_cast<long long>(getpid())) + ".json";
    std::remove(preference_path.c_str());
    lunar::app::XboxIcePreferenceStore preference_store(preference_path);
    auto empty_preference = preference_store.load("console-1");
    require(empty_preference.preferred_stun_url.empty() &&
                !empty_preference.hasHomeRoute(),
            "missing preference file should be an empty cache miss");

    lunar::app::XboxIcePreference saved_preference;
    saved_preference.preferred_stun_url =
        "stun:worldaz.relay.teams.microsoft.com:3478";
    saved_preference.remote_address = "203.0.113.50";
    saved_preference.remote_port = 3074;
    require(preference_store.save("console-1", saved_preference),
            "successful ICE preference should persist");
    auto loaded_preference = preference_store.load("console-1");
    require(loaded_preference.preferred_stun_url ==
                saved_preference.preferred_stun_url &&
                loaded_preference.remote_address == "203.0.113.50" &&
                loaded_preference.remote_port == 3074,
            "persisted STUN and Home route should round-trip");

    lunar::app::XboxIcePreference second_preference;
    second_preference.preferred_stun_url = "stun:stun.l.google.com:19302";
    second_preference.remote_address = "192.168.1.30";
    second_preference.remote_port = 9002;
    require(preference_store.save("console-2", second_preference),
            "a second console preference should persist");
    loaded_preference = preference_store.load("console-1");
    require(loaded_preference.preferred_stun_url ==
                second_preference.preferred_stun_url &&
                loaded_preference.remote_address == "203.0.113.50" &&
                loaded_preference.remote_port == 3074,
            "global STUN preference should update without losing another console route");
    std::remove(preference_path.c_str());

    auto lines = processor.toLibPeerLines(array);
    require(lines.size() == 1, "libpeer lines should skip end-of-candidates");
    require(lines[0].rfind("a=candidate:", 0) == 0,
            "libpeer line should include a= prefix");

    std::cout << "ice candidate processor tests passed\n";
    return 0;
}
