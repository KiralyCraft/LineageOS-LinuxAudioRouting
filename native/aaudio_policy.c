#include "../include/aaudio_policy.h"
#include <dlfcn.h>
#include <pthread.h>

static pthread_mutex_t _aaudio_policy_mutex = PTHREAD_MUTEX_INITIALIZER;

int32_t aaudio_policy_open(AAudioStreamBuilder *__builder, AAudioStream **__stream, uint8_t __preferMmap)
{
	/* Android's documented testing-policy exports apply to this process, not
	 * the system. Serialize our opens and restore the previous policy before
	 * admitting another stream. Existing streams keep their selected backend. */
	pthread_mutex_lock(&_aaudio_policy_mutex);
	int32_t (*_getPolicy)(void) = dlsym(RTLD_DEFAULT, "AAudio_getMMapPolicy");
	int32_t (*_setPolicy)(int32_t) = dlsym(RTLD_DEFAULT, "AAudio_setMMapPolicy");
	int32_t _result;
	if (_getPolicy != NULL && _setPolicy != NULL)
	{
		int32_t _previous = _getPolicy();
		int32_t _policy = 1; /* NEVER: keep low-latency legacy AAudio available. */
		if (__preferMmap)
		{
			_policy = 2; /* AUTO: prefer MMAP, retain a functioning fallback. */
		}
		_result = _setPolicy(_policy);
		if (_result == AAUDIO_OK)
		{
			_result = AAudioStreamBuilder_openStream(__builder, __stream);
			_setPolicy(_previous);
		}
	}
	else
	{
		/* The pinned framework excludes MMAP without LOW_LATENCY. This
		 * public-API fallback avoids depending on optional exports to work. */
		if (!__preferMmap)
		{
			AAudioStreamBuilder_setPerformanceMode(__builder, AAUDIO_PERFORMANCE_MODE_NONE);
		}
		_result = AAudioStreamBuilder_openStream(__builder, __stream);
	}
	pthread_mutex_unlock(&_aaudio_policy_mutex);
	return _result;
}
