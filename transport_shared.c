#include "include/transport.h"
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

void *transport_shared_worker(void *__context)
{
	transport_t *_transport = __context;
	shared_header_t *_header = _transport->shared.header;
	uint32_t _frameBytes = _transport->channels * _transport->sampleBytes;
	int32_t _event = _transport->shared.consumerEvent;
	if (_transport->capture != 0)
	{
		_event = _transport->shared.producerEvent;
	}
	while (transport_live(_transport) != 0 && __atomic_load_n(&_transport->failed, __ATOMIC_ACQUIRE) == 0)
	{
		/* This observer owns neither counter. Retry a torn snapshot when
		 * producer and consumer advance between the two loads. */
		uint64_t _reader;
		uint64_t _writer;
		uint64_t _readerAfter;
		do
		{
			_reader = __atomic_load_n(&_header->reader, __ATOMIC_ACQUIRE);
			_writer = __atomic_load_n(&_header->writer, __ATOMIC_ACQUIRE);
			_readerAfter = __atomic_load_n(&_header->reader, __ATOMIC_ACQUIRE);
		} while (_reader != _readerAfter);
		if (_writer - _reader > _transport->ring.capacity || (_reader % _frameBytes) != 0 || (_writer % _frameBytes) != 0 || __atomic_load_n(&_header->error, __ATOMIC_ACQUIRE) != 0 || __atomic_load_n(&_header->stopped, __ATOMIC_ACQUIRE) != 0)
		{
			if (transport_live(_transport) != 0)
			{
				transport_fail_reason(_transport, 20);
			}
			break;
		}
		if (_transport->capture != 0)
		{
			if (_writer - _reader >= (uint64_t)_transport->prefillFrames * _frameBytes)
			{
				__atomic_store_n(&_transport->ready, 1, __ATOMIC_RELEASE);
			}
		}
		else
		{
			__atomic_store_n(&_transport->sentFrames, _writer / _frameBytes, __ATOMIC_RELEASE);
			__atomic_store_n(&_transport->acceptedFrames, _reader / _frameBytes, __ATOMIC_RELEASE);
		}
		uint32_t _before = __atomic_load_n(&_header->serial, __ATOMIC_ACQUIRE);
		uint64_t _played = __atomic_load_n(&_header->played, __ATOMIC_RELAXED);
		uint64_t _frame = __atomic_load_n(&_header->clockFrame, __ATOMIC_RELAXED);
		uint64_t _time = __atomic_load_n(&_header->clockTime, __ATOMIC_RELAXED);
		uint64_t _presentationFrame = __atomic_load_n(&_header->presentationFrame, __ATOMIC_RELAXED);
		uint64_t _presentationTime = __atomic_load_n(&_header->presentationTime, __ATOMIC_RELAXED);
		uint32_t _after = __atomic_load_n(&_header->serial, __ATOMIC_ACQUIRE);
		if (_before == _after && (_before & 1) == 0)
		{
			__atomic_store_n(&_transport->playedFrames, _played, __ATOMIC_RELEASE);
			__atomic_fetch_add(&_transport->clockSerial, 1, __ATOMIC_ACQ_REL);
			__atomic_store_n(&_transport->clockFrame, _frame, __ATOMIC_RELAXED);
			__atomic_store_n(&_transport->clockTime, _time, __ATOMIC_RELAXED);
			__atomic_fetch_add(&_transport->clockSerial, 1, __ATOMIC_RELEASE);
			if (_transport->presentationClock != 0)
			{
				__atomic_fetch_add(&_transport->presentationSerial, 1, __ATOMIC_ACQ_REL);
				__atomic_store_n(&_transport->presentationFrame, _presentationFrame, __ATOMIC_RELAXED);
				__atomic_store_n(&_transport->presentationTime, _presentationTime, __ATOMIC_RELAXED);
				__atomic_fetch_add(&_transport->presentationSerial, 1, __ATOMIC_RELEASE);
			}
		}
		uint32_t _drain = __atomic_load_n(&_transport->drainSerial, __ATOMIC_ACQUIRE);
		if (_drain != 0 && _writer == _reader && _played >= _writer / _frameBytes)
		{
			__atomic_store_n(&_transport->drainedSerial, _drain, __ATOMIC_RELEASE);
		}
		if (_transport->event != NULL)
		{
			_transport->event(_transport->context);
		}
		struct pollfd _poll[3] = {{_transport->socket, POLLIN, 0}, {_transport->wake, POLLIN, 0}, {_event, POLLIN, 0}};
		int32_t _result = poll(_poll, 3, -1);
		if (_result < 0 && errno == EINTR)
		{
			continue;
		}
		if (_result < 0 || _poll[0].revents != 0 || (_poll[2].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
		{
			if (transport_live(_transport) != 0)
			{
				transport_fail_reason(_transport, 21);
			}
			break;
		}
		if ((_poll[1].revents & POLLIN) != 0)
		{
			shared_clear(_transport->wake);
		}
		if ((_poll[2].revents & POLLIN) != 0)
		{
			shared_clear(_event);
		}
	}
	__atomic_store_n(&_transport->live, 0, __ATOMIC_RELEASE);
	if (_transport->event != NULL)
	{
		_transport->event(_transport->context);
	}
	return NULL;
}
