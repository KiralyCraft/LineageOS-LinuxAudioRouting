#define _POSIX_C_SOURCE 200809L
#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int64_t _aaudio_probe_now(void)
{
	struct timespec _time;
	clock_gettime(CLOCK_MONOTONIC, &_time);
	return (int64_t)_time.tv_sec * INT64_C(1000000000) + _time.tv_nsec;
}

int main(int __argc, char **__argv)
{
	if (__argc != 2)
	{
		return 2;
	}
	AAudioStreamBuilder *_builder = NULL;
	if (AAudio_createStreamBuilder(&_builder) != AAUDIO_OK)
	{
		return 3;
	}
	AAudioStreamBuilder_setDeviceId(_builder, (int32_t)strtol(__argv[1], NULL, 10));
	AAudioStreamBuilder_setDirection(_builder, AAUDIO_DIRECTION_OUTPUT);
	AAudioStreamBuilder_setSharingMode(_builder, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setPerformanceMode(_builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
	AAudioStreamBuilder_setFormat(_builder, AAUDIO_FORMAT_PCM_FLOAT);
	AAudioStreamBuilder_setSampleRate(_builder, 48000);
	AAudioStreamBuilder_setChannelCount(_builder, 2);
	AAudioStream *_stream = NULL;
	int32_t _result = AAudioStreamBuilder_openStream(_builder, &_stream);
	AAudioStreamBuilder_delete(_builder);
	if (_result != AAUDIO_OK)
	{
		printf("open_error=%d %s\n", _result, AAudio_convertResultToText(_result));
		return 4;
	}
	bool (*_isMmap)(AAudioStream *) = dlsym(RTLD_DEFAULT, "AAudioStream_isMMapUsed");
	int32_t _mmap = -1;
	if (_isMmap != NULL)
	{
		_mmap = _isMmap(_stream);
	}
	printf("device=%d rate=%d sharing=%d performance=%d burst=%d capacity=%d mmap=%d\n", AAudioStream_getDeviceId(_stream), AAudioStream_getSampleRate(_stream), AAudioStream_getSharingMode(_stream), AAudioStream_getPerformanceMode(_stream), AAudioStream_getFramesPerBurst(_stream), AAudioStream_getBufferCapacityInFrames(_stream), _mmap);
	AAudioStream_setBufferSizeInFrames(_stream, 2 * AAudioStream_getFramesPerBurst(_stream));
	_result = AAudioStream_requestStart(_stream);
	if (_result != AAUDIO_OK)
	{
		printf("start_error=%d\n", _result);
		AAudioStream_close(_stream);
		return 5;
	}
	float _silence[960] = {0};
	int64_t _end = _aaudio_probe_now() + INT64_C(4000000000);
	int64_t _nextReport = 0;
	while (_aaudio_probe_now() < _end)
	{
		_result = AAudioStream_write(_stream, _silence, 480, INT64_C(1000000000));
		if (_result < 0)
		{
			printf("write_error=%d\n", _result);
			break;
		}
		int64_t _now = _aaudio_probe_now();
		if (_now >= _nextReport)
		{
			_nextReport = _now + INT64_C(500000000);
			int64_t _frame = 0;
			int64_t _time = 0;
			int32_t _valid = AAudioStream_getTimestamp(_stream, CLOCK_MONOTONIC, &_frame, &_time);
			printf("sample timestamp_result=%d frame=%" PRId64 " time=%" PRId64 " now=%" PRId64 " written=%" PRId64 " size=%d xruns=%d device=%d\n", _valid, _frame, _time, _now, AAudioStream_getFramesWritten(_stream), AAudioStream_getBufferSizeInFrames(_stream), AAudioStream_getXRunCount(_stream), AAudioStream_getDeviceId(_stream));
			fflush(stdout);
		}
	}
	AAudioStream_close(_stream);
	return 0;
}
