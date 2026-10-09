#ifndef AUDIO_TRANSPORT_H
#define AUDIO_TRANSPORT_H
#include "control.h"
#include "ring.h"
typedef void (*transport_event_t)(void *__context);
typedef struct
{
	control_t *control;
	int32_t socket;
	int32_t wake;
	int32_t live;
	int32_t ready;
	int32_t failed;
	uint32_t drainSerial;
	uint32_t drainedSerial;
	uint8_t threadStarted;
	uint8_t capture;
	uint32_t rate;
	uint32_t channels;
	uint32_t sampleBytes;
	uint32_t prefillFrames;
	uint64_t stream;
	uint64_t epoch;
	uint64_t sentFrames;
	uint64_t playedFrames;
	uint64_t clockFrame;
	uint64_t clockTime;
	uint32_t clockSerial;
	uint64_t xruns;
	pthread_t thread;
	ring_t ring;
	transport_event_t event;
	void *context;
} transport_t;
int32_t transport_start(transport_t *__transport, control_t *__control, const cJSON *__device, uint64_t __generation, transport_event_t __event, void *__context);
void transport_stop(transport_t *__transport);
int32_t transport_drain(transport_t *__transport);
void transport_wake(transport_t *__transport);
void transport_fail(transport_t *__transport);
uint8_t transport_live(const transport_t *__transport);
#endif
