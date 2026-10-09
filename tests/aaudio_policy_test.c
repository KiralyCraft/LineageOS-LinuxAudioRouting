#define _POSIX_C_SOURCE 200809L
#include "../include/aaudio_policy.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static int32_t _aaudio_policy_test_policy;
static int32_t _aaudio_policy_test_exports = 1;
static int32_t _aaudio_policy_test_reject;
static int32_t _aaudio_policy_test_get(void)
{
	return _aaudio_policy_test_policy;
}
static int32_t _aaudio_policy_test_set(int32_t __policy)
{
	if (_aaudio_policy_test_reject)
	{
		return -1;
	}
	_aaudio_policy_test_policy = __policy;
	return 0;
}
void *aaudio_policy_test_lookup(void *__handle, const char *__symbol)
{
	(void)__handle;
	if (!_aaudio_policy_test_exports)
	{
		return NULL;
	}
	if (strcmp(__symbol, "AAudio_getMMapPolicy") == 0)
	{
		return _aaudio_policy_test_get;
	}
	assert(strcmp(__symbol, "AAudio_setMMapPolicy") == 0);
	return _aaudio_policy_test_set;
}
int32_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *__builder, AAudioStream **__stream)
{
	(void)__stream;
	if (_aaudio_policy_test_exports)
	{
		assert(_aaudio_policy_test_policy == __builder->expectedPolicy);
		struct timespec _delay = {0, 1000000};
		nanosleep(&_delay, NULL);
		assert(_aaudio_policy_test_policy == __builder->expectedPolicy);
	}
	else if (__builder->expectedPolicy == 1)
	{
		assert(__builder->mode == AAUDIO_PERFORMANCE_MODE_NONE);
	}
	else
	{
		assert(__builder->mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
	}
	return __builder->result;
}
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *__builder, int32_t __mode)
{
	__builder->mode = __mode;
}
static void *_aaudio_policy_test_worker(void *__context)
{
	AAudioStreamBuilder *_builder = __context;
	AAudioStream *_stream = NULL;
	for (uint32_t _index = 0; _index < 40; ++_index)
	{
		assert(aaudio_policy_open(_builder, &_stream, _builder->expectedPolicy == 2) == _builder->result);
	}
	return NULL;
}
int main(void)
{
	AAudioStreamBuilder _builders[2] = {{1, 12, 0}, {2, 12, -99}};
	pthread_t _threads[2];
	for (uint32_t _index = 0; _index < 2; ++_index)
	{
		assert(pthread_create(&_threads[_index], NULL, _aaudio_policy_test_worker, &_builders[_index]) == 0);
	}
	for (uint32_t _index = 0; _index < 2; ++_index)
	{
		assert(pthread_join(_threads[_index], NULL) == 0);
	}
	assert(_aaudio_policy_test_policy == 0);
	AAudioStream *_stream = NULL;
	_aaudio_policy_test_reject = 1;
	assert(aaudio_policy_open(&_builders[0], &_stream, 0) == -1);
	assert(_aaudio_policy_test_policy == 0);
	_aaudio_policy_test_reject = 0;
	_aaudio_policy_test_exports = 0;
	assert(aaudio_policy_open(&_builders[0], &_stream, 0) == 0);
	assert(aaudio_policy_open(&_builders[1], &_stream, 1) == -99);
	puts("PASS MMAP policy: serialized concurrent AUTO/NEVER opens, failed-open restoration, rejected policy, missing-export fallback");
	return 0;
}
