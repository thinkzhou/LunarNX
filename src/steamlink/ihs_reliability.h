#pragma once

#include "session/retransmission.h"
#include "session/window.h"

// Receive-window adaptation of kxn/ihslib 04688d1 (LGPL-3.0-or-later).
// Keep the pinned vendor ABI; all additional state lives in tracked overrides.
IHS_SessionPacketsWindow *IHS_SessionPacketsWindowCreateReliable(uint16_t capacity, uint16_t firstId);
uint16_t IHS_SessionPacketsWindowContiguousId(const IHS_SessionPacketsWindow *window);
bool IHS_SessionPacketsWindowHasHole(const IHS_SessionPacketsWindow *window);
size_t IHS_SessionPacketsWindowHoleBitmap(const IHS_SessionPacketsWindow *window,
                                         uint16_t startId, uint8_t *bitmap, size_t maxPackets);
void LunarIHSRetransmissionInitialSent(IHS_SessionRetransmission *r,
                                      const IHS_SessionPacketHeader *header);
void LunarIHSRetransmissionFeedback(IHS_SessionRetransmission *r,
                                   const IHS_SessionPacket *packet);
