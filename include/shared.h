#ifndef AUDIO_SHARED_H
#define AUDIO_SHARED_H
#include "ring.h"
#define SHARED_HEADER_BYTES 4096
#define SHARED_MAGIC UINT32_C(0x53445541)
#define SHARED_VERSION 1
/* Fixed-width, naturally aligned AArch64 ABI. Counters count bytes, never slots.
 * Reader and writer each have exactly one owner. No process-local pointers. */
typedef struct
{
	uint32_t magic;
	uint32_t version;
	uint64_t epoch;
	uint32_t rate;
	uint32_t channels;
	uint32_t sampleBytes;
	uint32_t capture;
	uint64_t capacity;
	uint64_t reader;
	uint64_t writer;
	uint32_t stopped;
	int32_t error;
	uint32_t serial;
	uint32_t reserved;
	uint64_t played;
	uint64_t clockFrame;
	uint64_t clockTime;
	uint64_t presentationFrame;
	uint64_t presentationTime;
	uint32_t mmapUsed;
	uint32_t xruns;
} shared_header_t;
typedef struct
{
	int32_t memory;
	int32_t producerEvent;
	int32_t consumerEvent;
	shared_header_t *header;
	size_t length;
	uint64_t epoch;
	ring_t ring;
} shared_t;
void shared_init(shared_t *__shared);
int32_t shared_create(shared_t *__shared, uint64_t __epoch, uint32_t __rate, uint32_t __channels, uint32_t __sampleBytes, uint8_t __capture, size_t __capacity);
int32_t shared_send(shared_t *__shared, int32_t __socket);
int32_t shared_receive(shared_t *__shared, int32_t __socket, uint64_t __epoch, uint32_t __rate, uint32_t __channels, uint32_t __sampleBytes, uint8_t __capture);
void shared_notify(int32_t __event);
void shared_clear(int32_t __event);
void shared_close(shared_t *__shared);
#endif
