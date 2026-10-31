// Production IHS transport/window replays; no Steam host or Switch GPU involved.
extern "C" {
#include "ihslib.h"
#include "session/session_pri.h"
#include "session/channels/ch_control.h"
#include "steamlink/ihs_reliability.h"
#include "steamlink/ihs_streaming_support.h"
#include "ihs_buffer_ext.h"
}
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

extern "C" bool CaptureNegotiationSend(IHS_SessionChannel*, EStreamControlMessage,
                                       const ProtobufCMessage*, int32_t) { return false; }

static IHS_SessionPacket packet(uint16_t id, IHS_SessionPacketType type,
                                int16_t fragment = 0, size_t size = 1) {
    IHS_SessionPacket p{};
    p.header.type = type; p.header.channelId = IHS_SessionChannelIdControl;
    p.header.packetId = id; p.header.fragmentId = fragment;
    IHS_SessionPacketBodyInitialize(&p.body, false);
    std::vector<uint8_t> bytes(size, 0x33);
    IHS_BufferAppendMem(&p.body, bytes.data(), bytes.size());
    return p;
}

static bool countItem(IHS_QueueItem*, void* context) {
    ++*static_cast<size_t*>(context); return false;
}
static void unused(IHS_QueueItem*, void*) {}
static size_t pendingCount(IHS_Session* session) {
    LunarIHSTimerLock(session->timers);
    IHS_MutexLock(session->retransmission.lock);
    size_t result = 0;
    IHS_QueuePollEach(session->retransmission.queue, countItem, &result, unused, nullptr);
    IHS_MutexUnlock(session->retransmission.lock);
    LunarIHSTimerUnlock(session->timers);
    return result;
}
static void track(IHS_Session* session, uint16_t id,
                  IHS_SessionChannelId channel = IHS_SessionChannelIdControl) {
    auto p = packet(id, IHS_SessionPacketTypeReliable);
    p.header.channelId = channel;
    assert(IHS_RetransmissionQueue(&session->retransmission, &p));
    assert(p.body.data); // tracking owns a separate copy
    IHS_SessionPacketClear(&p, true);
}
static void ack(IHS_Session* session, uint16_t id) {
    auto p = packet(id, IHS_SessionPacketTypeACK, 0, 4);
    LunarIHSRetransmissionFeedback(&session->retransmission, &p);
    IHS_SessionPacketClear(&p, true);
}
static bool retryCount(IHS_QueueItem* item, void* context) {
    // The tracked packet is the first member of the pending queue entry.
    auto* p = reinterpret_cast<IHS_SessionPacket*>(item);
    auto* maximum = static_cast<unsigned*>(context);
    if (p->header.retransmitCount > *maximum) *maximum = p->header.retransmitCount;
    return false;
}
static void windowTests() {
    auto* w = IHS_SessionPacketsWindowCreateReliable(4, 0);
    auto p = packet(1, IHS_SessionPacketTypeReliable);
    assert(IHS_SessionPacketsWindowAdd(w, &p));
    assert(IHS_SessionPacketsWindowHasHole(w));
    assert(IHS_SessionPacketsWindowContiguousId(w) == 65535);
    IHS_SessionFrame frame{}; IHS_BufferInit(&frame.body, 8, 65536);
    assert(!IHS_SessionPacketsWindowPoll(w, &frame));
    p = packet(0, IHS_SessionPacketTypeReliable);
    assert(IHS_SessionPacketsWindowAdd(w, &p));
    assert(IHS_SessionPacketsWindowContiguousId(w) == 1);
    for (int id : {0, 1}) {
        assert(IHS_SessionPacketsWindowPoll(w, &frame));
        assert(frame.header.packetId == id);
        IHS_SessionPacketsWindowReleaseFrame(&frame);
    }
    // An already consumed duplicate at the exact tail must not refill a slot.
    p = packet(1, IHS_SessionPacketTypeReliable);
    assert(IHS_SessionPacketsWindowAdd(w, &p));
    IHS_SessionPacketClear(&p, true);
    assert(IHS_SessionPacketsWindowSize(w) == 0);
    // More than the original 128 control slots: preserve the hole, grow boundedly.
    p = packet(300, IHS_SessionPacketTypeReliable);
    assert(IHS_SessionPacketsWindowAdd(w, &p));
    assert(IHS_SessionPacketsWindowSize(w) == 299);
    uint8_t bitmap[40]{};
    IHS_SessionPacketsWindowHoleBitmap(w, 2, bitmap, 320);
    assert(bitmap[298 / 8] & (1u << (298 % 8)));
    IHS_SessionPacketsWindowDestroy(w);
    // Sequence rollover still waits for the missing head.
    w = IHS_SessionPacketsWindowCreateReliable(4, 65535);
    p = packet(0, IHS_SessionPacketTypeReliable);
    assert(IHS_SessionPacketsWindowAdd(w, &p));
    assert(!IHS_SessionPacketsWindowPoll(w, &frame));
    p = packet(65535, IHS_SessionPacketTypeReliable);
    assert(IHS_SessionPacketsWindowAdd(w, &p));
    assert(IHS_SessionPacketsWindowContiguousId(w) == 0);
    IHS_SessionPacketsWindowDestroy(w);
    // Media can join mid-frame; orphan fragments must not wedge the window.
    w = IHS_SessionPacketsWindowCreate(8);
    p = packet(5, IHS_SessionPacketTypeUnreliableFrag);
    assert(IHS_SessionPacketsWindowAdd(w, &p));
    p = packet(6, IHS_SessionPacketTypeUnreliable);
    assert(IHS_SessionPacketsWindowAdd(w, &p));
    assert(IHS_SessionPacketsWindowPoll(w, &frame));
    assert(frame.header.packetId == 6);
    IHS_SessionPacketsWindowReleaseFrame(&frame);
    IHS_SessionPacketsWindowDestroy(w);
    IHS_BufferClear(&frame.body, true);
}
static void reliabilityTests(IHS_Session* session) {
    track(session, 0); track(session, 1); track(session, 2);
    track(session, 0, IHS_SessionChannelIdDiscovery);
    ack(session, 1);
    assert(pendingCount(session) == 2); // cumulative ACK; discovery untouched
    auto nack = packet(2, IHS_SessionPacketTypeNACK, 0, 0);
    LunarIHSRetransmissionFeedback(&session->retransmission, &nack);
    assert(pendingCount(session) == 2); // missing packet is not acknowledged
    IHS_SessionPacketClear(&nack, true);
    ack(session, 2);
    assert(IHS_RetransmissionCancel(&session->retransmission, IHS_SessionChannelIdDiscovery, 0, 0));
    assert(pendingCount(session) == 0);
    // ACK can arrive before the send worker reports its original send.
    track(session, 3); ack(session, 3);
    IHS_SessionPacketHeader sent{}; sent.channelId = IHS_SessionChannelIdControl; sent.packetId = 3;
    LunarIHSRetransmissionInitialSent(&session->retransmission, &sent);
    assert(pendingCount(session) == 0);
    track(session, 65535); track(session, 0); track(session, 1);
    ack(session, 0); assert(pendingCount(session) == 1); ack(session, 1);
    track(session, 10); track(session, 11); track(session, 12);
    // Selective NACK confirms through 10 and presence of 12, while 11 stays missing.
    nack = packet(11, IHS_SessionPacketTypeNACK, 0, 0);
    IHS_BufferAppendUInt32LE(&nack.body, 0);
    IHS_BufferAppendUInt16LE(&nack.body, 10);
    IHS_BufferAppendUInt8(&nack.body, 2);
    LunarIHSRetransmissionFeedback(&session->retransmission, &nack);
    assert(pendingCount(session) == 1); ack(session, 12);
    IHS_SessionPacketClear(&nack, true);
    track(session, 13);
    auto malformed = packet(13, IHS_SessionPacketTypeACK, 0, 3);
    LunarIHSRetransmissionFeedback(&session->retransmission, &malformed);
    assert(pendingCount(session) == 1);
    IHS_SessionPacketClear(&malformed, true);
    // Dormant registration may not retransmit before the original is sent.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    unsigned retries = 0;
    LunarIHSTimerLock(session->timers);
    IHS_QueuePollEach(session->retransmission.queue, retryCount, &retries, unused, nullptr);
    LunarIHSTimerUnlock(session->timers);
    assert(retries == 0);
    sent.packetId = 13;
    LunarIHSRetransmissionInitialSent(&session->retransmission, &sent);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        LunarIHSTimerLock(session->timers);
        IHS_QueuePollEach(session->retransmission.queue, retryCount, &retries, unused, nullptr);
        LunarIHSTimerUnlock(session->timers);
    } while (retries <= 20 && std::chrono::steady_clock::now() < deadline);
    assert(retries > 20 && pendingCount(session) == 1); // survives the old 20-retry limit
    ack(session, 13); assert(pendingCount(session) == 0);
}
static void receiveWire(IHS_Session* session, IHS_SessionPacket* p, IHS_SocketAddress address) {
    IHS_SessionPacketPopulateBuffer(p);
    IHS_BufferExtendSize(&p->body);
    session->base.callbacks.received(&session->base, &address, &p->body);
    IHS_SessionPacketClear(p, true);
}
static void routingTests(IHS_Session* session) {
    session->state.connectionId = 3; session->state.hostConnectionId = 4;
    session->state.connectionState = IHS_SessionConnectionStateHandshaking;
    auto source = session->info.address;
    // Exercise production queue admission and wire-parser feedback, not just
    // the helper: ACK while the independent original is still queued.
    auto queued = packet(60, IHS_SessionPacketTypeReliable);
    assert(IHS_SessionQueuePacket(session, &queued, true));
    assert(!queued.body.data && pendingCount(session) == 1);
    auto sendAck = [&](int portDelta, uint8_t dst, uint8_t src) {
        auto p = packet(60, IHS_SessionPacketTypeACK, 0, 4);
        p.header.dstConnectionId = dst; p.header.srcConnectionId = src;
        auto from = source; from.port += portDelta;
        receiveWire(session, &p, from);
    };
    sendAck(1, 3, 4); assert(pendingCount(session) == 1);
    sendAck(0, 9, 4); assert(pendingCount(session) == 1);
    sendAck(0, 3, 9); assert(pendingCount(session) == 1);
    sendAck(0, 3, 4); assert(pendingCount(session) == 0);
    auto* item = IHS_QueuePoll(session->sendQueue);
    auto* original = reinterpret_cast<IHS_SessionPacket*>(item);
    assert(original->body.size == 1 && *IHS_BufferPointer(&original->body) == 0x33);
    IHS_SessionPacketClear(original, true); IHS_QueueItemFree(item);
    // Out-of-order control packets must not ACK a missing earlier datagram.
    auto sendReliable = [&](uint16_t id) {
        auto p = packet(id, IHS_SessionPacketTypeReliable);
        *IHS_BufferPointer(&p.body) = 255; // ignored unknown control message
        p.header.dstConnectionId = 3; p.header.srcConnectionId = 4;
        p.header.sendTimestamp = 1234;
        receiveWire(session, &p, source);
    };
    sendReliable(1);
    item = IHS_QueuePoll(session->sendQueue);
    auto* response = reinterpret_cast<IHS_SessionPacket*>(item);
    assert(response->header.type == IHS_SessionPacketTypeACK);
    assert(response->header.packetId == 65535);
    assert(response->body.size == 4);
    IHS_SessionPacketClear(response, true); IHS_QueueItemFree(item);
    sendReliable(0);
    item = IHS_QueuePoll(session->sendQueue);
    response = reinterpret_cast<IHS_SessionPacket*>(item);
    assert(response->header.packetId == 1);
    IHS_SessionPacketClear(response, true); IHS_QueueItemFree(item);
}

static void fragmentationTests(IHS_Session* session) {
    session->state.mtu = 64;
    auto* channel = IHS_SessionChannelFor(session, IHS_SessionChannelIdControl);
    // Boundary, exact multiple and final short fragment.
    for (size_t bytes : {size_t(51), size_t(102), size_t(103)}) {
        IHS_SessionFrame f{};
        IHS_SessionChannelInitializeFrame(channel, &f, IHS_SessionPacketTypeReliable, false, IHS_PACKET_ID_NEXT);
        uint16_t first = f.header.packetId;
        std::vector<uint8_t> body(bytes, 0x55);
        IHS_BufferAppendMem(&f.body, body.data(), body.size());
        assert(IHS_SessionChannelQueueFrame(channel, &f, false));
        auto* w = IHS_SessionPacketsWindowCreateReliable(8, first);
        struct Queued { IHS_SessionPacket packet; bool retransmit; };
        size_t count = 0;
        while (auto* item = IHS_QueuePoll(session->sendQueue)) {
            auto* q = reinterpret_cast<Queued*>(item);
            assert(q->packet.header.packetId == uint16_t(first + count));
            if (count == 0) assert(q->packet.header.fragmentId == (bytes + 50) / 51 - 1);
            assert(IHS_SessionPacketsWindowAdd(w, &q->packet));
            IHS_SessionPacketClear(&q->packet, true); IHS_QueueItemFree(item); ++count;
        }
        assert(count == (bytes + 50) / 51);
        IHS_SessionFrame assembled{}; IHS_BufferInit(&assembled.body, 8, 65536);
        assert(IHS_SessionPacketsWindowPoll(w, &assembled));
        assert(assembled.body.size == bytes);
        assert(std::memcmp(IHS_BufferPointer(&assembled.body), body.data(), bytes) == 0);
        IHS_SessionFrameClear(&assembled, true); IHS_SessionPacketsWindowDestroy(w);
        IHS_SessionFrameClear(&f, true);
    }
}
int main() {
    // A lost middle fragment blocks Poll even after newer complete frames arrive.
    // The old worker checked expiry once, then waited/repolled forever without
    // advancing that expiry. Exercise the exact helper used after every wakeup.
    for (unsigned cycle = 0; cycle < 200; ++cycle) {
        auto* window = IHS_SessionPacketsWindowCreate(8);
        uint16_t first = uint16_t(65500 + cycle * 7);
        auto head = packet(first, IHS_SessionPacketTypeUnreliable, 1);
        head.header.sendTimestamp = 1000;
        assert(IHS_SessionPacketsWindowAdd(window, &head));
        IHS_SessionFrame frame{}; IHS_BufferInit(&frame.body, 8, 65536);
        assert(!LunarIHSMediaWindowPoll(window, &frame));
        // first+1 is deliberately lost; first+2 is a complete later frame.
        auto next = packet(uint16_t(first + 2), IHS_SessionPacketTypeUnreliable);
        next.header.sendTimestamp = 1000 + IHS_SESSION_PACKET_TIMESTAMP_FROM_MILLIS(300);
        assert(IHS_SessionPacketsWindowAdd(window, &next));
        assert(!IHS_SessionPacketsWindowPoll(window, &frame)); // old wait loop wedges here
        assert(LunarIHSMediaWindowPoll(window, &frame));
        assert(frame.header.packetId == uint16_t(first + 2));
        assert(IHS_SessionPacketsWindowSize(window) == 0);
        auto burst = packet(uint16_t(first + 40), IHS_SessionPacketTypeUnreliable);
        burst.header.sendTimestamp = next.header.sendTimestamp + 30000;
        assert(IHS_SessionPacketsWindowAdd(window, &burst));
        IHS_SessionPacketsWindowReleaseFrame(&frame);
        assert(LunarIHSMediaWindowPoll(window, &frame));
        assert(frame.header.packetId == uint16_t(first + 40));
        IHS_SessionPacketsWindowReleaseFrame(&frame);
        auto reordered = packet(uint16_t(first + 41), IHS_SessionPacketTypeUnreliable, 1);
        auto following = packet(uint16_t(first + 43), IHS_SessionPacketTypeUnreliable);
        reordered.header.sendTimestamp = following.header.sendTimestamp = burst.header.sendTimestamp;
        assert(IHS_SessionPacketsWindowAdd(window, &reordered));
        assert(IHS_SessionPacketsWindowAdd(window, &following));
        assert(!LunarIHSMediaWindowPoll(window, &frame));
        auto late = packet(uint16_t(first + 42), IHS_SessionPacketTypeUnreliableFrag, 0);
        late.header.sendTimestamp = burst.header.sendTimestamp;
        assert(IHS_SessionPacketsWindowAdd(window, &late));
        assert(LunarIHSMediaWindowPoll(window, &frame));
        assert(frame.header.packetId == uint16_t(first + 41) && frame.body.size == 2);
        IHS_SessionPacketsWindowReleaseFrame(&frame);
        assert(LunarIHSMediaWindowPoll(window, &frame));
        assert(frame.header.packetId == uint16_t(first + 43));
        IHS_SessionFrameClear(&frame, true);
        IHS_SessionPacketsWindowDestroy(window);
    }
    std::cout << "PASS: 200 lost-middle-fragment wakeups recover without window overflow, including ID wrap\n";
    IHS_Init(); windowTests();
    uint8_t key[32]{};
    IHS_ClientConfig config{}; config.deviceId = 42; config.secretKey = key; config.deviceName = "Replay";
    IHS_SessionInfo info{};
    auto* session = IHS_SessionCreate(&config, &info);
    reliabilityTests(session); routingTests(session); fragmentationTests(session);
    // Destroy with an outstanding dormant task; timer cleanup must own its buffer.
    for (uint16_t id = 100; id < 1124; ++id) track(session, id);
    auto overflow = packet(1124, IHS_SessionPacketTypeReliable);
    assert(!IHS_RetransmissionQueue(&session->retransmission, &overflow));
    assert(overflow.body.data && pendingCount(session) == 1024);
    IHS_SessionPacketClear(&overflow, true);
    IHS_SessionDestroy(session); IHS_Quit();
    std::cout << "PASS: production cumulative/selective ACK, NACK retention, fast ACK, retry lifetime, wraparound, sparse windows, orphan media and fragment round trips\n";
}
