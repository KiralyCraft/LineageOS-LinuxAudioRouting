#define _GNU_SOURCE
#include "include/transport.h"
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

uint8_t transport_live(const transport_t *__transport)
{
	return __atomic_load_n(&__transport->live, __ATOMIC_ACQUIRE) != 0;
}

void transport_wake(transport_t *__transport)
{
	uint64_t _value = 1;
	if (__transport->wake >= 0)
	{
		ssize_t _written = write(__transport->wake, &_value, sizeof(_value));
		(void)_written;
	}
}

void transport_fail(transport_t *__transport)
{
	__atomic_store_n(&__transport->failed, 1, __ATOMIC_RELEASE);
	transport_wake(__transport);
}

static void *_transport_worker(void *__context)
{
	transport_t *_transport = __context;
	protocol_t *_packet = calloc(1, sizeof(*_packet));
	uint64_t _nextFrame = 0;
	uint64_t _notifiedFrame = 0;
	uint32_t _frameBytes = _transport->channels * _transport->sampleBytes;
	size_t _packetBytes = (size_t)(_transport->rate / 100) * _frameBytes;
	if (_packet == NULL)
	{
		transport_fail(_transport);
	}
	while (_packet != NULL && transport_live(_transport) != 0 && __atomic_load_n(&_transport->failed, __ATOMIC_ACQUIRE) == 0)
	{
		struct pollfd _poll[2] = {{_transport->socket, POLLIN, 0}, {_transport->wake, POLLIN, 0}};
		int32_t _result = poll(_poll, 2, -1);
		if (_result < 0 && errno == EINTR)
		{
			continue;
		}
		if (_result < 0 || (_poll[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
		{
			transport_fail(_transport);
			break;
		}
		uint32_t _drainSerial = __atomic_load_n(&_transport->drainSerial, __ATOMIC_ACQUIRE);
		if ((_poll[1].revents & POLLIN) != 0)
		{
			uint64_t _value;
			ssize_t _read = read(_transport->wake, &_value, sizeof(_value));
			(void)_read;
		}
		if ((_poll[0].revents & POLLIN) != 0)
		{
			if (protocol_receive_pcm(_transport->socket, _packet) != 0 || _packet->epoch != _transport->epoch)
			{
				transport_fail(_transport);
				break;
			}
			if (_transport->capture != 0)
			{
				if (_packet->kind != AUDIO_PCM_DATA || _packet->frame != _nextFrame || _packet->frames == 0 || _packet->length != (size_t)_packet->frames * _frameBytes || ring_write(&_transport->ring, _packet->data, _packet->length) != _packet->length)
				{
					transport_fail(_transport);
					break;
				}
				_nextFrame += _packet->frames;
				if (ring_available(&_transport->ring) >= (size_t)(_transport->rate / 50) * _frameBytes)
				{
					__atomic_store_n(&_transport->ready, 1, __ATOMIC_RELEASE);
				}
			}
			else
			{
				if (_packet->kind != AUDIO_PCM_CLOCK || _packet->length != 0 || _packet->frame > _transport->sentFrames || _packet->frame < _transport->playedFrames)
				{
					transport_fail(_transport);
					break;
				}
				__atomic_store_n(&_transport->playedFrames, _packet->frame, __ATOMIC_RELEASE);
			}
			__atomic_fetch_add(&_transport->clockSerial, 1, __ATOMIC_ACQ_REL);
			__atomic_store_n(&_transport->clockFrame, _packet->frame, __ATOMIC_RELAXED);
			__atomic_store_n(&_transport->clockTime, _packet->timestamp, __ATOMIC_RELAXED);
			__atomic_fetch_add(&_transport->clockSerial, 1, __ATOMIC_RELEASE);
			if (_packet->frame - _notifiedFrame >= _transport->rate / 10)
			{
				_notifiedFrame = _packet->frame;
				if (_transport->event != NULL)
				{
					_transport->event(_transport->context);
				}
			}
		}
		if (_transport->capture == 0)
		{
			while (transport_live(_transport) != 0 && ring_available(&_transport->ring) >= _frameBytes && _transport->sentFrames - _transport->playedFrames < _transport->rate / 50)
			{
				size_t _length = ring_available(&_transport->ring);
				if (_length > _packetBytes)
				{
					_length = _packetBytes;
				}
				_length -= _length % _frameBytes;
				_packet->kind = AUDIO_PCM_DATA;
				_packet->epoch = _transport->epoch;
				_packet->frame = _transport->sentFrames;
				_packet->timestamp = protocol_now();
				_packet->frames = (uint32_t)(_length / _frameBytes);
				_packet->length = ring_read(&_transport->ring, _packet->data, _length);
				if (_packet->length != _length || protocol_send_pcm(_transport->socket, _packet) != 0)
				{
					transport_fail(_transport);
					break;
				}
				__atomic_fetch_add(&_transport->sentFrames, _packet->frames, __ATOMIC_RELEASE);
			}
		}
		if (_drainSerial != 0 && __atomic_load_n(&_transport->drainSerial, __ATOMIC_ACQUIRE) == _drainSerial && ring_available(&_transport->ring) == 0 && _transport->sentFrames == _transport->playedFrames)
		{
			__atomic_store_n(&_transport->drainedSerial, _drainSerial, __ATOMIC_RELEASE);
		}
	}
	free(_packet);
	if (transport_live(_transport) != 0)
	{
		__atomic_store_n(&_transport->failed, 1, __ATOMIC_RELEASE);
		__atomic_fetch_add(&_transport->xruns, 1, __ATOMIC_RELAXED);
		if (_transport->event != NULL)
		{
			_transport->event(_transport->context);
		}
	}
	__atomic_store_n(&_transport->live, 0, __ATOMIC_RELEASE);
	return NULL;
}

int32_t transport_start(transport_t *__transport, control_t *__control, const cJSON *__device, uint64_t __generation, transport_event_t __event, void *__context)
{
	memset(__transport, 0, sizeof(*__transport));
	__transport->socket = -1;
	__transport->wake = -1;
	__transport->control = __control;
	__transport->event = __event;
	__transport->context = __context;
	__transport->capture = strcmp(protocol_string(__device, "direction"), "input") == 0;
	__transport->rate = (uint32_t)protocol_number(__device, "rate", 48000);
	__transport->channels = (uint32_t)protocol_number(__device, "channels", 2);
	__transport->sampleBytes = 4;
	if (strcmp(protocol_string(__device, "format"), "s16le") == 0)
	{
		__transport->sampleBytes = 2;
	}
	if (__transport->rate < 8000 || __transport->rate > 96000 || __transport->channels < 1 || __transport->channels > 2)
	{
		return -1;
	}
	cJSON *_request = cJSON_CreateObject();
	protocol_set_string(_request, "op", "open");
	protocol_set_string(_request, "endpoint", protocol_string(__device, "key"));
	protocol_set_number(_request, "generation", __generation);
	cJSON *_reply = control_request(__control, _request);
	cJSON_Delete(_request);
	if (!protocol_boolean(_reply, "ok"))
	{
		cJSON_Delete(_reply);
		return -1;
	}
	__transport->stream = protocol_number(_reply, "stream", 0);
	__transport->epoch = protocol_number(_reply, "epoch", 0);
	__transport->socket = control_attach(__control, __transport->stream, protocol_string(_reply, "token"));
	cJSON_Delete(_reply);
	if (__transport->socket < 0)
	{
		return -1;
	}
	__transport->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (__transport->wake < 0 || ring_init(&__transport->ring, (size_t)__transport->rate * __transport->channels * __transport->sampleBytes * 80 / 1000) != 0)
	{
		return -1;
	}
	__atomic_store_n(&__transport->live, 1, __ATOMIC_RELEASE);
	if (pthread_create(&__transport->thread, NULL, _transport_worker, __transport) != 0)
	{
		__atomic_store_n(&__transport->live, 0, __ATOMIC_RELEASE);
		return -1;
	}
	__transport->threadStarted = 1;
	return 0;
}

int32_t transport_drain(transport_t *__transport)
{
	/* Only normal idle drains; call/unplug cancellation must discard its epoch. */
	uint64_t _deadline = protocol_now() + UINT64_C(500000000);
	uint32_t _serial = __atomic_add_fetch(&__transport->drainSerial, 1, __ATOMIC_ACQ_REL);
	transport_wake(__transport);
	while (transport_live(__transport) != 0 && protocol_now() < _deadline)
	{
		if (__atomic_load_n(&__transport->drainedSerial, __ATOMIC_ACQUIRE) == _serial)
		{
			return 0;
		}
		struct timespec _interval = {0, 1000000};
		nanosleep(&_interval, NULL);
	}
	return -1;
}

void transport_stop(transport_t *__transport)
{
	__atomic_store_n(&__transport->live, 0, __ATOMIC_RELEASE);
	if (__transport->socket >= 0)
	{
		shutdown(__transport->socket, SHUT_RDWR);
	}
	transport_wake(__transport);
	if (__transport->threadStarted != 0)
	{
		pthread_join(__transport->thread, NULL);
	}
	if (__transport->socket >= 0)
	{
		close(__transport->socket);
	}
	if (__transport->wake >= 0)
	{
		close(__transport->wake);
	}
	if (__transport->stream != 0 && control_live(__transport->control) != 0)
	{
		cJSON *_request = cJSON_CreateObject();
		protocol_set_string(_request, "op", "close");
		protocol_set_number(_request, "stream", __transport->stream);
		cJSON *_reply = control_request(__transport->control, _request);
		cJSON_Delete(_request);
		cJSON_Delete(_reply);
	}
	ring_destroy(&__transport->ring);
	__transport->socket = -1;
	__transport->wake = -1;
	__transport->stream = 0;
	__transport->threadStarted = 0;
}
