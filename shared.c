#define _GNU_SOURCE
#include "include/shared.h"
#include "include/protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stddef.h>
#include <poll.h>

typedef char shared_header_layout[(sizeof(shared_header_t) == 120 && offsetof(shared_header_t, reader) == 40 && offsetof(shared_header_t, played) == 72) ? 1 : -1];

void shared_init(shared_t *__shared)
{
	memset(__shared, 0, sizeof(*__shared));
	__shared->memory = -1;
	__shared->producerEvent = -1;
	__shared->consumerEvent = -1;
}

static int32_t _shared_map(shared_t *__shared, size_t __capacity)
{
	__shared->length = SHARED_HEADER_BYTES + __capacity;
	void *_mapping = mmap(NULL, __shared->length, PROT_READ | PROT_WRITE, MAP_SHARED, __shared->memory, 0);
	if (_mapping == MAP_FAILED)
	{
		return -1;
	}
	__shared->header = _mapping;
	ring_attach(&__shared->ring, (uint8_t *)_mapping + SHARED_HEADER_BYTES, __capacity, &__shared->header->reader, &__shared->header->writer);
	return 0;
}

int32_t shared_create(shared_t *__shared, uint64_t __epoch, uint32_t __rate, uint32_t __channels, uint32_t __sampleBytes, uint8_t __capture, size_t __capacity)
{
	shared_init(__shared);
	if (__capacity == 0 || __capacity > 1048576 || __channels < 1 || __channels > 2 || (__sampleBytes != 2 && __sampleBytes != 4) || __capacity % (__channels * __sampleBytes) != 0)
	{
		return -1;
	}
	__shared->memory = memfd_create("linux-audio-pcm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (__shared->memory < 0 || ftruncate(__shared->memory, SHARED_HEADER_BYTES + __capacity) != 0 || fcntl(__shared->memory, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0 || _shared_map(__shared, __capacity) != 0)
	{
		shared_close(__shared);
		return -1;
	}
	__shared->producerEvent = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	__shared->consumerEvent = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (__shared->producerEvent < 0 || __shared->consumerEvent < 0)
	{
		shared_close(__shared);
		return -1;
	}
	shared_header_t *_header = __shared->header;
	memset(_header, 0, __shared->length);
	_header->magic = SHARED_MAGIC;
	_header->version = SHARED_VERSION;
	_header->epoch = __epoch;
	_header->rate = __rate;
	_header->channels = __channels;
	_header->sampleBytes = __sampleBytes;
	_header->capture = __capture;
	_header->capacity = __capacity;
	__shared->epoch = __epoch;
	return 0;
}

int32_t shared_send(shared_t *__shared, int32_t __socket)
{
	int32_t _descriptors[3] = {__shared->memory, __shared->producerEvent, __shared->consumerEvent};
	for (size_t _index = 0; _index < 3; ++_index)
	{
		int32_t _sent = protocol_send_descriptor(__socket, _descriptors[_index]);
		if (_sent == 0)
		{
			struct pollfd _poll = {__socket, POLLOUT, 0};
			if (poll(&_poll, 1, 1000) > 0)
			{
				_sent = protocol_send_descriptor(__socket, _descriptors[_index]);
			}
		}
		if (_sent != 1)
		{
			return -1;
		}
	}
	return 0;
}

static uint8_t _shared_event(int32_t __descriptor)
{
	char _path[64];
	char _name[64];
	snprintf(_path, sizeof(_path), "/proc/self/fd/%d", __descriptor);
	ssize_t _length = readlink(_path, _name, sizeof(_name) - 1);
	if (_length < 0)
	{
		return 0;
	}
	_name[_length] = '\0';
	return strcmp(_name, "anon_inode:[eventfd]") == 0 && (fcntl(__descriptor, F_GETFL) & O_NONBLOCK) != 0;
}

int32_t shared_receive(shared_t *__shared, int32_t __socket, uint64_t __epoch, uint32_t __rate, uint32_t __channels, uint32_t __sampleBytes, uint8_t __capture)
{
	shared_init(__shared);
	__shared->memory = protocol_receive_descriptor(__socket);
	__shared->producerEvent = protocol_receive_descriptor(__socket);
	__shared->consumerEvent = protocol_receive_descriptor(__socket);
	struct stat _status;
	if (__shared->memory < 0 || !_shared_event(__shared->producerEvent) || !_shared_event(__shared->consumerEvent) || fstat(__shared->memory, &_status) != 0 || _status.st_size <= SHARED_HEADER_BYTES || _status.st_size > SHARED_HEADER_BYTES + 1048576)
	{
		goto fail;
	}
	int32_t _seals = fcntl(__shared->memory, F_GET_SEALS);
	if (_seals < 0 || (_seals & (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) != (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) || _shared_map(__shared, (size_t)_status.st_size - SHARED_HEADER_BYTES) != 0)
	{
		goto fail;
	}
	shared_header_t *_header = __shared->header;
	if (_header->magic != SHARED_MAGIC || _header->version != SHARED_VERSION || _header->epoch != __epoch || _header->rate != __rate || _header->channels != __channels || _header->sampleBytes != __sampleBytes || _header->capture != __capture || _header->capacity != __shared->ring.capacity || __channels < 1 || __channels > 2 || (__sampleBytes != 2 && __sampleBytes != 4) || __shared->ring.capacity % (__channels * __sampleBytes) != 0)
	{
		goto fail;
	}
	__shared->epoch = __epoch;
	return 0;
fail:
	shared_close(__shared);
	return -1;
}

void shared_notify(int32_t __event)
{
	uint64_t _value = 1;
	ssize_t _result;
	do
	{
		_result = write(__event, &_value, sizeof(_value));
	} while (_result < 0 && errno == EINTR);
}

void shared_clear(int32_t __event)
{
	uint64_t _value;
	ssize_t _result = read(__event, &_value, sizeof(_value));
	(void)_result;
}

void shared_close(shared_t *__shared)
{
	if (__shared->header != NULL)
	{
		munmap(__shared->header, __shared->length);
	}
	if (__shared->memory >= 0)
	{
		close(__shared->memory);
	}
	if (__shared->producerEvent >= 0)
	{
		close(__shared->producerEvent);
	}
	if (__shared->consumerEvent >= 0)
	{
		close(__shared->consumerEvent);
	}
	shared_init(__shared);
}
