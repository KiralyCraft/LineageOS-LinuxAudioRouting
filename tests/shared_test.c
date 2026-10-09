#define _GNU_SOURCE
#include "../include/shared.h"
#include "../include/protocol.h"
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void _shared_test_transfer(uint8_t __capture)
{
	int32_t _pair[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, _pair) == 0);
	shared_t _shared;
	assert(shared_create(&_shared, 1234, 48000, 2, 4, __capture, 4096) == 0);
	assert(ftruncate(_shared.memory, 1) == -1);
	pid_t _child = fork();
	assert(_child >= 0);
	if (_child == 0)
	{
		close(_pair[0]);
		shared_close(&_shared);
		assert(shared_receive(&_shared, _pair[1], 1234, 48000, 2, 4, __capture) == 0);
	}
	else
	{
		close(_pair[1]);
		assert(shared_send(&_shared, _pair[0]) == 0);
	}
	uint8_t _producer = _child != 0;
	if (__capture)
	{
		_producer = !_producer;
	}
	uint8_t _data[504];
	size_t _position = 0;
	while (_position < 1048576)
	{
		size_t _length = sizeof(_data);
		if (_length > 1048576 - _position)
		{
			_length = 1048576 - _position;
		}
		int32_t _event = _shared.producerEvent;
		if (_producer)
		{
			_event = _shared.consumerEvent;
		}
		if ((_producer && _shared.ring.capacity - ring_available(&_shared.ring) < _length) || (!_producer && ring_available(&_shared.ring) < _length))
		{
			struct pollfd _poll = {_event, POLLIN, 0};
			assert(poll(&_poll, 1, 3000) > 0);
			shared_clear(_event);
			continue;
		}
		if (_producer)
		{
			for (size_t _index = 0; _index < _length; ++_index)
			{
				_data[_index] = (uint8_t)((_position + _index) * 31 + ((_position + _index) >> 8));
			}
			assert(ring_write(&_shared.ring, _data, _length) == _length);
			shared_notify(_shared.producerEvent);
		}
		else
		{
			assert(ring_read(&_shared.ring, _data, _length) == _length);
			for (size_t _index = 0; _index < _length; ++_index)
			{
				assert(_data[_index] == (uint8_t)((_position + _index) * 31 + ((_position + _index) >> 8)));
			}
			shared_notify(_shared.consumerEvent);
		}
		_position += _length;
	}
	shared_close(&_shared);
	if (_child == 0)
	{
		close(_pair[1]);
		_exit(0);
	}
	int32_t _status;
	assert(waitpid(_child, &_status, 0) == _child && WIFEXITED(_status) && WEXITSTATUS(_status) == 0);
	close(_pair[0]);
}

int main(void)
{
	_shared_test_transfer(0);
	_shared_test_transfer(1);
	int32_t _pair[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, _pair) == 0);
	shared_t _producer;
	shared_t _consumer;
	assert(shared_create(&_producer, 44, 48000, 1, 4, 0, 1024) == 0);
	assert(shared_send(&_producer, _pair[0]) == 0);
	assert(shared_receive(&_consumer, _pair[1], 45, 48000, 1, 4, 0) == -1);
	__atomic_store_n(&_producer.header->writer, 2048, __ATOMIC_RELEASE);
	uint8_t _data[8] = {0};
	assert(ring_read(&_producer.ring, _data, sizeof(_data)) == 0);
	assert(ring_write(&_producer.ring, _data, sizeof(_data)) == 0);
	shared_close(&_producer);
	close(_pair[0]);
	close(_pair[1]);
	puts("PASS shared PCM: sealed allocation, bidirectional cross-process exact wraparound, event wakeup, epoch mismatch and corrupt counters");
	return 0;
}
