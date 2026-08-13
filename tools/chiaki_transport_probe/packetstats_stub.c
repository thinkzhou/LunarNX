// SPDX-License-Identifier: AGPL-3.0-only

#include <chiaki/packetstats.h>

void chiaki_packet_stats_push_generation(ChiakiPacketStats *stats,
	uint64_t received, uint64_t lost)
{
	(void)stats;
	(void)received;
	(void)lost;
}
