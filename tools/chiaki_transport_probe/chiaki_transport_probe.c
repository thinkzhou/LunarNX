// SPDX-License-Identifier: AGPL-3.0-only

#include <chiaki/fec.h>
#include <chiaki/frameprocessor.h>
#include <chiaki/log.h>
#include <chiaki/reorderqueue.h>

#include <arpa/inet.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct
{
	uint64_t dropped;
} DropStats;

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int compare_u64(const void *a, const void *b)
{
	uint64_t av = *(const uint64_t *)a;
	uint64_t bv = *(const uint64_t *)b;
	return av < bv ? -1 : av > bv;
}

static void drop_packet(uint64_t seq_num, void *elem_user, void *cb_user)
{
	(void)seq_num;
	DropStats *stats = cb_user;
	stats->dropped++;
	free(elem_user);
}

static int probe_reorder(size_t size_exp, unsigned int units)
{
	ChiakiReorderQueue queue;
	DropStats stats = { 0 };
	if(chiaki_reorder_queue_init_16(&queue, size_exp, 1000) != CHIAKI_ERR_SUCCESS)
		return 1;
	chiaki_reorder_queue_set_drop_cb(&queue, drop_packet, &stats);

	// Hold the first packet until the complete remainder of a burst has arrived.
	for(unsigned int i = 1; i < units; i++)
	{
		unsigned int *value = malloc(sizeof(*value));
		*value = i;
		chiaki_reorder_queue_push(&queue, 1000 + i, value);
	}
	unsigned int *head = malloc(sizeof(*head));
	*head = 0;
	chiaki_reorder_queue_push(&queue, 1000, head);

	uint64_t pulled = 0;
	void *value = NULL;
	while(chiaki_reorder_queue_pull(&queue, NULL, &value))
	{
		free(value);
		pulled++;
	}
	printf("reorder capacity=%zu burst_units=%u retained=%" PRIu64 " dropped=%" PRIu64 " result=%s\n",
		((size_t)1) << size_exp, units, pulled, stats.dropped,
		pulled == units && stats.dropped == 0 ? "PASS" : "DROP");
	chiaki_reorder_queue_fini(&queue);
	return 0;
}

static void silent_log(ChiakiLogLevel level, const char *msg, void *user)
{
	(void)level;
	(void)msg;
	(void)user;
}

static int probe_frame(unsigned int source_units, unsigned int fec_units,
	unsigned int erased_sources, unsigned int iterations)
{
	const size_t unit_size = 1400;
	const size_t stride = 1408;
	const unsigned int total_units = source_units + fec_units;
	const size_t frame_size = stride * total_units;
	uint8_t *encoded = calloc(1, frame_size);
	uint64_t *samples = calloc(iterations, sizeof(*samples));
	if(!encoded || !samples)
		return 1;

	for(unsigned int unit = 0; unit < source_units; unit++)
	{
		uint8_t *data = encoded + stride * unit;
		*((uint16_t *)data) = htons(0);
		for(size_t i = 2; i < unit_size; i++)
			data[i] = (uint8_t)((unit * 31 + i * 17) & 0xff);
	}
	if(chiaki_fec_encode(encoded, unit_size, stride, source_units, fec_units)
			!= CHIAKI_ERR_SUCCESS)
		return 1;

	ChiakiLog log;
	chiaki_log_init(&log, 0, silent_log, NULL);
	for(unsigned int iteration = 0; iteration < iterations; iteration++)
	{
		ChiakiFrameProcessor processor;
		chiaki_frame_processor_init(&processor, &log);
		ChiakiTakionAVPacket packet = { 0 };
		packet.is_video = true;
		packet.units_in_frame_total = total_units;
		packet.units_in_frame_fec = fec_units;
		packet.data_size = unit_size;
		packet.data = encoded;

		uint64_t start = now_ns();
		if(chiaki_frame_processor_alloc_frame(&processor, &packet) != CHIAKI_ERR_SUCCESS)
			return 1;
		for(unsigned int unit = 0; unit < total_units; unit++)
		{
			if(unit < erased_sources)
				continue;
			packet.unit_index = unit;
			packet.data = encoded + stride * unit;
			if(chiaki_frame_processor_put_unit(&processor, &packet) != CHIAKI_ERR_SUCCESS)
				return 1;
		}
		uint8_t *frame = NULL;
		size_t output_size = 0;
		ChiakiFrameProcessorFlushResult result =
			chiaki_frame_processor_flush(&processor, &frame, &output_size);
		samples[iteration] = now_ns() - start;
		if((erased_sources == 0 && result != CHIAKI_FRAME_PROCESSOR_FLUSH_RESULT_SUCCESS) ||
			(erased_sources > 0 && result != CHIAKI_FRAME_PROCESSOR_FLUSH_RESULT_FEC_SUCCESS))
			return 1;
		chiaki_frame_processor_fini(&processor);
	}

	qsort(samples, iterations, sizeof(*samples), compare_u64);
	double frame_mbit = (double)source_units * (double)(unit_size - 2) * 8.0 / 1000000.0;
	printf("frame source=%u fec=%u erased=%u payload=%.3fMbit iterations=%u p50=%.3fms p95=%.3fms p99=%.3fms\n",
		source_units, fec_units, erased_sources, frame_mbit, iterations,
		samples[iterations / 2] / 1000000.0,
		samples[(iterations * 95) / 100] / 1000000.0,
		samples[(iterations * 99) / 100] / 1000000.0);
	free(samples);
	free(encoded);
	return 0;
}

int main(void)
{
	printf("Chiaki transport probe (native host baseline; not Switch timing)\n");
	if(probe_reorder(6, 94) || probe_reorder(8, 94))
		return 1;

	const unsigned int scenarios[][3] = {
		{ 32, 4, 0 }, { 32, 4, 1 },
		{ 48, 5, 0 }, { 48, 5, 2 },
		{ 88, 6, 0 }, { 88, 6, 4 },
		{ 176, 12, 0 }, { 176, 12, 8 },
	};
	for(size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
	{
		if(probe_frame(scenarios[i][0], scenarios[i][1], scenarios[i][2], 300))
			return 1;
	}
	return 0;
}
