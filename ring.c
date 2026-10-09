#include "include/ring.h"
#include <stdlib.h>
#include <string.h>

int32_t ring_init(ring_t *__ring, size_t __capacity)
{
	memset(__ring, 0, sizeof(*__ring));
	__ring->bytes = calloc(__capacity, 1);
	if (__ring->bytes == NULL || __capacity == 0)
	{
		free(__ring->bytes);
		__ring->bytes = NULL;
		return -1;
	}
	__ring->capacity = __capacity;
	return 0;
}

void ring_destroy(ring_t *__ring)
{
	free(__ring->bytes);
	memset(__ring, 0, sizeof(*__ring));
}

size_t ring_available(const ring_t *__ring)
{
	uint64_t _reader = __atomic_load_n(&__ring->reader, __ATOMIC_ACQUIRE);
	uint64_t _writer = __atomic_load_n(&__ring->writer, __ATOMIC_ACQUIRE);
	return (size_t)(_writer - _reader);
}

size_t ring_write(ring_t *__ring, const void *__data, size_t __length)
{
	uint64_t _writer = __atomic_load_n(&__ring->writer, __ATOMIC_RELAXED);
	uint64_t _reader = __atomic_load_n(&__ring->reader, __ATOMIC_ACQUIRE);
	size_t _space = __ring->capacity - (size_t)(_writer - _reader);
	if (__length > _space)
	{
		return 0; /* Whole frames only; callers count overflows. */
	}
	size_t _offset = (size_t)(_writer % __ring->capacity);
	size_t _first = __ring->capacity - _offset;
	if (_first > __length)
	{
		_first = __length;
	}
	memcpy(__ring->bytes + _offset, __data, _first);
	memcpy(__ring->bytes, (const uint8_t *)__data + _first, __length - _first);
	__atomic_store_n(&__ring->writer, _writer + __length, __ATOMIC_RELEASE);
	return __length;
}

size_t ring_read(ring_t *__ring, void *__data, size_t __length)
{
	uint64_t _reader = __atomic_load_n(&__ring->reader, __ATOMIC_RELAXED);
	uint64_t _writer = __atomic_load_n(&__ring->writer, __ATOMIC_ACQUIRE);
	if (__atomic_exchange_n(&__ring->discard, 0, __ATOMIC_ACQ_REL) != 0)
	{
		__atomic_store_n(&__ring->reader, _writer, __ATOMIC_RELEASE);
		return 0;
	}
	size_t _available = (size_t)(_writer - _reader);
	if (__length > _available)
	{
		__length = _available;
	}
	size_t _offset = (size_t)(_reader % __ring->capacity);
	size_t _first = __ring->capacity - _offset;
	if (_first > __length)
	{
		_first = __length;
	}
	memcpy(__data, __ring->bytes + _offset, _first);
	memcpy((uint8_t *)__data + _first, __ring->bytes, __length - _first);
	__atomic_store_n(&__ring->reader, _reader + __length, __ATOMIC_RELEASE);
	return __length;
}

void ring_discard(ring_t *__ring)
{
	__atomic_store_n(&__ring->discard, 1, __ATOMIC_RELEASE);
}
