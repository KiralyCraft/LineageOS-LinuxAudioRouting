#ifndef AUDIO_RING_H
#define AUDIO_RING_H
#include <stddef.h>
#include <stdint.h>
typedef struct
{
	uint8_t *bytes;
	size_t capacity;
	uint64_t reader;
	uint64_t writer;
	uint32_t discard;
} ring_t;
int32_t ring_init(ring_t *__ring, size_t __capacity);
void ring_destroy(ring_t *__ring);
size_t ring_available(const ring_t *__ring);
size_t ring_write(ring_t *__ring, const void *__data, size_t __length);
size_t ring_read(ring_t *__ring, void *__data, size_t __length);
void ring_discard(ring_t *__ring);
#endif
