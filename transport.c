#define _GNU_SOURCE
#include "include/transport.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
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
	if (__transport->sharedPcm != 0 && __transport->shared.header != NULL)
	{
		int32_t _event = __transport->shared.producerEvent;
		if (__transport->capture != 0)
		{
			_event = __transport->shared.consumerEvent;
		}
		shared_notify(_event);
	}
	uint64_t _value = 1;
	if (__transport->wake >= 0)
	{
		ssize_t _written = write(__transport->wake, &_value, sizeof(_value));
		(void)_written;
	}
}

void transport_fail(transport_t *__transport)
{
	transport_fail_reason(__transport, 1);
}

void transport_fail_reason(transport_t *__transport, int32_t __reason)
{
	/* Shutdown intentionally wakes blocked socket I/O. Cancellation must not
	 * turn that wakeup into a route failure or latch a paused microphone. */
	if (transport_live(__transport) == 0)
	{
		return;
	}
	int32_t _expected = 0;
	__atomic_compare_exchange_n(&__transport->failed, &_expected, __reason, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	transport_wake(__transport);
}

void transport_cancel_capture(transport_t *__transport)
{
	/* Called on graph pause, before the manager can block on another route.
	 * Stop producing immediately; resource destruction still uses its barrier. */
	__atomic_store_n(&__transport->live, 0, __ATOMIC_RELEASE);
	if (__transport->shared.header != NULL)
	{
		__atomic_store_n(&__transport->shared.header->stopped, 1, __ATOMIC_RELEASE);
	}
	if (__transport->socket >= 0)
	{
		shutdown(__transport->socket, SHUT_RDWR);
	}
	transport_wake(__transport);
}

static void *_transport_worker(void *__context)
{
	transport_t *_transport = __context;
	protocol_t *_packet = calloc(1, sizeof(*_packet));
	uint8_t *_transmit = malloc(AUDIO_PCM_HEADER + AUDIO_PCM_MAX);
	size_t _transmitLength = 0;
	size_t _transmitOffset = 0;
	uint32_t _transmitFrames = 0;
	uint64_t _transmitDeadline = 0;
	uint64_t _nextFrame = 0;
	uint32_t _frameBytes = _transport->channels * _transport->sampleBytes;
	size_t _packetBytes = (size_t)(_transport->rate / 100) * _frameBytes;
	if (_packet == NULL || _transmit == NULL)
	{
		transport_fail(_transport);
	}
	while (_packet != NULL && _transmit != NULL && transport_live(_transport) != 0 && __atomic_load_n(&_transport->failed, __ATOMIC_ACQUIRE) == 0)
	{
		if (_transport->capture == 0 && _transmitLength == 0)
		{
			uint64_t _credited = _transport->playedFrames;
			if (_transport->writeCredit != 0)
			{
				_credited = _transport->acceptedFrames;
			}
			if (ring_available(&_transport->ring) >= _frameBytes && _transport->sentFrames - _credited < _transport->rate / 50)
			{
				size_t _length = ring_available(&_transport->ring);
				if (_length > _packetBytes)
				{
					_length = _packetBytes;
				}
				size_t _creditBytes = (size_t)(_transport->rate / 50 - (_transport->sentFrames - _credited)) * _frameBytes;
				if (_length > _creditBytes)
				{
					_length = _creditBytes;
				}
				_length -= _length % _frameBytes;
				_packet->kind = AUDIO_PCM_DATA;
				_packet->epoch = _transport->epoch;
				_packet->frame = _transport->sentFrames;
				_packet->timestamp = protocol_now();
				_packet->frames = (uint32_t)(_length / _frameBytes);
				_packet->length = _length;
				if (protocol_encode_pcm_header(_transmit, _packet) != 0 || ring_read(&_transport->ring, _transmit + AUDIO_PCM_HEADER, _length) != _length)
				{
					transport_fail_reason(_transport, 13);
					break;
				}
				_transmitFrames = _packet->frames;
				_transmitLength = AUDIO_PCM_HEADER + _length;
				_transmitOffset = 0;
				_transmitDeadline = protocol_now() + UINT64_C(1000000000);
			}
		}
		struct pollfd _poll[2] = {{_transport->socket, POLLIN, 0}, {_transport->wake, POLLIN, 0}};
		int32_t _timeout = -1;
		if (_transmitLength != 0)
		{
			/* A full send buffer must never prevent reading Android's credits
			 * and clock messages: both directions can otherwise deadlock. */
			uint64_t _now = protocol_now();
			if (_now >= _transmitDeadline)
			{
				transport_fail_reason(_transport, 13);
				break;
			}
			_poll[0].events |= POLLOUT;
			_timeout = (int32_t)((_transmitDeadline - _now + UINT64_C(999999)) / UINT64_C(1000000));
		}
		int32_t _result = poll(_poll, 2, _timeout);
		if (transport_live(_transport) == 0)
		{
			break;
		}
		if (_result == 0)
		{
			transport_fail_reason(_transport, 13);
			break;
		}
		if (_result < 0 && errno == EINTR)
		{
			continue;
		}
		if (_result < 0 || (_poll[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
		{
			transport_fail_reason(_transport, 11);
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
				transport_fail_reason(_transport, 12);
				break;
			}
			__atomic_store_n(&_transport->receivedKind, _packet->kind, __ATOMIC_RELAXED);
			__atomic_store_n(&_transport->receivedFrame, _packet->frame, __ATOMIC_RELAXED);
			if (_transport->capture != 0)
			{
				if (_packet->kind != AUDIO_PCM_DATA || _packet->frame != _nextFrame || _packet->frames == 0 || _packet->length != (size_t)_packet->frames * _frameBytes)
				{
					transport_fail_reason(_transport, 5);
					break;
				}
				if (ring_write(&_transport->ring, _packet->data, _packet->length) != _packet->length)
				{
					transport_fail_reason(_transport, 6);
					break;
				}
				_nextFrame += _packet->frames;
				if (ring_available(&_transport->ring) >= (size_t)_transport->prefillFrames * _frameBytes)
				{
					__atomic_store_n(&_transport->ready, 1, __ATOMIC_RELEASE);
				}
			}
			else
			{
				if (_packet->length != 0 || _packet->frames != 0 || _packet->frame > _transport->sentFrames)
				{
					transport_fail_reason(_transport, 7);
					break;
				}
				if (_packet->kind == AUDIO_PCM_CREDIT && _transport->writeCredit != 0 && _packet->frame >= _transport->acceptedFrames)
				{
					__atomic_store_n(&_transport->acceptedFrames, _packet->frame, __ATOMIC_RELEASE);
				}
				else if (_packet->kind == AUDIO_PCM_PRESENTATION && _transport->presentationClock != 0)
				{
					/* Invalid/stale metadata must not change credits or drain state. */
					uint64_t _now = protocol_now();
					if (_packet->timestamp != 0 && _packet->timestamp >= _transport->presentationTime && _packet->frame >= _transport->presentationFrame && _packet->timestamp <= _now + UINT64_C(2000000000))
					{
						__atomic_fetch_add(&_transport->presentationSerial, 1, __ATOMIC_ACQ_REL);
						__atomic_store_n(&_transport->presentationFrame, _packet->frame, __ATOMIC_RELAXED);
						__atomic_store_n(&_transport->presentationTime, _packet->timestamp, __ATOMIC_RELAXED);
						__atomic_fetch_add(&_transport->presentationSerial, 1, __ATOMIC_RELEASE);
					}
				}
				else if (_packet->kind == AUDIO_PCM_CLOCK && _packet->frame >= _transport->playedFrames)
				{
					__atomic_store_n(&_transport->playedFrames, _packet->frame, __ATOMIC_RELEASE);
				}
				else
				{
					transport_fail_reason(_transport, 8);
					break;
				}
			}
			if (_packet->kind == AUDIO_PCM_CLOCK || _packet->kind == AUDIO_PCM_DATA)
			{
				__atomic_fetch_add(&_transport->clockSerial, 1, __ATOMIC_ACQ_REL);
				__atomic_store_n(&_transport->clockFrame, _packet->frame, __ATOMIC_RELAXED);
				__atomic_store_n(&_transport->clockTime, _packet->timestamp, __ATOMIC_RELAXED);
				__atomic_fetch_add(&_transport->clockSerial, 1, __ATOMIC_RELEASE);
			}
			if (_transport->event != NULL)
			{
				_transport->event(_transport->context);
			}
		}
		if ((_poll[0].revents & POLLOUT) != 0 && _transmitLength != 0)
		{
			ssize_t _written = send(_transport->socket, _transmit + _transmitOffset, _transmitLength - _transmitOffset, MSG_DONTWAIT | MSG_NOSIGNAL);
			if (_written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			{
				transport_fail_reason(_transport, 13);
				break;
			}
			if (_written > 0)
			{
				_transmitOffset += (size_t)_written;
				_transmitDeadline = protocol_now() + UINT64_C(1000000000);
				if (_transmitOffset == _transmitLength)
				{
					__atomic_fetch_add(&_transport->sentFrames, _transmitFrames, __ATOMIC_RELEASE);
					_transmitLength = 0;
				}
			}
		}
		if (_drainSerial != 0 && __atomic_load_n(&_transport->drainSerial, __ATOMIC_ACQUIRE) == _drainSerial && _transmitLength == 0 && ring_available(&_transport->ring) == 0 && _transport->sentFrames == _transport->playedFrames)
		{
			__atomic_store_n(&_transport->drainedSerial, _drainSerial, __ATOMIC_RELEASE);
		}
	}
	free(_transmit);
	free(_packet);
	if (transport_live(_transport) != 0)
	{
		int32_t _unfailed = 0;
		__atomic_compare_exchange_n(&_transport->failed, &_unfailed, 1, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
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
	shared_init(&__transport->shared);
	__transport->control = __control;
	__transport->event = __event;
	__transport->context = __context;
	__transport->capture = strcmp(protocol_string(__device, "direction"), "input") == 0;
	__transport->rate = (uint32_t)protocol_number(__device, "rate", 48000);
	__transport->prefillFrames = __transport->rate / 50;
	if (strcmp(protocol_string(__device, "profile"), "headset") == 0)
	{
		/* SCO input arrives in 20 ms HAL bursts; retain two periods across jitter. */
		__transport->prefillFrames = __transport->rate / 25;
	}
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
	protocol_set_boolean(_request, "write_credit", 1);
	protocol_set_boolean(_request, "presentation_clock", 1);
	uint8_t _shared = protocol_boolean(__device, "shared_pcm");
	const char *_sharedOption = getenv("LINUX_AUDIO_SHARED_PCM");
	if (_sharedOption != NULL && strcmp(_sharedOption, "0") == 0)
	{
		_shared = 0;
	}
	protocol_set_boolean(_request, "shared_pcm", _shared);
	protocol_set_string(_request, "endpoint", protocol_string(__device, "key"));
	protocol_set_number(_request, "generation", __generation);
	uint64_t _openStart = protocol_now();
	cJSON *_reply = control_request(__control, _request);
	cJSON_Delete(_request);
	if (!protocol_boolean(_reply, "ok"))
	{
		snprintf(__transport->error, sizeof(__transport->error), "%s", protocol_string(_reply, "error"));
		cJSON_Delete(_reply);
		return -1;
	}
	__transport->stream = protocol_number(_reply, "stream", 0);
	__transport->writeCredit = protocol_boolean(_reply, "write_credit");
	__transport->sharedPcm = _shared && protocol_boolean(_reply, "shared_pcm");
	__transport->presentationClock = protocol_boolean(_reply, "presentation_clock");
	fprintf(stderr, "PCM open %s: stream=%llu rate=%u channels=%u sample_bytes=%u write_credit=%u open_ms=%.3f\n", protocol_string(__device, "key"), (unsigned long long)__transport->stream, __transport->rate, __transport->channels, __transport->sampleBytes, __transport->writeCredit, (double)(protocol_now() - _openStart) / 1000000.0);
	__transport->epoch = protocol_number(_reply, "epoch", 0);
	__transport->socket = control_attach(__control, __transport->stream, protocol_string(_reply, "token"));
	cJSON_Delete(_reply);
	if (__transport->socket < 0)
	{
		return -1;
	}
	__transport->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	size_t _capacityFrames = (size_t)__transport->rate * 80 / 1000;
	if (__transport->capture != 0)
	{
		/* Reserve prefill separately from the bounded 80 ms delivery-burst budget. */
		_capacityFrames += __transport->prefillFrames;
	}
	size_t _capacity = _capacityFrames * __transport->channels * __transport->sampleBytes;
	if (__transport->wake < 0)
	{
		return -1;
	}
	if (__transport->sharedPcm != 0)
	{
		if (shared_create(&__transport->shared, __transport->epoch, __transport->rate, __transport->channels, __transport->sampleBytes, __transport->capture, _capacity) != 0 || shared_send(&__transport->shared, __transport->socket) != 0)
		{
			return -1;
		}
		__transport->ring = __transport->shared.ring;
		fprintf(stderr, "PCM shared memory: epoch=%llu capacity=%zu bytes\n", (unsigned long long)__transport->epoch, _capacity);
	}
	else if (ring_init(&__transport->ring, _capacity) != 0)
	{
		return -1;
	}
	__atomic_store_n(&__transport->live, 1, __ATOMIC_RELEASE);
	void *(*_worker)(void *) = _transport_worker;
	if (__transport->sharedPcm != 0)
	{
		_worker = transport_shared_worker;
	}
	if (pthread_create(&__transport->thread, NULL, _worker, __transport) != 0)
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
	shared_close(&__transport->shared);
	__transport->socket = -1;
	__transport->wake = -1;
	__transport->stream = 0;
	__transport->threadStarted = 0;
}
