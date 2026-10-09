#define _GNU_SOURCE
#include "include/transport.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PIPEWIRE_ENDPOINTS 64
typedef struct pipewire pipewire_t;
typedef struct
{
	pipewire_t *owner;
	char key[100];
	char label[256];
	cJSON *device;
	struct pw_stream *stream;
	struct spa_hook listener;
	transport_t transport;
	int32_t wanted;
	int32_t processing;
	int32_t accepting;
	ring_t startup;
	int32_t startupFailed;
	uint8_t present;
	uint8_t opened;
	uint8_t capture;
	uint32_t frameBytes;
	uint64_t failedGeneration;
	uint64_t clockFrame;
	uint64_t clockTime;
	double rateFactor;
} pipewire_endpoint_t;

struct pipewire
{
	control_t control;
	struct pw_thread_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	pthread_mutex_t mutex;
	pthread_cond_t changed;
	cJSON *inventory;
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
		fprintf(stderr, "route verified stream=%llu android_device=%llu\n", (unsigned long long)protocol_number(__message, "stream", 0), (unsigned long long)protocol_number(__message, "actual_android_id", 0));
	}
	else if (strcmp(protocol_string(__message, "op"), "stream_closed") == 0 && strlen(protocol_string(__message, "error")) != 0)
	{
		fprintf(stderr, "Android stream stopped: %s\n", protocol_string(__message, "error"));
	}
	pthread_cond_signal(&_owner->changed);
	pthread_mutex_unlock(&_owner->mutex);
}

static void _pipewire_state(void *__context, enum pw_stream_state __old, enum pw_stream_state __state, const char *__error)
{
	pipewire_endpoint_t *_endpoint = __context;
	fprintf(stderr, "Endpoint state %s: %s -> %s\n", _endpoint->key, pw_stream_state_as_string(__old), pw_stream_state_as_string(__state));
	__atomic_store_n(&_endpoint->wanted, __state == PW_STREAM_STATE_STREAMING, __ATOMIC_RELEASE);
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
			transport_fail(&__endpoint->transport);
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
			_length = ring_read(&_endpoint->transport.ring, _data->data, _requested);
			if (_length < _requested)
			{
				transport_fail(&_endpoint->transport);
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
		ring_t *_ring = &_endpoint->startup;
		if (_processing != 0 && ring_available(&_endpoint->startup) == 0)
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
				transport_fail(&_endpoint->transport);
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
	struct pw_loop *_dataLoop = pw_stream_get_data_loop(__endpoint->stream);
	if (_dataLoop != NULL)
	{
		pw_loop_invoke(_dataLoop, _pipewire_barrier, SPA_ID_INVALID, &__processing, sizeof(__processing), true, __endpoint);
	}
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
	if (ring_init(&_endpoint->startup, (size_t)_rate * _endpoint->frameBytes / 2) != 0)
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
	struct pw_properties *_properties = pw_properties_new(PW_KEY_NODE_NAME, _nodeName, PW_KEY_NODE_DESCRIPTION, _endpoint->label, PW_KEY_MEDIA_CLASS, _mediaClass, PW_KEY_NODE_VIRTUAL, "true", PW_KEY_NODE_WANT_DRIVER, "true", PW_KEY_NODE_PAUSE_ON_IDLE, "true", "linux.audio.endpoint", _endpoint->key, "linux.audio.profile", protocol_string(__device, "profile"), "node.latency", "256/48000", "resample.disable", "false", NULL);
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
	pw_thread_loop_lock(__owner->loop);
	_endpoint->stream = pw_stream_new(__owner->core, _endpoint->label, _properties);
	if (_endpoint->stream == NULL)
	{
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
	int32_t _result = pw_stream_connect(_endpoint->stream, _direction, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, &_formatPod, 1);
	pw_thread_loop_unlock(__owner->loop);
	if (_result < 0)
	{
		pw_thread_loop_lock(__owner->loop);
		pw_stream_destroy(_endpoint->stream);
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
				__owner->endpoints[__owner->count++] = _endpoint;
			}
		}
		if (_endpoint != NULL)
		{
			cJSON_Delete(_endpoint->device);
			_endpoint->device = cJSON_Duplicate(_device, 1);
			_endpoint->present = protocol_boolean(_device, "available");
		}
	}
	for (size_t _index = 0; _index < __owner->count; ++_index)
	{
		pipewire_endpoint_t *_endpoint = __owner->endpoints[_index];
		if (__atomic_exchange_n(&_endpoint->startupFailed, 0, __ATOMIC_ACQ_REL) != 0)
		{
			_endpoint->failedGeneration = _generation;
			fprintf(stderr, "PCM startup queue overrun: %s\n", _endpoint->key);
		}
		uint8_t _wanted = __atomic_load_n(&_endpoint->wanted, __ATOMIC_ACQUIRE) != 0;
		if (_endpoint->opened != 0 && (_endpoint->present == 0 || _suspended != 0 || _wanted == 0 || _endpoint->failedGeneration == _generation || transport_live(&_endpoint->transport) == 0))
		{
			if (__atomic_load_n(&_endpoint->transport.failed, __ATOMIC_ACQUIRE) != 0)
			{
				_endpoint->failedGeneration = _generation;
				fprintf(stderr, "PCM stream failed: %s; no samples silently dropped\n", _endpoint->key);
			}
			_pipewire_stop_endpoint(_endpoint, _wanted == 0 && _endpoint->present != 0 && _suspended == 0 && transport_live(&_endpoint->transport) != 0);
		}
		if (_endpoint->opened == 0 && _endpoint->present != 0 && _suspended == 0 && _wanted != 0 && _endpoint->failedGeneration != _generation)
		{
			if (transport_start(&_endpoint->transport, &__owner->control, _endpoint->device, _generation, _pipewire_notify, __owner) == 0)
			{
				_endpoint->opened = 1;
				_pipewire_pending(_endpoint, 1);
			}
			else
			{
				_endpoint->failedGeneration = _generation;
				transport_stop(&_endpoint->transport);
				fprintf(stderr, "Cannot open Android endpoint: %s\n", _endpoint->key);
			}
		}
		if (_wanted == 0)
		{
			_endpoint->failedGeneration = 0;
		}
		uint8_t _accepting = _endpoint->present != 0 && _suspended == 0 && _endpoint->failedGeneration != _generation;
		__atomic_store_n(&_endpoint->accepting, _accepting, __ATOMIC_RELEASE);
		if (_accepting == 0 && _endpoint->opened == 0)
		{
			_pipewire_pending(_endpoint, 0);
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
		else if (_endpoint->failedGeneration == _generation)
		{
			snprintf(_label, sizeof(_label), "%s (route or stream error)", _endpoint->label);
		}
		pw_thread_loop_lock(__owner->loop);
		struct spa_dict_item _item = SPA_DICT_ITEM_INIT(PW_KEY_NODE_DESCRIPTION, _label);
		struct spa_dict _properties = SPA_DICT_INIT(&_item, 1);
		pw_stream_update_properties(_endpoint->stream, &_properties);
		if (_endpoint->opened != 0)
		{
			_pipewire_clock(_endpoint);
		}
		pw_thread_loop_unlock(__owner->loop);
	}
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
	if (_owner->core == NULL || pw_thread_loop_start(_owner->loop) < 0)
	{
		fputs("Linux PipeWire server unavailable\n", stderr);
		return 1;
	}
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
		pthread_cond_timedwait(&_owner->changed, &_owner->mutex, &_deadline);
		cJSON *_inventory = cJSON_Duplicate(_owner->inventory, 1);
		pthread_mutex_unlock(&_owner->mutex);
		if (_inventory != NULL)
		{
			_pipewire_reconcile(_owner, _inventory);
			cJSON_Delete(_inventory);
		}
	}
	for (size_t _index = 0; _index < _owner->count; ++_index)
	{
		pipewire_endpoint_t *_endpoint = _owner->endpoints[_index];
		_pipewire_stop_endpoint(_endpoint, 0);
		__atomic_store_n(&_endpoint->accepting, 0, __ATOMIC_RELEASE);
		_pipewire_pending(_endpoint, 0);
		pw_thread_loop_lock(_owner->loop);
		pw_stream_destroy(_endpoint->stream);
		pw_thread_loop_unlock(_owner->loop);
		cJSON_Delete(_endpoint->device);
		ring_destroy(&_endpoint->startup);
		free(_endpoint);
	}
	control_stop(&_owner->control);
	pw_thread_loop_stop(_owner->loop);
	pw_core_disconnect(_owner->core);
	pw_context_destroy(_owner->context);
	pw_thread_loop_destroy(_owner->loop);
	cJSON_Delete(_owner->inventory);
	pthread_cond_destroy(&_owner->changed);
	pthread_mutex_destroy(&_owner->mutex);
	free(_owner);
	pw_deinit();
	return 0;
}
