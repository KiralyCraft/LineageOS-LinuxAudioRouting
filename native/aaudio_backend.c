#define _GNU_SOURCE
#include "../include/shared.h"
#include "../include/protocol.h"
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <jni.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
	AAudioStream *stream;
	shared_t shared;
	uint64_t epoch;
	uint32_t rate;
	uint32_t channels;
	uint32_t sampleBytes;
	size_t capacity;
	uint8_t capture;
	uint8_t presentation;
	int32_t device;
	int32_t cancelled;
	int32_t error;
	int32_t wake;
} aaudio_backend_t;

static void _aaudio_backend_error(AAudioStream *__stream, void *__context, aaudio_result_t __error)
{
	(void)__stream;
	aaudio_backend_t *_backend = __context;
	__atomic_store_n(&_backend->error, __error, __ATOMIC_RELEASE);
	shared_notify(_backend->wake);
	/* The worker uses bounded native I/O waits; no JNI, allocation or close
	 * on AAudio's error callback thread. */
}

static void _aaudio_backend_throw(JNIEnv *__env, const char *__message)
{
	jclass _exception = (*__env)->FindClass(__env, "java/io/IOException");
	if (_exception != NULL)
	{
		(*__env)->ThrowNew(__env, _exception, __message);
	}
}

JNIEXPORT jlong JNICALL Java_dev_kiraly_linuxaudio_NativeAudio_create(JNIEnv *__env, jclass __class, jint __device, jint __rate, jint __channels, jint __sampleBytes, jboolean __capture, jint __source, jboolean __headset, jlong __epoch, jboolean __presentation, jstring __package)
{
	(void)__class;
	aaudio_backend_t *_backend = calloc(1, sizeof(*_backend));
	if (_backend == NULL)
	{
		_aaudio_backend_throw(__env, "Native audio allocation failed");
		return 0;
	}
	_backend->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (_backend->wake < 0)
	{
		free(_backend);
		_aaudio_backend_throw(__env, "Native audio wake descriptor failed");
		return 0;
	}
	shared_init(&_backend->shared);
	_backend->device = __device;
	_backend->rate = (uint32_t)__rate;
	_backend->channels = (uint32_t)__channels;
	_backend->sampleBytes = (uint32_t)__sampleBytes;
	_backend->capture = __capture;
	_backend->capacity = (size_t)__rate * 80 / 1000;
	if (__capture)
	{
		if (__headset)
		{
			_backend->capacity += (size_t)__rate / 25;
		}
		else
		{
			_backend->capacity += (size_t)__rate / 50;
		}
	}
	_backend->capacity *= (size_t)__channels * __sampleBytes;
	_backend->epoch = (uint64_t)__epoch;
	_backend->presentation = __presentation;
	AAudioStreamBuilder *_builder = NULL;
	int32_t _result = AAudio_createStreamBuilder(&_builder);
	if (_result == AAUDIO_OK)
	{
		AAudioStreamBuilder_setDeviceId(_builder, __device);
		AAudioStreamBuilder_setSampleRate(_builder, __rate);
		AAudioStreamBuilder_setChannelCount(_builder, __channels);
		AAudioStreamBuilder_setSharingMode(_builder, AAUDIO_SHARING_MODE_SHARED);
		AAudioStreamBuilder_setPerformanceMode(_builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
		AAudioStreamBuilder_setErrorCallback(_builder, _aaudio_backend_error, _backend);
		if (__sampleBytes == 2)
		{
			AAudioStreamBuilder_setFormat(_builder, AAUDIO_FORMAT_PCM_I16);
		}
		else
		{
			AAudioStreamBuilder_setFormat(_builder, AAUDIO_FORMAT_PCM_FLOAT);
		}
		if (__capture)
		{
			AAudioStreamBuilder_setDirection(_builder, AAUDIO_DIRECTION_INPUT);
			AAudioStreamBuilder_setInputPreset(_builder, __source);
		}
		else
		{
			AAudioStreamBuilder_setDirection(_builder, AAUDIO_DIRECTION_OUTPUT);
			AAudioStreamBuilder_setUsage(_builder, AAUDIO_USAGE_MEDIA);
			AAudioStreamBuilder_setContentType(_builder, AAUDIO_CONTENT_TYPE_MUSIC);
			if (__headset)
			{
				AAudioStreamBuilder_setUsage(_builder, AAUDIO_USAGE_VOICE_COMMUNICATION);
				AAudioStreamBuilder_setContentType(_builder, AAUDIO_CONTENT_TYPE_SPEECH);
			}
		}
		const char *_package = (*__env)->GetStringUTFChars(__env, __package, NULL);
		if (_package != NULL)
		{
			AAudioStreamBuilder_setPackageName(_builder, _package);
			_result = AAudioStreamBuilder_openStream(_builder, &_backend->stream);
			(*__env)->ReleaseStringUTFChars(__env, __package, _package);
		}
		else
		{
			_result = AAUDIO_ERROR_NO_MEMORY;
		}
		AAudioStreamBuilder_delete(_builder);
	}
	int32_t _format = AAUDIO_FORMAT_PCM_FLOAT;
	if (__sampleBytes == 2)
	{
		_format = AAUDIO_FORMAT_PCM_I16;
	}
	if (_result != AAUDIO_OK || AAudioStream_getSampleRate(_backend->stream) != __rate || AAudioStream_getChannelCount(_backend->stream) != __channels || AAudioStream_getFormat(_backend->stream) != _format)
	{
		char _message[128];
		snprintf(_message, sizeof(_message), "AAudio open/format rejected: %d", _result);
		if (_backend->stream != NULL)
		{
			AAudioStream_close(_backend->stream);
		}
		close(_backend->wake);
		free(_backend);
		_aaudio_backend_throw(__env, _message);
		return 0;
	}
	int32_t _burst = AAudioStream_getFramesPerBurst(_backend->stream);
	if (_burst > 0)
	{
		AAudioStream_setBufferSizeInFrames(_backend->stream, 2 * _burst);
	}
	return (jlong)(uintptr_t)_backend;
}

static void _aaudio_backend_clock(aaudio_backend_t *__backend)
{
	shared_header_t *_header = __backend->shared.header;
	int64_t _frame = 0;
	int64_t _time = 0;
	uint64_t _now = protocol_now();
	int32_t _valid = AAudioStream_getTimestamp(__backend->stream, CLOCK_MONOTONIC, &_frame, &_time);
	int64_t _played = AAudioStream_getFramesRead(__backend->stream);
	if (_played < 0)
	{
		_played = 0;
	}
	uint64_t _transferred = __atomic_load_n(&_header->reader, __ATOMIC_ACQUIRE) / (__backend->channels * __backend->sampleBytes);
	if (__backend->capture != 0)
	{
		_transferred = __atomic_load_n(&_header->writer, __ATOMIC_ACQUIRE) / (__backend->channels * __backend->sampleBytes);
	}
	if ((uint64_t)_played > _transferred)
	{
		_played = (int64_t)_transferred;
	}
	__atomic_fetch_add(&_header->serial, 1, __ATOMIC_ACQ_REL);
	__atomic_store_n(&_header->played, (uint64_t)_played, __ATOMIC_RELAXED);
	__atomic_store_n(&_header->clockFrame, (uint64_t)_played, __ATOMIC_RELAXED);
	__atomic_store_n(&_header->clockTime, _now, __ATOMIC_RELAXED);
	if (__backend->capture != 0 && _valid == AAUDIO_OK && _frame >= 0 && _time > 0)
	{
		int64_t _captureTime = _time + ((int64_t)_transferred - _frame) * INT64_C(1000000000) / __backend->rate;
		if (_captureTime > 0)
		{
			__atomic_store_n(&_header->clockFrame, _transferred, __ATOMIC_RELAXED);
			__atomic_store_n(&_header->clockTime, (uint64_t)_captureTime, __ATOMIC_RELAXED);
		}
	}
	/* Bluetooth MMAP timestamps have not been calibrated through codec and
	 * headset buffering. Do not publish them as end-to-end presentation. */
	if (__backend->presentation && _valid == AAUDIO_OK && _frame >= 0 && (uint64_t)_frame <= _transferred && _time > 0)
	{
		__atomic_store_n(&_header->presentationFrame, (uint64_t)_frame, __ATOMIC_RELAXED);
		__atomic_store_n(&_header->presentationTime, (uint64_t)_time, __ATOMIC_RELAXED);
	}
	__atomic_fetch_add(&_header->serial, 1, __ATOMIC_RELEASE);
	int32_t _xruns = AAudioStream_getXRunCount(__backend->stream);
	if (_xruns >= 0)
	{
		__atomic_store_n(&_header->xruns, (uint32_t)_xruns, __ATOMIC_RELEASE);
	}
}

JNIEXPORT void JNICALL Java_dev_kiraly_linuxaudio_NativeAudio_run(JNIEnv *__env, jclass __class, jlong __handle, jint __socket, jobject __owner)
{
	(void)__class;
	aaudio_backend_t *_backend = (aaudio_backend_t *)(uintptr_t)__handle;
	shared_t *_shared = &_backend->shared;
	int32_t _error = 0;
	int32_t _socket = dup(__socket);
	if (_socket < 0)
	{
		_aaudio_backend_throw(__env, "Native audio socket duplication failed");
		return;
	}
	if (shared_create(_shared, _backend->epoch, _backend->rate, _backend->channels, _backend->sampleBytes, _backend->capture, _backend->capacity) != 0 || shared_send(_shared, _socket) != 0)
	{
		close(_socket);
		_aaudio_backend_throw(__env, "Unable to export Android-owned shared PCM");
		return;
	}
	bool (*_isMmap)(AAudioStream *) = dlsym(RTLD_DEFAULT, "AAudioStream_isMMapUsed");
	uint32_t _mmap = 0;
	if (_isMmap != NULL)
	{
		_mmap = _isMmap(_backend->stream);
	}
	__atomic_store_n(&_shared->header->mmapUsed, _mmap, __ATOMIC_RELEASE);
	_error = AAudioStream_requestStart(_backend->stream);
	if (_error != AAUDIO_OK)
	{
		goto finish;
	}
	aaudio_stream_state_t _state = AAudioStream_getState(_backend->stream);
	if (_state == AAUDIO_STREAM_STATE_STARTING)
	{
		_error = AAudioStream_waitForStateChange(_backend->stream, _state, &_state, INT64_C(1000000000));
	}
	if (_error != AAUDIO_OK || _state != AAUDIO_STREAM_STATE_STARTED || AAudioStream_getDeviceId(_backend->stream) != _backend->device)
	{
		_error = AAUDIO_ERROR_DISCONNECTED;
		goto finish;
	}
	jclass _ownerClass = (*__env)->GetObjectClass(__env, __owner);
	jmethodID _ready = (*__env)->GetMethodID(__env, _ownerClass, "nativeReady", "(IZ)V");
	if (_ready == NULL)
	{
		goto finish;
	}
	(*__env)->CallVoidMethod(__env, __owner, _ready, _backend->device, (jboolean)_mmap);
	if ((*__env)->ExceptionCheck(__env))
	{
		goto finish;
	}
	uint32_t _frameBytes = _backend->channels * _backend->sampleBytes;
	int32_t _initialXruns = AAudioStream_getXRunCount(_backend->stream);
	while (!__atomic_load_n(&_backend->cancelled, __ATOMIC_ACQUIRE) && !__atomic_load_n(&_shared->header->stopped, __ATOMIC_ACQUIRE))
	{
		_error = __atomic_load_n(&_backend->error, __ATOMIC_ACQUIRE);
		if (_error != 0 || AAudioStream_getDeviceId(_backend->stream) != _backend->device)
		{
			if (_error == 0)
			{
				_error = AAUDIO_ERROR_DISCONNECTED;
			}
			break;
		}
		uint64_t _reader = __atomic_load_n(&_shared->header->reader, __ATOMIC_ACQUIRE);
		uint64_t _writer = __atomic_load_n(&_shared->header->writer, __ATOMIC_ACQUIRE);
		if (_writer - _reader > _shared->ring.capacity || _reader % _frameBytes != 0 || _writer % _frameBytes != 0)
		{
			_error = AAUDIO_ERROR_ILLEGAL_ARGUMENT;
			break;
		}
		size_t _available = (size_t)(_writer - _reader);
		uint64_t _position = _reader;
		int32_t _waitEvent = _shared->producerEvent;
		int32_t _notifyEvent = _shared->consumerEvent;
		if (_backend->capture)
		{
			_available = _shared->ring.capacity - _available;
			_position = _writer;
			_waitEvent = _shared->consumerEvent;
			_notifyEvent = _shared->producerEvent;
		}
		size_t _offset = (size_t)(_position % _shared->ring.capacity);
		if (_available > _shared->ring.capacity - _offset)
		{
			_available = _shared->ring.capacity - _offset;
		}
		if (_available > (size_t)(_backend->rate / 100) * _frameBytes)
		{
			_available = (size_t)(_backend->rate / 100) * _frameBytes;
		}
		if (_available != 0)
		{
			int32_t _frames = (int32_t)(_available / _frameBytes);
			int32_t _result;
			if (_backend->capture)
			{
				_result = AAudioStream_read(_backend->stream, _shared->ring.bytes + _offset, _frames, INT64_C(50000000));
			}
			else
			{
				_result = AAudioStream_write(_backend->stream, _shared->ring.bytes + _offset, _frames, INT64_C(50000000));
			}
			if (_result < 0)
			{
				_error = _result;
				break;
			}
			if (_result > 0)
			{
				if (_backend->capture)
				{
					__atomic_store_n(&_shared->header->writer, _writer + (uint64_t)_result * _frameBytes, __ATOMIC_RELEASE);
				}
				else
				{
					__atomic_store_n(&_shared->header->reader, _reader + (uint64_t)_result * _frameBytes, __ATOMIC_RELEASE);
				}
				_aaudio_backend_clock(_backend);
				shared_notify(_notifyEvent);
			}
			if (_backend->capture && AAudioStream_getXRunCount(_backend->stream) > _initialXruns)
			{
				_error = AAUDIO_ERROR_INTERNAL;
				break;
			}
		}
		else
		{
			/* No PCM timer drives the producer. While draining the native
			 * output tail only, sample hardware progress to retire that tail. */
			int32_t _timeout = -1;
			if (!_backend->capture && __atomic_load_n(&_shared->header->played, __ATOMIC_ACQUIRE) < _reader / _frameBytes)
			{
				_timeout = 5;
			}
			struct pollfd _poll[3] = {{_socket, POLLIN, 0}, {_waitEvent, POLLIN, 0}, {_backend->wake, POLLIN, 0}};
			int32_t _result = poll(_poll, 3, _timeout);
			if (_result < 0 && errno == EINTR)
			{
				continue;
			}
			if (_result < 0 || _poll[0].revents != 0 || (_poll[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
			{
				break;
			}
			if ((_poll[2].revents & POLLIN) != 0)
			{
				shared_clear(_backend->wake);
			}
			if ((_poll[1].revents & POLLIN) != 0)
			{
				shared_clear(_waitEvent);
			}
			_aaudio_backend_clock(_backend);
			shared_notify(_notifyEvent);
		}
	}
finish:
	close(_socket);
	__atomic_store_n(&_shared->header->error, _error, __ATOMIC_RELEASE);
	__atomic_store_n(&_shared->header->stopped, 1, __ATOMIC_RELEASE);
	shared_notify(_shared->consumerEvent);
	shared_notify(_shared->producerEvent);
	if (_error != 0 && !__atomic_load_n(&_backend->cancelled, __ATOMIC_ACQUIRE))
	{
		char _message[128];
		snprintf(_message, sizeof(_message), "AAudio stream failed: %d (%s)", _error, AAudio_convertResultToText(_error));
		_aaudio_backend_throw(__env, _message);
	}
}

JNIEXPORT void JNICALL Java_dev_kiraly_linuxaudio_NativeAudio_cancel(JNIEnv *__env, jclass __class, jlong __handle)
{
	(void)__env;
	(void)__class;
	aaudio_backend_t *_backend = (aaudio_backend_t *)(uintptr_t)__handle;
	__atomic_store_n(&_backend->cancelled, 1, __ATOMIC_RELEASE);
	shared_notify(_backend->wake);
	/* Java shuts down the attached socket to wake poll/descriptor receipt.
	 * In-flight AAudio calls have a bounded wait; close happens after join. */
}

JNIEXPORT void JNICALL Java_dev_kiraly_linuxaudio_NativeAudio_destroy(JNIEnv *__env, jclass __class, jlong __handle)
{
	(void)__env;
	(void)__class;
	aaudio_backend_t *_backend = (aaudio_backend_t *)(uintptr_t)__handle;
	AAudioStream_close(_backend->stream);
	shared_close(&_backend->shared);
	close(_backend->wake);
	free(_backend);
}
