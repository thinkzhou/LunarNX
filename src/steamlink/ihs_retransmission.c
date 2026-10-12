/* Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * LunarNX adaptation of the reliable delivery fixes in kxn/ihslib 04688d1.
 * Retain the pinned IHS_SessionRetransmission ABI and our timer lock order.
 */
#include "ihs_reliability.h"
#include "ihs_streaming_support.h"
#include "session/session_pri.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

#define RETRY_MS 25

typedef struct PendingPacket {
    IHS_SessionPacket packet;
    IHS_TimerTask *task;
    IHS_SessionRetransmission *owner;
    bool initialSent;
    uint64_t lastSendMs;
} PendingPacket;

typedef struct Feedback {
    const IHS_SessionPacket *packet;
    bool exact;
    uint16_t fragmentId;
} Feedback;

static bool Identical(IHS_QueueItem *item, void *context) { return item == context; }

// All access to pending packets and task pointers uses timer -> retransmission.
// Timer callbacks already hold the first lock. The timer end callback never
// takes timer->mutex, including when IHS_TimerDestroy drains the session.
static void RetryEnd(void *context) {
    PendingPacket *pending = context;
    IHS_SessionRetransmission *r = pending->owner;
    IHS_MutexLock(r->lock);
    IHS_QueuePollBy(r->queue, Identical, pending);
    IHS_MutexUnlock(r->lock);
    IHS_SessionPacketClear(&pending->packet, true);
    IHS_QueueItemFree((IHS_QueueItem *)pending);
}

static uint64_t RetryRun(int count, void *context) {
    (void)count;
    PendingPacket *pending = context;
    if (pending->owner->session->base.interrupted) return 0;
    if (!pending->initialSent || IHS_TimerNow() - pending->lastSendMs < RETRY_MS) {
        return RETRY_MS;
    }
    // Reliable control messages must survive until acknowledgement/disconnect.
    // Giving up after 20 attempts leaves a permanent encrypted sequence hole.
    if (pending->packet.header.type == IHS_SessionPacketTypeConnect &&
        pending->packet.header.retransmitCount >= 20) return 0;
    if (pending->packet.header.retransmitCount < UINT8_MAX) {
        pending->packet.header.retransmitCount++;
    }
    IHS_SessionSendPacket(pending->owner->session, &pending->packet);
    pending->lastSendMs = IHS_TimerNow();
    return RETRY_MS;
}

void IHS_RetransmissionInit(IHS_SessionRetransmission *r, IHS_Session *session) {
    r->session = session;
    r->lock = IHS_MutexCreate();
    r->queue = IHS_QueueCreate(sizeof(PendingPacket));
}

void IHS_RetransmissionDeinit(IHS_SessionRetransmission *r) {
    // Session destruction drains timers first, including RetryEnd for every
    // outstanding packet. Do not dereference the now-destroyed timer here.
    assert(IHS_QueueIsEmpty(r->queue));
    IHS_QueueDestroy(r->queue, NULL, NULL);
    IHS_MutexDestroy(r->lock);
}

static bool CountPending(IHS_QueueItem *item, void *context) {
    (void)item;
    ++*(size_t *)context;
    return false;
}
static void IgnorePending(IHS_QueueItem *item, void *context) { (void)item; (void)context; }

bool IHS_RetransmissionQueue(IHS_SessionRetransmission *r, IHS_SessionPacket *packet) {
    LunarIHSTimerLock(r->session->timers);
    if (r->session->base.interrupted) {
        LunarIHSTimerUnlock(r->session->timers);
        return false;
    }
    // Bound memory and keep outstanding IDs well inside the half-range used
    // by cumulative ACK arithmetic. A failed control enqueue ends the session.
    size_t count = 0;
    IHS_MutexLock(r->lock);
    IHS_QueuePollEach(r->queue, CountPending, &count, IgnorePending, NULL);
    IHS_MutexUnlock(r->lock);
    if (count >= 1024) {
        LunarIHSTimerUnlock(r->session->timers);
        return false;
    }
    PendingPacket *pending = (void *)IHS_QueueItemObtain(r->queue);
    pending->owner = r;
    pending->packet.header = packet->header;
    pending->packet.crc = packet->crc;
    // The sender retains its own buffer; an ACK may release this copy before
    // the send worker returns from sendto().
    IHS_SessionPacketBodyInitialize(&pending->packet.body, packet->header.hasCrc);
    IHS_BufferAppendMem(&pending->packet.body, IHS_BufferPointer(&packet->body), packet->body.size);
    IHS_MutexLock(r->lock);
    IHS_QueueAppend(r->queue, (void *)pending);
    IHS_MutexUnlock(r->lock);
    pending->task = LunarIHSTimerStartLocked(r->session->timers, RetryRun, RetryEnd, RETRY_MS, pending);
    bool ok = pending->task != NULL;
    if (!ok) RetryEnd(pending);
    LunarIHSTimerUnlock(r->session->timers);
    return ok;
}

static bool InitialSent(IHS_QueueItem *item, void *context) {
    PendingPacket *pending = (void *)item;
    const IHS_SessionPacketHeader *header = context;
    if (pending->packet.header.channelId == header->channelId &&
        pending->packet.header.packetId == header->packetId &&
        pending->packet.header.fragmentId == header->fragmentId) {
        pending->initialSent = true;
        pending->lastSendMs = IHS_TimerNow();
        pending->packet.header.sendTimestamp = header->sendTimestamp;
    }
    return false;
}

static void UnusedDestroy(IHS_QueueItem *item, void *context) { (void)item; (void)context; }

void LunarIHSRetransmissionInitialSent(IHS_SessionRetransmission *r,
                                      const IHS_SessionPacketHeader *header) {
    LunarIHSTimerLock(r->session->timers);
    IHS_MutexLock(r->lock);
    IHS_QueuePollEach(r->queue, InitialSent, (void *)header, UnusedDestroy, NULL);
    IHS_MutexUnlock(r->lock);
    LunarIHSTimerUnlock(r->session->timers);
}

static bool FeedbackMatch(IHS_QueueItem *item, void *context) {
    PendingPacket *pending = (void *)item;
    const Feedback *feedback = context;
    const IHS_SessionPacket *packet = feedback->packet;
    const IHS_SessionPacketHeader *header = &pending->packet.header;
    if (header->channelId != packet->header.channelId) return false;
    uint16_t distance = (uint16_t)(packet->header.packetId - header->packetId);
    if (feedback->exact) {
        return distance == 0 && (uint16_t)header->fragmentId == feedback->fragmentId;
    }
    if (packet->header.type == IHS_SessionPacketTypeACK) {
        return distance < 0x8000u;
    }
    // Extended NACK: timestamp, contiguous ID, then presence bitmap.
    // Presence is a positive receipt; missing IDs remain outstanding.
    const uint8_t *body = IHS_BufferPointer(&packet->body);
    if (packet->body.size >= 6) {
        uint16_t contiguous = (uint16_t)(body[4] | (uint16_t)body[5] << 8);
        if ((uint16_t)(contiguous - header->packetId) < 0x8000u) return true;
        uint16_t bit = (uint16_t)(header->packetId - packet->header.packetId);
        size_t bytes = packet->body.size - 6;
        if (bit / 8 < bytes && (body[6 + bit / 8] & (1u << (bit % 8)))) return true;
    }
    // Periodic retransmission will heal missing packets. Unlike the pinned
    // implementation, a NACK never retires the missing encrypted message.
    return false;
}

static bool CancelMatching(IHS_SessionRetransmission *r, const Feedback *feedback) {
    bool found = false;
    LunarIHSTimerLock(r->session->timers);
    for (;;) {
        IHS_MutexLock(r->lock);
        PendingPacket *pending = (void *)IHS_QueuePollBy(r->queue, FeedbackMatch, (void *)feedback);
        IHS_MutexUnlock(r->lock);
        if (!pending) break;
        found = true;
        // The end callback frees pending. Keep the timer lock until it returns
        // so a concurrent retry cannot send or free the same packet.
        LunarIHSTimerRemoveLocked(pending->task);
    }
    LunarIHSTimerUnlock(r->session->timers);
    return found;
}

bool IHS_RetransmissionCancel(IHS_SessionRetransmission *r, IHS_SessionChannelId channelId,
                              uint16_t packetId, uint16_t fragmentId) {
    IHS_SessionPacket packet = {.header = {.channelId = channelId, .packetId = packetId}};
    Feedback feedback = {.packet = &packet, .exact = true, .fragmentId = fragmentId};
    return CancelMatching(r, &feedback);
}

void LunarIHSRetransmissionFeedback(IHS_SessionRetransmission *r,
                                   const IHS_SessionPacket *packet) {
    if ((packet->header.type == IHS_SessionPacketTypeACK && packet->body.size < 4) ||
        (packet->header.type == IHS_SessionPacketTypeNACK &&
         packet->body.size != 0 && packet->body.size < 6)) return;
    Feedback feedback = {.packet = packet};
    CancelMatching(r, &feedback);
}
