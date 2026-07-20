#include "../src/webrtc/xstreaming_data_channels.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

} // namespace

int main() {
    using lunar::webrtc::kXStreamingDataChannels;
    using lunar::webrtc::xstreamingDataChannelProtocol;

    require(std::string_view(kXStreamingDataChannels[0].label) == "input" &&
                kXStreamingDataChannels[0].sid == 0,
            "XStreaming opens input first on DTLS-client SID 0");
    require(std::string_view(kXStreamingDataChannels[1].label) == "chat" &&
                kXStreamingDataChannels[1].sid == 2,
            "XStreaming opens chat second on DTLS-client SID 2");
    require(std::string_view(kXStreamingDataChannels[2].label) == "control" &&
                kXStreamingDataChannels[2].sid == 4,
            "XStreaming opens control third on DTLS-client SID 4");
    require(std::string_view(kXStreamingDataChannels[3].label) == "message" &&
                kXStreamingDataChannels[3].sid == 6,
            "XStreaming opens message fourth on DTLS-client SID 6");
    for (const auto& channel : kXStreamingDataChannels) {
        require(channel.sid % 2 == 0,
                "DTLS clients must use even locally initiated DataChannel SIDs");
    }

    require(std::string_view(xstreamingDataChannelProtocol("input")) == "1.0",
            "input data channel protocol should be 1.0");
    require(std::string_view(xstreamingDataChannelProtocol("chat")) == "chatV1",
            "chat data channel protocol should be chatV1");
    require(std::string_view(xstreamingDataChannelProtocol("control")) == "controlV1",
            "control data channel protocol should be controlV1");
    require(std::string_view(xstreamingDataChannelProtocol("message")) == "messageV1",
            "message data channel protocol should be messageV1");
    require(std::string_view(xstreamingDataChannelProtocol("unknown")).empty(),
            "unknown data channel protocol should be empty");

    std::cout << "XStreaming data channel tests passed\n";
    return 0;
}
