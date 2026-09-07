// Capture the actual production negotiation protobuf at its transport boundary.
#include "session/channels/ch_control.h"
bool CaptureNegotiationSend(IHS_SessionChannel*, EStreamControlMessage,
                            const ProtobufCMessage*, int32_t);
#define IHS_SessionChannelControlSend CaptureNegotiationSend
#include "../src/steamlink/ihs_negotiation.c"
