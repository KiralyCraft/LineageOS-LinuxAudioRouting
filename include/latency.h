#ifndef AUDIO_LATENCY_H
#define AUDIO_LATENCY_H
#include <stdint.h>

/* A timestamp describes when one frame reaches Android's presentation point.
 * It is neither an acceptance credit nor an instruction to discard audio. */
typedef struct
{
	uint64_t frame;
	uint64_t time;
} latency_t;
uint8_t latency_estimate(const latency_t *__point, uint64_t __produced, uint32_t __rate, uint64_t __now, uint64_t *__delay);
#endif
