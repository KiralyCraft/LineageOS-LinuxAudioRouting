#define _GNU_SOURCE
#include "include/transport.h"
#include "include/latency.h"
#include "include/preferences.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/param/latency-utils.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PIPEWIRE_ENDPOINTS 64
/* Compatibility path only; event-driven output does not buffer cold-open time. */
#define PIPEWIRE_BACKLOG_MS 1500
typedef struct pipewire pipewire_t;
typedef struct
{
	pipewire_t *owner;
	char key[100];
	char label[256];
	char failure[256];
	cJSON *device;
	struct pw_stream *stream;
	struct spa_hook listener;
	struct spa_source *driveEvent;
	struct spa_source *graphWatchdog;
	uint8_t clockDriven;
	uint8_t inFlight;
	uint8_t timing;
	uint64_t reportTime;
	uint64_t latencyUpdated;
	uint8_t latencyValid;
	uint64_t producedFrames;
	uint32_t requestFrames;
	transport_t transport;
	int32_t wanted;
	uint64_t activationSerial;
	uint64_t observedActivation;
	int32_t processing;
	int32_t accepting;
	ring_t startup;
	int32_t startupFailed;
	uint8_t present;
	uint8_t opened;
	uint8_t mmapPreferred;
	uint8_t openedMmapPreferred;
	uint8_t capture;
	uint8_t previouslyAvailable;
	uint8_t previouslyBusy;
	uint8_t failed;
	uint32_t frameBytes;
	uint32_t captureNeed;
	uint32_t captureHave;
	uint64_t captureUnderruns;
	uint64_t reportedUnderruns;
	uint64_t failedGeneration;
	uint64_t clockFrame;
	uint64_t clockTime;
	double rateFactor;
} pipewire_endpoint_t;

struct pipewire
{
	control_t control;
	preferences_t preferences;
	struct pw_thread_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	pthread_mutex_t mutex;
	pthread_cond_t changed;
	cJSON *inventory;
	cJSON *routes;
	uint8_t previouslySuspended;
	uint8_t pending;
	pipewire_endpoint_t *endpoints[PIPEWIRE_ENDPOINTS];
	size_t count;
};

static volatile sig_atomic_t _pipewire_quit;

static void _pipewire_signal(int __signal)
{
	(void)__signal;
	_pipewire_quit = 1;
}

static void _pipewire_notify(void *__context)
{
	pipewire_t *_owner = __context;
	pthread_mutex_lock(&_owner->mutex);
	_owner->pending = 1;
	pthread_cond_signal(&_owner->changed);
	pthread_mutex_unlock(&_owner->mutex);
}

static void _pipewire_event(void *__context, const cJSON *__message)
{
	pipewire_t *_owner = __context;
	pthread_mutex_lock(&_owner->mutex);
	if (strcmp(protocol_string(__message, "op"), "inventory") == 0)
	{
		cJSON_Delete(_owner->inventory);
		_owner->inventory = cJSON_Duplicate(__message, 1);
	}
	else if (strcmp(protocol_string(__message, "op"), "route") == 0)
	{
		const char *_key = protocol_string(__message, "endpoint");
		cJSON_DeleteItemFromObjectCaseSensitive(_owner->routes, _key);
		cJSON_AddItemToObject(_owner->routes, _key, cJSON_Duplicate(__message, 1));
		for (size_t _index = 0; _index < _owner->count; ++_index)
		{
			pipewire_endpoint_t *_endpoint = _owner->endpoints[_index];
			if (strcmp(_endpoint->key, _key) == 0 && _endpoint->driveEvent != NULL)
			{
				pw_loop_signal_event(pw_thread_loop_get_loop(_owner->loop), _endpoint->driveEvent);
			}
		}
		fprintf(stderr, "route verified stream=%llu android_device=%llu\n", (unsigned long long)protocol_number(__message, "stream", 0), (unsigned long long)protocol_number(__message, "actual_android_id", 0));
	}
	else if (strcmp(protocol_string(__message, "op"), "stream_closed") == 0 && strlen(protocol_string(__message, "error")) != 0)
	{
		fprintf(stderr, "Android stream stopped: %s\n", protocol_string(__message, "error"));
	}
	_owner->pending = 1;
	pthread_cond_signal(&_owner->changed);
	pthread_mutex_unlock(&_owner->mutex);
}

static void _pipewire_latency(pipewire_endpoint_t *__endpoint)
{
	if (__endpoint->capture != 0 || __atomic_load_n(&__endpoint->processing, __ATOMIC_ACQUIRE) == 0)
	{
		return;
	}
	uint64_t _now = protocol_now();
	if (_now - __endpoint->latencyUpdated < UINT64_C(250000000))
	{
		return;
	}
	transport_t *_transport = &__endpoint->transport;
	uint32_t _before = __atomic_load_n(&_transport->presentationSerial, __ATOMIC_ACQUIRE);
	latency_t _point = {__atomic_load_n(&_transport->presentationFrame, __ATOMIC_RELAXED), __atomic_load_n(&_transport->presentationTime, __ATOMIC_RELAXED)};
	uint32_t _after = __atomic_load_n(&_transport->presentationSerial, __ATOMIC_ACQUIRE);
	if (_before != _after || (_before & 1) != 0)
	{
		return;
	}
	uint64_t _produced = __atomic_load_n(&__endpoint->producedFrames, __ATOMIC_ACQUIRE);
	uint64_t _delay = 0;
	uint8_t _valid = latency_estimate(&_point, _produced, _transport->rate, _now, &_delay);
	__endpoint->latencyUpdated = _now;
	if (_valid != 0)
	{
		uint8_t _storage[256];
		struct spa_pod_builder _builder = SPA_POD_BUILDER_INIT(_storage, sizeof(_storage));
		struct spa_latency_info _info = {.direction = SPA_DIRECTION_INPUT, .min_ns = _delay, .max_ns = _delay};
		const struct spa_pod *_parameter = spa_latency_build(&_builder, SPA_PARAM_Latency, &_info);
		pw_stream_update_params(__endpoint->stream, &_parameter, 1);
		if (__endpoint->timing != 0)
		{
			fprintf(stderr, "PCM presentation %s epoch=%llu delay_ms=%.3f frame=%llu timestamp_ns=%llu\n", __endpoint->key, (unsigned long long)_transport->epoch, (double)_delay / 1000000.0, (unsigned long long)_point.frame, (unsigned long long)_point.time);
		}
	}
	if (_valid != __endpoint->latencyValid)
	{
		const char *_value = "false";
		if (_valid != 0)
		{
			_value = "true";
		}
		struct spa_dict_item _item = SPA_DICT_ITEM_INIT("linux.audio.latency.valid", _value);
		struct spa_dict _properties = SPA_DICT_INIT(&_item, 1);
		pw_stream_update_properties(__endpoint->stream, &_properties);
		__endpoint->latencyValid = _valid;
	}
}

static void _pipewire_watchdog(void *__context, uint64_t __expirations)
{
	(void)__expirations;
	pipewire_endpoint_t *_endpoint = __context;
	if (_endpoint->inFlight != 0)
	{
		/* A graph iteration may fail to emit trigger_done. This one-shot
		 * recovery deadline is not the audio clock or steady-state pacer. */
		_endpoint->inFlight = 0;
		pw_loop_signal_event(pw_thread_loop_get_loop(_endpoint->owner->loop), _endpoint->driveEvent);
	}
}

static void _pipewire_trigger(pipewire_endpoint_t *__endpoint)
{
	uint32_t _frames = __atomic_load_n(&__endpoint->requestFrames, __ATOMIC_ACQUIRE);
	if (_frames == 0)
	{
		_frames = __endpoint->transport.rate / 100;
	}
	uint64_t _duration = (uint64_t)_frames * UINT64_C(3000000000) / __endpoint->transport.rate;
	struct timespec _deadline = {(time_t)(_duration / UINT64_C(1000000000)), (long)(_duration % UINT64_C(1000000000))};
	__endpoint->inFlight = 1;
	pw_loop_update_timer(pw_thread_loop_get_loop(__endpoint->owner->loop), __endpoint->graphWatchdog, &_deadline, NULL, false);
	if (pw_stream_trigger_process(__endpoint->stream) < 0)
	{
		__endpoint->inFlight = 0;
		transport_fail_reason(&__endpoint->transport, 14);
	}
}

static void _pipewire_drive(void *__context, uint64_t __count)
{
	(void)__count;
	pipewire_endpoint_t *_endpoint = __context;
	_pipewire_latency(_endpoint);
	if (_endpoint->clockDriven == 0 || __atomic_load_n(&_endpoint->processing, __ATOMIC_ACQUIRE) == 0 || _endpoint->inFlight != 0 || pw_stream_get_state(_endpoint->stream, NULL) != PW_STREAM_STATE_STREAMING)
	{
		return;
	}
	pthread_mutex_lock(&_endpoint->owner->mutex);
	const cJSON *_route = cJSON_GetObjectItemCaseSensitive(_endpoint->owner->routes, _endpoint->key);
	uint8_t _ready = protocol_boolean(_route, "verified") && protocol_number(_route, "epoch", 0) == _endpoint->transport.epoch;
	pthread_mutex_unlock(&_endpoint->owner->mutex);
	if (_ready == 0 || !pw_stream_is_driving(_endpoint->stream))
	{
		return;
	}
	if (_endpoint->capture != 0)
	{
		uint32_t _requested = __atomic_load_n(&_endpoint->requestFrames, __ATOMIC_ACQUIRE);
		if (_requested == 0)
		{
			_requested = _endpoint->transport.rate / 50;
		}
		if (__atomic_load_n(&_endpoint->transport.ready, __ATOMIC_ACQUIRE) != 0 && ring_available(&_endpoint->transport.ring) >= (size_t)_requested * _endpoint->frameBytes)
		{
			_pipewire_trigger(_endpoint);
		}
		return;
	}
	uint64_t _produced = __atomic_load_n(&_endpoint->producedFrames, __ATOMIC_ACQUIRE);
	uint64_t _accepted = __atomic_load_n(&_endpoint->transport.acceptedFrames, __ATOMIC_ACQUIRE);
	if (_endpoint->transport.writeCredit == 0)
	{
		_accepted = __atomic_load_n(&_endpoint->transport.playedFrames, __ATOMIC_ACQUIRE);
	}
	if (_produced >= _accepted && _produced - _accepted < _endpoint->transport.rate / 50)
	{
		_pipewire_trigger(_endpoint);
	}
}

static void _pipewire_trigger_done(void *__context)
{
	pipewire_endpoint_t *_endpoint = __context;
	pw_loop_update_timer(pw_thread_loop_get_loop(_endpoint->owner->loop), _endpoint->graphWatchdog, NULL, NULL, false);
	_endpoint->inFlight = 0;
	if (_endpoint->driveEvent != NULL)
	{
		pw_loop_signal_event(pw_thread_loop_get_loop(_endpoint->owner->loop), _endpoint->driveEvent);
	}
}

static void _pipewire_transport_event(void *__context)
{
	pipewire_endpoint_t *_endpoint = __context;
	uint64_t _now = protocol_now();
	if (_endpoint->timing != 0 && _endpoint->capture == 0 && _now - _endpoint->reportTime >= UINT64_C(1000000000))
	{
		_endpoint->reportTime = _now;
		uint64_t _produced = __atomic_load_n(&_endpoint->producedFrames, __ATOMIC_ACQUIRE);
		uint64_t _accepted = __atomic_load_n(&_endpoint->transport.acceptedFrames, __ATOMIC_ACQUIRE);
		uint64_t _played = __atomic_load_n(&_endpoint->transport.playedFrames, __ATOMIC_ACQUIRE);
		fprintf(stderr, "PCM timing %s epoch=%llu rate=%u produced=%llu accepted=%llu played=%llu startup_frames=%zu transport_frames=%zu monotonic_ns=%llu\n", _endpoint->key, (unsigned long long)_endpoint->transport.epoch, _endpoint->transport.rate, (unsigned long long)_produced, (unsigned long long)_accepted, (unsigned long long)_played, ring_available(&_endpoint->startup) / _endpoint->frameBytes, ring_available(&_endpoint->transport.ring) / _endpoint->frameBytes, (unsigned long long)_now);
	}
	if (_endpoint->driveEvent != NULL)
	{
		pw_loop_signal_event(pw_thread_loop_get_loop(_endpoint->owner->loop), _endpoint->driveEvent);
	}
	if (_endpoint->clockDriven == 0 || __atomic_load_n(&_endpoint->transport.failed, __ATOMIC_ACQUIRE) != 0)
	{
		_pipewire_notify(_endpoint->owner);
	}
}

static void _pipewire_state(void *__context, enum pw_stream_state __old, enum pw_stream_state __state, const char *__error)
{
	pipewire_endpoint_t *_endpoint = __context;
	fprintf(stderr, "Endpoint state %s: %s -> %s\n", _endpoint->key, pw_stream_state_as_string(__old), pw_stream_state_as_string(__state));
	if (__state != PW_STREAM_STATE_STREAMING)
	{
		if (_endpoint->capture != 0 && __atomic_load_n(&_endpoint->processing, __ATOMIC_ACQUIRE) != 0)
		{
			transport_cancel_capture(&_endpoint->transport);
		}
		_endpoint->inFlight = 0;
	}
	__atomic_store_n(&_endpoint->wanted, __state == PW_STREAM_STATE_STREAMING, __ATOMIC_RELEASE);
	if (__state == PW_STREAM_STATE_STREAMING && __old != PW_STREAM_STATE_STREAMING)
	{
		__atomic_fetch_add(&_endpoint->activationSerial, 1, __ATOMIC_RELEASE);
	}
	if (__state == PW_STREAM_STATE_ERROR)
	{
		fprintf(stderr, "PipeWire endpoint %s: %s\n", _endpoint->key, __error);
	}
	_pipewire_notify(_endpoint->owner);
}

static void _pipewire_pump(pipewire_endpoint_t *__endpoint)
{
	uint8_t _bytes[2048];
	while (ring_available(&__endpoint->startup) != 0)
	{
		size_t _free = __endpoint->transport.ring.capacity - ring_available(&__endpoint->transport.ring);
		if (_free < __endpoint->frameBytes)
		{
			break;
		}
		size_t _length = sizeof(_bytes);
		if (_length > _free)
		{
			_length = _free;
		}
		_length -= _length % __endpoint->frameBytes;
		_length = ring_read(&__endpoint->startup, _bytes, _length);
		if (ring_write(&__endpoint->transport.ring, _bytes, _length) != _length)
		{
			transport_fail_reason(&__endpoint->transport, 9);
			break;
		}
	}
	transport_wake(&__endpoint->transport);
}

static void _pipewire_buffer(pipewire_endpoint_t *__endpoint, struct pw_buffer *__buffer)
{
	pipewire_endpoint_t *_endpoint = __endpoint;
	struct pw_buffer *_buffer = __buffer;
	struct spa_buffer *_spa = _buffer->buffer;
	if (_spa->n_datas == 0 || _spa->datas[0].data == NULL || _spa->datas[0].chunk == NULL)
	{
		pw_stream_queue_buffer(_endpoint->stream, _buffer);
		return;
	}
	struct spa_data *_data = &_spa->datas[0];
	uint8_t _processing = __atomic_load_n(&_endpoint->processing, __ATOMIC_ACQUIRE) != 0;
	if (_endpoint->capture != 0)
	{
		size_t _length = 0;
		if (_processing != 0 && __atomic_load_n(&_endpoint->transport.ready, __ATOMIC_ACQUIRE) != 0)
		{
			size_t _requested = _data->maxsize;
			if (_buffer->requested != 0 && _buffer->requested * _endpoint->frameBytes < _requested)
			{
				_requested = (size_t)_buffer->requested * _endpoint->frameBytes;
			}
			_requested -= _requested % _endpoint->frameBytes;
			__atomic_store_n(&_endpoint->requestFrames, (uint32_t)(_requested / _endpoint->frameBytes), __ATOMIC_RELEASE);
			size_t _available = ring_available(&_endpoint->transport.ring);
			if (_available >= _requested)
			{
				_length = ring_read(&_endpoint->transport.ring, _data->data, _requested);
				transport_wake(&_endpoint->transport);
			}
			else
			{
				/* A graph deadline precedes data arrival, not a PCM discontinuity.
				 * Retain all captured bytes and re-prime the existing recorder.
				 * Empty output reports no data; the transport invents no samples. */
				__atomic_store_n(&_endpoint->captureNeed, (uint32_t)(_requested / _endpoint->frameBytes), __ATOMIC_RELAXED);
				__atomic_store_n(&_endpoint->captureHave, (uint32_t)(_available / _endpoint->frameBytes), __ATOMIC_RELAXED);
				__atomic_fetch_add(&_endpoint->captureUnderruns, 1, __ATOMIC_RELAXED);
				__atomic_store_n(&_endpoint->transport.ready, 0, __ATOMIC_RELEASE);
			}
		}
		_data->chunk->offset = 0;
		_data->chunk->size = (uint32_t)_length;
		_data->chunk->stride = (int32_t)_endpoint->frameBytes;
		_buffer->size = _length / _endpoint->frameBytes;
	}
	else if (_processing != 0 || __atomic_load_n(&_endpoint->accepting, __ATOMIC_ACQUIRE) != 0)
	{
		uint32_t _offset = _data->chunk->offset;
		uint32_t _length = _data->chunk->size;
		__atomic_store_n(&_endpoint->requestFrames, _length / _endpoint->frameBytes, __ATOMIC_RELEASE);
		__atomic_fetch_add(&_endpoint->producedFrames, _length / _endpoint->frameBytes, __ATOMIC_RELEASE);
		ring_t *_ring = &_endpoint->startup;
		/* Retain a bounded backlog when Android temporarily stops accepting
		 * writes (notably Bluetooth startup). The transport queue alone is
		 * not the endpoint capacity; preserve FIFO order through startup. */
		if (_processing != 0 && ring_available(&_endpoint->startup) == 0 && _length <= _endpoint->transport.ring.capacity - ring_available(&_endpoint->transport.ring))
		{
			_ring = &_endpoint->transport.ring;
		}
		if (_offset > _data->maxsize || _length > _data->maxsize - _offset || _length % _endpoint->frameBytes != 0)
		{
			__atomic_store_n(&_endpoint->startupFailed, 1, __ATOMIC_RELEASE);
		}
		else if (ring_write(_ring, (uint8_t *)_data->data + _offset, _length) != _length)
		{
			if (_processing != 0)
			{
				transport_fail_reason(&_endpoint->transport, 9);
			}
			else
			{
				__atomic_store_n(&_endpoint->startupFailed, 1, __ATOMIC_RELEASE);
			}
		}
		if (_processing != 0)
		{
			_pipewire_pump(_endpoint);
		}
	}
	pw_stream_queue_buffer(_endpoint->stream, _buffer);
}

static void _pipewire_process(void *__context)
{
	pipewire_endpoint_t *_endpoint = __context;
	struct pw_buffer *_buffer = pw_stream_dequeue_buffer(_endpoint->stream);
	if (_buffer == NULL)
	{
		return;
	}
	_pipewire_buffer(_endpoint, _buffer);
}

static void _pipewire_format(void *__context, uint32_t __id, const struct spa_pod *__parameter)
{
	pipewire_endpoint_t *_endpoint = __context;
	if (__id != SPA_PARAM_Format || __parameter == NULL)
	{
		return;
	}
	struct spa_audio_info_raw _audio = {0};
	if (spa_format_audio_raw_parse(__parameter, &_audio) == 0)
	{
		fprintf(stderr, "PCM graph format %s: format=%u rate=%u channels=%u\n", _endpoint->key, _audio.format, _audio.rate, _audio.channels);
	}
	/* Bound graph buffers; the preallocated startup FIFO retains cold-open audio. */
	uint8_t _storage[256];
	struct spa_pod_builder _builder = SPA_POD_BUILDER_INIT(_storage, sizeof(_storage));
	const struct spa_pod *_parameter = spa_pod_builder_add_object(&_builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16), SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size, SPA_POD_CHOICE_RANGE_Int(256 * _endpoint->frameBytes, _endpoint->frameBytes, 512 * _endpoint->frameBytes), SPA_PARAM_BUFFERS_stride, SPA_POD_Int(_endpoint->frameBytes));
	pw_stream_update_params(_endpoint->stream, &_parameter, 1);
}

static const struct pw_stream_events _pipewire_events = {
	.version = PW_VERSION_STREAM_EVENTS,
	.state_changed = _pipewire_state,
	.process = _pipewire_process,
	.param_changed = _pipewire_format,
	.trigger_done = _pipewire_trigger_done,
};

static int32_t _pipewire_barrier(struct spa_loop *__loop, bool __async, uint32_t __sequence, const void *__data, size_t __size, void *__context)
{
	(void)__loop;
	(void)__async;
	(void)__sequence;
	(void)__data;
	(void)__size;
	pipewire_endpoint_t *_endpoint = __context;
	uint8_t _operation = *(const uint8_t *)__data;
	if (_operation == 2 && __atomic_load_n(&_endpoint->processing, __ATOMIC_ACQUIRE) != 0)
	{
		/* The final graph buffers may still be dequeuable when Pause arrives. */
		struct pw_buffer *_buffer;
		while ((_buffer = pw_stream_dequeue_buffer(_endpoint->stream)) != NULL)
		{
			_pipewire_buffer(_endpoint, _buffer);
		}
	}
	__atomic_store_n(&_endpoint->processing, _operation == 1, __ATOMIC_RELEASE);
	if (_operation == 1)
	{
		_pipewire_pump(_endpoint);
		if (_endpoint->driveEvent != NULL)
		{
			pw_loop_signal_event(pw_thread_loop_get_loop(_endpoint->owner->loop), _endpoint->driveEvent);
		}
	}
	else if (_operation == 0)
	{
		/* Explicit epoch retirement, performed by the sole RT reader/writer. */
		ring_discard(&_endpoint->startup);
		uint8_t _byte;
		ring_read(&_endpoint->startup, &_byte, 0);
	}
	return 0;
}

static void _pipewire_pending(pipewire_endpoint_t *__endpoint, uint8_t __processing)
{
	/* Quiesce main-loop fence/clock callbacks as well as the data-loop
	 * producer before resetting an epoch or destroying its transport ring. */
	pw_thread_loop_lock(__endpoint->owner->loop);
	struct pw_loop *_dataLoop = pw_stream_get_data_loop(__endpoint->stream);
	if (_dataLoop != NULL)
	{
		pw_loop_invoke(_dataLoop, _pipewire_barrier, SPA_ID_INVALID, &__processing, sizeof(__processing), true, __endpoint);
	}
	pw_thread_loop_unlock(__endpoint->owner->loop);
}

static void _pipewire_stop_endpoint(pipewire_endpoint_t *__endpoint, uint8_t __drain)
{
	if (__endpoint->opened == 0)
	{
		return;
	}
	__atomic_store_n(&__endpoint->accepting, 0, __ATOMIC_RELEASE);
	uint8_t _operation = 0;
	if (__drain != 0)
	{
		_operation = 2;
	}
	_pipewire_pending(__endpoint, _operation);
	if (__drain != 0 && __endpoint->capture == 0)
	{
		do
		{
			_pipewire_pump(__endpoint);
			if (transport_drain(&__endpoint->transport) != 0)
			{
				fprintf(stderr, "PCM drain failed: %s\n", __endpoint->key);
				break;
			}
		} while (ring_available(&__endpoint->startup) != 0);
	}
	_pipewire_pending(__endpoint, 0);
	transport_stop(&__endpoint->transport);
	__endpoint->opened = 0;
	__endpoint->clockFrame = 0;
	__endpoint->clockTime = 0;
	__endpoint->latencyUpdated = 0;
	__endpoint->latencyValid = 0;
	pw_thread_loop_lock(__endpoint->owner->loop);
	uint8_t _storage[256];
	struct spa_pod_builder _builder = SPA_POD_BUILDER_INIT(_storage, sizeof(_storage));
	struct spa_latency_info _info = {.direction = SPA_DIRECTION_INPUT};
	if (__endpoint->capture != 0)
	{
		_info.direction = SPA_DIRECTION_OUTPUT;
	}
	const struct spa_pod *_parameter = spa_latency_build(&_builder, SPA_PARAM_Latency, &_info);
	pw_stream_update_params(__endpoint->stream, &_parameter, 1);
	struct spa_dict_item _item = SPA_DICT_ITEM_INIT("linux.audio.latency.valid", "false");
	struct spa_dict _properties = SPA_DICT_INIT(&_item, 1);
	pw_stream_update_properties(__endpoint->stream, &_properties);
	pw_thread_loop_unlock(__endpoint->owner->loop);
	__atomic_store_n(&__endpoint->producedFrames, 0, __ATOMIC_RELEASE);
}

static pipewire_endpoint_t *_pipewire_create(pipewire_t *__owner, const cJSON *__device)
{
	pipewire_endpoint_t *_endpoint = calloc(1, sizeof(*_endpoint));
	if (_endpoint == NULL)
	{
		return NULL;
	}
	_endpoint->owner = __owner;
	_endpoint->rateFactor = 1.0;
	_endpoint->accepting = protocol_boolean(__device, "available");
	_endpoint->capture = strcmp(protocol_string(__device, "direction"), "input") == 0;
	_endpoint->clockDriven = protocol_boolean(__device, "clock_driver");
	const char *_clockOption = getenv("LINUX_AUDIO_CLOCK_DRIVER");
	if (_clockOption != NULL)
	{
		_endpoint->clockDriven = strcmp(_clockOption, "1") == 0;
	}
	_endpoint->timing = getenv("LINUX_AUDIO_TIMING") != NULL && strcmp(getenv("LINUX_AUDIO_TIMING"), "1") == 0;
	snprintf(_endpoint->key, sizeof(_endpoint->key), "%s", protocol_string(__device, "key"));
	snprintf(_endpoint->label, sizeof(_endpoint->label), "%s", protocol_string(__device, "name"));
	uint32_t _channels = (uint32_t)protocol_number(__device, "channels", 2);
	uint32_t _rate = (uint32_t)protocol_number(__device, "rate", 48000);
	if (_channels < 1 || _channels > 2 || _rate < 8000 || _rate > 96000)
	{
		free(_endpoint);
		return NULL;
	}
	uint32_t _format = SPA_AUDIO_FORMAT_F32_LE;
	uint32_t _sampleBytes = 4;
	if (strcmp(protocol_string(__device, "format"), "s16le") == 0)
	{
		_format = SPA_AUDIO_FORMAT_S16_LE;
		_sampleBytes = 2;
	}
	_endpoint->frameBytes = _channels * _sampleBytes;
	uint32_t _backlogMs = PIPEWIRE_BACKLOG_MS;
	if (_endpoint->clockDriven != 0)
	{
		_backlogMs = 80;
	}
	if (ring_init(&_endpoint->startup, (size_t)_rate * _endpoint->frameBytes * _backlogMs / 1000) != 0)
	{
		free(_endpoint);
		return NULL;
	}
	char _nodeName[128];
	snprintf(_nodeName, sizeof(_nodeName), "linux-audio.%s", _endpoint->key);
	const char *_mediaClass = "Audio/Sink";
	enum pw_direction _direction = PW_DIRECTION_INPUT;
	if (_endpoint->capture != 0)
	{
		_mediaClass = "Audio/Source";
		_direction = PW_DIRECTION_OUTPUT;
	}
	struct pw_properties *_properties = pw_properties_new(PW_KEY_NODE_NAME, _nodeName, PW_KEY_NODE_DESCRIPTION, _endpoint->label, PW_KEY_MEDIA_CLASS, _mediaClass, PW_KEY_NODE_VIRTUAL, "true", PW_KEY_NODE_WANT_DRIVER, "true", PW_KEY_NODE_PAUSE_ON_IDLE, "true", "linux.audio.endpoint", _endpoint->key, "linux.audio.profile", protocol_string(__device, "profile"), "node.latency", "256/48000", "linux.audio.latency.valid", "false", "resample.disable", "false", NULL);
	const char *_priority = "1000";
	if (strcmp(protocol_string(__device, "profile"), "stereo") == 0)
	{
		_priority = "2000";
	}
	else if (strcmp(protocol_string(__device, "profile"), "headset") == 0)
	{
		_priority = "500";
	}
	pw_properties_set(_properties, PW_KEY_PRIORITY_SESSION, _priority);
	if (_endpoint->clockDriven != 0)
	{
		pw_properties_set(_properties, PW_KEY_PRIORITY_DRIVER, "30000");
	}
	pw_thread_loop_lock(__owner->loop);
	_endpoint->stream = pw_stream_new(__owner->core, _endpoint->label, _properties);
	if (_endpoint->stream == NULL)
	{
		pw_thread_loop_unlock(__owner->loop);
		ring_destroy(&_endpoint->startup);
		free(_endpoint);
		return NULL;
	}
	_endpoint->driveEvent = pw_loop_add_event(pw_thread_loop_get_loop(__owner->loop), _pipewire_drive, _endpoint);
	_endpoint->graphWatchdog = pw_loop_add_timer(pw_thread_loop_get_loop(__owner->loop), _pipewire_watchdog, _endpoint);
	if (_endpoint->driveEvent == NULL || _endpoint->graphWatchdog == NULL)
	{
		if (_endpoint->driveEvent != NULL)
		{
			pw_loop_destroy_source(pw_thread_loop_get_loop(__owner->loop), _endpoint->driveEvent);
		}
		if (_endpoint->graphWatchdog != NULL)
		{
			pw_loop_destroy_source(pw_thread_loop_get_loop(__owner->loop), _endpoint->graphWatchdog);
		}
		pw_stream_destroy(_endpoint->stream);
		pw_thread_loop_unlock(__owner->loop);
		ring_destroy(&_endpoint->startup);
		free(_endpoint);
		return NULL;
	}
	pw_stream_add_listener(_endpoint->stream, &_endpoint->listener, &_pipewire_events, _endpoint);
	uint8_t _storage[1024];
	struct spa_pod_builder _builder = SPA_POD_BUILDER_INIT(_storage, sizeof(_storage));
	struct spa_audio_info_raw _audio = {0};
	_audio.format = _format;
	_audio.rate = _rate;
	_audio.channels = _channels;
	_audio.position[0] = SPA_AUDIO_CHANNEL_MONO;
	if (_channels == 2)
	{
		_audio.position[0] = SPA_AUDIO_CHANNEL_FL;
		_audio.position[1] = SPA_AUDIO_CHANNEL_FR;
	}
	const struct spa_pod *_formatPod = spa_format_audio_raw_build(&_builder, SPA_PARAM_EnumFormat, &_audio);
	enum pw_stream_flags _flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
	if (_endpoint->clockDriven != 0)
	{
		_flags |= PW_STREAM_FLAG_DRIVER;
	}
	int32_t _result = pw_stream_connect(_endpoint->stream, _direction, PW_ID_ANY, _flags, &_formatPod, 1);
	pw_thread_loop_unlock(__owner->loop);
	if (_result < 0)
	{
		pw_thread_loop_lock(__owner->loop);
		pw_stream_destroy(_endpoint->stream);
		pw_loop_destroy_source(pw_thread_loop_get_loop(__owner->loop), _endpoint->graphWatchdog);
		pw_loop_destroy_source(pw_thread_loop_get_loop(__owner->loop), _endpoint->driveEvent);
		pw_thread_loop_unlock(__owner->loop);
		ring_destroy(&_endpoint->startup);
		free(_endpoint);
		return NULL;
	}
	return _endpoint;
}

static void _pipewire_clock(pipewire_endpoint_t *__endpoint)
{
	transport_t *_transport = &__endpoint->transport;
	uint32_t _before = __atomic_load_n(&_transport->clockSerial, __ATOMIC_ACQUIRE);
	uint64_t _frame = __atomic_load_n(&_transport->clockFrame, __ATOMIC_RELAXED);
	uint64_t _time = __atomic_load_n(&_transport->clockTime, __ATOMIC_RELAXED);
	uint32_t _after = __atomic_load_n(&_transport->clockSerial, __ATOMIC_ACQUIRE);
	if (_before != _after || (_before & 1) != 0 || _time == 0 || _time <= __endpoint->clockTime || _frame <= __endpoint->clockFrame)
	{
		return;
	}
	if (__endpoint->clockTime != 0 && _time - __endpoint->clockTime >= UINT64_C(50000000))
	{
		double _measured = (double)(_frame - __endpoint->clockFrame) * 1000000000.0 / (double)(_time - __endpoint->clockTime);
		double _factor = _measured / _transport->rate;
		if (_factor > 0.995 && _factor < 1.005)
		{
			__endpoint->rateFactor = 0.95 * __endpoint->rateFactor + 0.05 * _factor;
			/* Resampling belongs to PipeWire's mixer, never to the PCM transport. */
			pw_stream_set_rate(__endpoint->stream, __endpoint->rateFactor);
		}
	}
	__endpoint->clockFrame = _frame;
	__endpoint->clockTime = _time;
}

static void _pipewire_reconcile(pipewire_t *__owner, const cJSON *__inventory)
{
	char _defaultSink[256];
	char _defaultSource[256];
	preferences_snapshot(&__owner->preferences, _defaultSink, sizeof(_defaultSink), _defaultSource, sizeof(_defaultSource));
	uint64_t _generation = protocol_number(__inventory, "generation", 0);
	uint8_t _suspended = protocol_boolean(__inventory, "suspended");
	for (size_t _index = 0; _index < __owner->count; ++_index)
	{
		__owner->endpoints[_index]->present = 0;
	}
	const cJSON *_device;
	cJSON_ArrayForEach(_device, cJSON_GetObjectItemCaseSensitive(__inventory, "devices"))
	{
		pipewire_endpoint_t *_endpoint = NULL;
		for (size_t _index = 0; _index < __owner->count; ++_index)
		{
			if (strcmp(__owner->endpoints[_index]->key, protocol_string(_device, "key")) == 0)
			{
				_endpoint = __owner->endpoints[_index];
				break;
			}
		}
		if (_endpoint == NULL && __owner->count < PIPEWIRE_ENDPOINTS)
		{
			_endpoint = _pipewire_create(__owner, _device);
			if (_endpoint != NULL)
			{
				pthread_mutex_lock(&__owner->mutex);
				__owner->endpoints[__owner->count++] = _endpoint;
				pthread_mutex_unlock(&__owner->mutex);
			}
		}
		if (_endpoint != NULL)
		{
			if (_endpoint->device != NULL && (protocol_number(_endpoint->device, "android_id", 0) != protocol_number(_device, "android_id", 0) || protocol_number(_endpoint->device, "rate", 0) != protocol_number(_device, "rate", 0) || protocol_number(_endpoint->device, "channels", 0) != protocol_number(_device, "channels", 0) || strcmp(protocol_string(_endpoint->device, "format"), protocol_string(_device, "format")) != 0))
			{
				_endpoint->failed = 0;
			}
			cJSON_Delete(_endpoint->device);
			_endpoint->device = cJSON_Duplicate(_device, 1);
			_endpoint->present = protocol_boolean(_device, "available");
		}
	}
	for (size_t _index = 0; _index < __owner->count; ++_index)
	{
		pipewire_endpoint_t *_endpoint = __owner->endpoints[_index];
		const char *_default = _defaultSink;
		if (_endpoint->capture)
		{
			_default = _defaultSource;
		}
		char _nodeName[128];
		snprintf(_nodeName, sizeof(_nodeName), "linux-audio.%s", _endpoint->key);
		uint8_t _preferred = strcmp(_default, _nodeName) == 0;
		uint8_t _mmapControl = transport_shared_requested(_endpoint->device) && protocol_boolean(_endpoint->device, "mmap_preference");
		uint8_t _policyChanged = _mmapControl && _preferred != _endpoint->mmapPreferred;
		_endpoint->mmapPreferred = _preferred;
		protocol_set_boolean(_endpoint->device, "prefer_mmap", _preferred);
		if (_policyChanged)
		{
			_endpoint->failed = 0;
			_endpoint->failure[0] = '\0';
		}
		uint8_t _reopen = _mmapControl && _endpoint->openedMmapPreferred != _preferred;
		uint64_t _activation = __atomic_load_n(&_endpoint->activationSerial, __ATOMIC_ACQUIRE);
		uint8_t _busy = strlen(protocol_string(_endpoint->device, "busy")) != 0;
		if (_activation != _endpoint->observedActivation || (_endpoint->previouslyAvailable == 0 && _endpoint->present != 0) || (_endpoint->previouslyBusy != 0 && _busy == 0) || (__owner->previouslySuspended != 0 && _suspended == 0))
		{
			_endpoint->failed = 0;
			_endpoint->failure[0] = '\0';
			_endpoint->observedActivation = _activation;
		}
		_endpoint->previouslyAvailable = _endpoint->present;
		_endpoint->previouslyBusy = _busy;
		if (__atomic_exchange_n(&_endpoint->startupFailed, 0, __ATOMIC_ACQ_REL) != 0)
		{
			_endpoint->failedGeneration = _generation;
			_endpoint->failed = 1;
			fprintf(stderr, "PCM startup queue overrun: %s queued=%zu capacity=%zu\n", _endpoint->key, ring_available(&_endpoint->startup), _endpoint->startup.capacity);
		}
		uint8_t _wanted = __atomic_load_n(&_endpoint->wanted, __ATOMIC_ACQUIRE) != 0;
		if (_endpoint->opened != 0 && (_reopen || _endpoint->present == 0 || _busy != 0 || _suspended != 0 || _wanted == 0 || _endpoint->failed != 0 || transport_live(&_endpoint->transport) == 0))
		{
			if (__atomic_load_n(&_endpoint->transport.failed, __ATOMIC_ACQUIRE) != 0)
			{
				_endpoint->failedGeneration = _generation;
				_endpoint->failed = 1;
				fprintf(stderr, "PCM stream failed: %s code=%d; received_kind=%u received_frame=%llu sent=%llu accepted=%llu played=%llu ring=%zu/%zu; capture requested=%u available=%u frames\n", _endpoint->key, __atomic_load_n(&_endpoint->transport.failed, __ATOMIC_ACQUIRE), __atomic_load_n(&_endpoint->transport.receivedKind, __ATOMIC_RELAXED), (unsigned long long)__atomic_load_n(&_endpoint->transport.receivedFrame, __ATOMIC_RELAXED), (unsigned long long)__atomic_load_n(&_endpoint->transport.sentFrames, __ATOMIC_RELAXED), (unsigned long long)__atomic_load_n(&_endpoint->transport.acceptedFrames, __ATOMIC_RELAXED), (unsigned long long)__atomic_load_n(&_endpoint->transport.playedFrames, __ATOMIC_RELAXED), ring_available(&_endpoint->transport.ring), _endpoint->transport.ring.capacity, __atomic_load_n(&_endpoint->captureNeed, __ATOMIC_RELAXED), __atomic_load_n(&_endpoint->captureHave, __ATOMIC_RELAXED));
			}
			_pipewire_stop_endpoint(_endpoint, (_wanted == 0 || _reopen) && _endpoint->present != 0 && _suspended == 0 && transport_live(&_endpoint->transport) != 0);
		}
	}
	/* Retire every old route before acquiring replacements. Endpoint array
	 * order must not transiently open two users of one Android audio path. */
	for (size_t _index = 0; _index < __owner->count; ++_index)
	{
		pipewire_endpoint_t *_endpoint = __owner->endpoints[_index];
		uint8_t _wanted = __atomic_load_n(&_endpoint->wanted, __ATOMIC_ACQUIRE) != 0;
		uint8_t _busy = strlen(protocol_string(_endpoint->device, "busy")) != 0;
		uint8_t _stale = strcmp(_endpoint->failure, "Stale device inventory") == 0 && _endpoint->failedGeneration == _generation;
		if (_endpoint->opened == 0 && _endpoint->present != 0 && _busy == 0 && _stale == 0 && _suspended == 0 && (_wanted != 0 || (_endpoint->capture == 0 && ring_available(&_endpoint->startup) != 0)) && _endpoint->failed == 0)
		{
			if (transport_start(&_endpoint->transport, &__owner->control, _endpoint->device, _generation, _pipewire_transport_event, _endpoint) == 0)
			{
				_endpoint->opened = 1;
				_endpoint->openedMmapPreferred = _endpoint->mmapPreferred;
				_endpoint->failure[0] = '\0';
				_pipewire_pending(_endpoint, 1);
			}
			else
			{
				_endpoint->failedGeneration = _generation;
				_endpoint->failed = 1;
				snprintf(_endpoint->failure, sizeof(_endpoint->failure), "%s", _endpoint->transport.error);
				if (strcmp(_endpoint->failure, "Stale device inventory") == 0)
				{
					/* Admission raced inventory publication. Preserve queued PCM
					 * and retry only when a fresh snapshot arrives. */
					_endpoint->failed = 0;
				}
				transport_stop(&_endpoint->transport);
				fprintf(stderr, "Cannot open Android endpoint: %s: %s\n", _endpoint->key, _endpoint->failure);
			}
		}
		if (_wanted == 0)
		{
			_endpoint->failedGeneration = 0;
			_endpoint->failed = 0;
		}
		uint8_t _accepting = _endpoint->present != 0 && _busy == 0 && _suspended == 0 && _endpoint->failed == 0;
		__atomic_store_n(&_endpoint->accepting, _accepting, __ATOMIC_RELEASE);
		if (_accepting == 0 && _endpoint->opened == 0)
		{
			_pipewire_pending(_endpoint, 0);
		}
		uint64_t _underruns = __atomic_load_n(&_endpoint->captureUnderruns, __ATOMIC_RELAXED);
		if (_underruns != _endpoint->reportedUnderruns)
		{
			fprintf(stderr, "Capture re-prime: %s count=%llu requested=%u available=%u frames; recorder retained\n", _endpoint->key, (unsigned long long)_underruns, __atomic_load_n(&_endpoint->captureNeed, __ATOMIC_RELAXED), __atomic_load_n(&_endpoint->captureHave, __ATOMIC_RELAXED));
			_endpoint->reportedUnderruns = _underruns;
		}
		char _label[320];
		snprintf(_label, sizeof(_label), "%s", _endpoint->label);
		if (_suspended != 0)
		{
			snprintf(_label, sizeof(_label), "%s (Android call/focus priority)", _endpoint->label);
		}
		else if (_endpoint->present == 0)
		{
			snprintf(_label, sizeof(_label), "%s (unavailable)", _endpoint->label);
		}
		else if (_busy != 0)
		{
			snprintf(_label, sizeof(_label), "%s (%s)", _endpoint->label, protocol_string(_endpoint->device, "busy"));
		}
		else if (_endpoint->failed != 0)
		{
			if (_endpoint->failure[0] != '\0')
			{
				snprintf(_label, sizeof(_label), "%.128s (%.180s)", _endpoint->label, _endpoint->failure);
			}
			else
			{
				snprintf(_label, sizeof(_label), "%s (route or stream error)", _endpoint->label);
			}
		}
		pw_thread_loop_lock(__owner->loop);
		struct spa_dict_item _item = SPA_DICT_ITEM_INIT(PW_KEY_NODE_DESCRIPTION, _label);
		struct spa_dict _properties = SPA_DICT_INIT(&_item, 1);
		pw_stream_update_properties(_endpoint->stream, &_properties);
		if (_endpoint->driveEvent != NULL)
		{
			pw_loop_signal_event(pw_thread_loop_get_loop(__owner->loop), _endpoint->driveEvent);
		}
		if (_endpoint->opened != 0)
		{
			_pipewire_clock(_endpoint);
		}
		pw_thread_loop_unlock(__owner->loop);
	}
	__owner->previouslySuspended = _suspended;
}

int main(int __argc, char **__argv)
{
	const char *_socketName = AUDIO_SOCKET;
	if (__argc == 3 && strcmp(__argv[1], "--socket") == 0)
	{
		_socketName = __argv[2];
	}
	pw_init(&__argc, &__argv);
	pipewire_t *_owner = calloc(1, sizeof(*_owner));
	if (_owner == NULL)
	{
		return 1;
	}
	pthread_mutex_init(&_owner->mutex, NULL);
	pthread_cond_init(&_owner->changed, NULL);
	_owner->loop = pw_thread_loop_new("linux-audio", NULL);
	if (_owner->loop == NULL)
	{
		free(_owner);
		return 1;
	}
	_owner->context = pw_context_new(pw_thread_loop_get_loop(_owner->loop), NULL, 0);
	if (_owner->context == NULL)
	{
		fputs("PipeWire client configuration unavailable\n", stderr);
		pw_thread_loop_destroy(_owner->loop);
		free(_owner);
		return 1;
	}
	_owner->core = pw_context_connect(_owner->context, NULL, 0);
	if (_owner->core == NULL || preferences_start(&_owner->preferences, _owner->core, _pipewire_notify, _owner) != 0 || pw_thread_loop_start(_owner->loop) < 0)
	{
		fputs("Linux PipeWire server unavailable\n", stderr);
		return 1;
	}
	_owner->routes = cJSON_CreateObject();
	if (control_start(&_owner->control, _socketName, _pipewire_event, _owner) != 0)
	{
		fputs("Android audio broker unavailable\n", stderr);
		control_stop(&_owner->control);
		return 1;
	}
	signal(SIGTERM, _pipewire_signal);
	signal(SIGINT, _pipewire_signal);
	while (_pipewire_quit == 0 && control_live(&_owner->control) != 0)
	{
		pthread_mutex_lock(&_owner->mutex);
		struct timespec _deadline;
		clock_gettime(CLOCK_REALTIME, &_deadline);
		_deadline.tv_nsec += 500000000;
		if (_deadline.tv_nsec >= 1000000000)
		{
			++_deadline.tv_sec;
			_deadline.tv_nsec -= 1000000000;
		}
		/* A graph/Android event can arrive while reconcile is opening or
		 * draining another endpoint. Retain that work instead of losing the
		 * notification and sleeping through the startup buffer budget. */
		if (_owner->pending == 0)
		{
			pthread_cond_timedwait(&_owner->changed, &_owner->mutex, &_deadline);
		}
		_owner->pending = 0;
		cJSON *_inventory = cJSON_Duplicate(_owner->inventory, 1);
		pthread_mutex_unlock(&_owner->mutex);
		if (_inventory != NULL)
		{
			_pipewire_reconcile(_owner, _inventory);
			cJSON_Delete(_inventory);
		}
	}
	control_stop(&_owner->control);
	for (size_t _index = 0; _index < _owner->count; ++_index)
	{
		pipewire_endpoint_t *_endpoint = _owner->endpoints[_index];
		_pipewire_stop_endpoint(_endpoint, 0);
		__atomic_store_n(&_endpoint->accepting, 0, __ATOMIC_RELEASE);
		_pipewire_pending(_endpoint, 0);
		pw_thread_loop_lock(_owner->loop);
		pw_stream_destroy(_endpoint->stream);
		pw_loop_destroy_source(pw_thread_loop_get_loop(_owner->loop), _endpoint->graphWatchdog);
		pw_loop_destroy_source(pw_thread_loop_get_loop(_owner->loop), _endpoint->driveEvent);
		pw_thread_loop_unlock(_owner->loop);
		cJSON_Delete(_endpoint->device);
		ring_destroy(&_endpoint->startup);
		free(_endpoint);
	}
	pw_thread_loop_stop(_owner->loop);
	preferences_stop(&_owner->preferences);
	pw_core_disconnect(_owner->core);
	pw_context_destroy(_owner->context);
	pw_thread_loop_destroy(_owner->loop);
	cJSON_Delete(_owner->inventory);
	cJSON_Delete(_owner->routes);
	pthread_cond_destroy(&_owner->changed);
	pthread_mutex_destroy(&_owner->mutex);
	free(_owner);
	pw_deinit();
	return 0;
}
