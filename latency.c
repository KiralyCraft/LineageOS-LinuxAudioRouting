#include "include/latency.h"
#include <stddef.h>

uint8_t latency_estimate(const latency_t *__point, uint64_t __produced, uint32_t __rate, uint64_t __now, uint64_t *__delay)
{
	if (__point == NULL || __delay == NULL || __rate == 0 || __point->time == 0 || __produced < __point->frame)
	{
		return 0;
	}
	/* Bound metadata age and arithmetic independently of audio flow control. */
	uint64_t _distance;
	if (__point->time > __now)
	{
		_distance = __point->time - __now;
	}
	else
	{
		_distance = __now - __point->time;
	}
	uint64_t _frames = __produced - __point->frame;
	if (_distance > UINT64_C(2000000000) || _frames > (uint64_t)__rate * 10)
	{
		return 0;
	}
	uint64_t _queued = _frames * UINT64_C(1000000000) / __rate;
	if (__point->time > __now)
	{
		_queued += _distance;
	}
	else if (_queued >= _distance)
	{
		_queued -= _distance;
	}
	else
	{
		_queued = 0;
	}
	*__delay = _queued;
	return 1;
}
