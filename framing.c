#include "include/framing.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static uint64_t _framing_get(const uint8_t *__bytes, size_t __width)
{
	uint64_t _value = 0;
	for (size_t _index = 0; _index < __width; ++_index)
	{
		_value |= (uint64_t)__bytes[_index] << (8 * _index);
	}
	return _value;
}

static void _framing_put(uint8_t *__bytes, uint64_t __value, size_t __width)
{
	for (size_t _index = 0; _index < __width; ++_index)
	{
		__bytes[_index] = (uint8_t)(__value >> (8 * _index));
	}
}

void framing_clear(framing_t *__framing)
{
	free(__framing->payload);
	free(__framing->output);
	memset(__framing, 0, sizeof(*__framing));
}

static int32_t _framing_read(int32_t __socket, uint8_t *__bytes, size_t *__read, size_t __length)
{
	ssize_t _count = recv(__socket, __bytes + *__read, __length - *__read, MSG_DONTWAIT);
	if (_count < 0 && (errno == EAGAIN || errno == EINTR))
	{
		return 0;
	}
	if (_count <= 0)
	{
		return -1;
	}
	*__read += (size_t)_count;
	if (*__read == __length)
	{
		return 1;
	}
	return 0;
}

int32_t framing_receive(int32_t __socket, framing_t *__framing, uint8_t __pcm, cJSON **__message, protocol_t *__packet)
{
	*__message = NULL;
	size_t _headerLength = 4;
	if (__pcm != 0)
	{
		_headerLength = AUDIO_PCM_HEADER;
	}
	if (__framing->headerLength < _headerLength)
	{
		int32_t _result = _framing_read(__socket, __framing->header, &__framing->headerLength, _headerLength);
		if (__framing->headerLength != 0 && __framing->readDeadline == 0)
		{
			__framing->readDeadline = protocol_now() + UINT64_C(5000000000);
		}
		if (_result <= 0)
		{
			return _result;
		}
		if (__pcm != 0)
		{
			if (_framing_get(__framing->header, 4) != AUDIO_PCM_MAGIC)
			{
				return -1;
			}
			__framing->payloadLength = (size_t)_framing_get(__framing->header + 36, 4);
		}
		else
		{
			__framing->payloadLength = (size_t)_framing_get(__framing->header, 4);
			if (__framing->payloadLength == 0)
			{
				return -1;
			}
		}
		if (__framing->payloadLength > AUDIO_CONTROL_MAX)
		{
			return -1;
		}
		__framing->payload = calloc(__framing->payloadLength + 1, 1);
		if (__framing->payload == NULL)
		{
			return -1;
		}
	}
	if (__framing->payloadRead < __framing->payloadLength)
	{
		int32_t _result = _framing_read(__socket, __framing->payload, &__framing->payloadRead, __framing->payloadLength);
		if (_result <= 0)
		{
			return _result;
		}
	}
	if (__pcm != 0)
	{
		__packet->kind = (uint32_t)_framing_get(__framing->header + 4, 4);
		if (__packet->kind != AUDIO_PCM_DATA && __packet->kind != AUDIO_PCM_CLOCK)
		{
			return -1;
		}
		__packet->epoch = _framing_get(__framing->header + 8, 8);
		__packet->frame = _framing_get(__framing->header + 16, 8);
		__packet->timestamp = _framing_get(__framing->header + 24, 8);
		__packet->frames = (uint32_t)_framing_get(__framing->header + 32, 4);
		__packet->length = __framing->payloadLength;
		memcpy(__packet->data, __framing->payload, __packet->length);
	}
	else
	{
		*__message = cJSON_ParseWithLengthOpts((char *)__framing->payload, __framing->payloadLength + 1, NULL, 1);
		if (!cJSON_IsObject(*__message))
		{
			cJSON_Delete(*__message);
			*__message = NULL;
			return -1;
		}
	}
	free(__framing->payload);
	__framing->payload = NULL;
	__framing->headerLength = 0;
	__framing->payloadLength = 0;
	__framing->payloadRead = 0;
	__framing->readDeadline = 0;
	return 1;
}

static int32_t _framing_enqueue(framing_t *__framing, const uint8_t *__header, size_t __headerLength, const uint8_t *__payload, size_t __payloadLength)
{
	size_t _pending = __framing->outputLength - __framing->outputOffset;
	size_t _length = _pending + __headerLength + __payloadLength;
	size_t _limit = __framing->outputLimit;
	if (_limit == 0)
	{
		_limit = 2 * AUDIO_CONTROL_MAX;
	}
	if (_length > _limit)
	{
		return -1;
	}
	uint8_t *_output = malloc(_length);
	if (_output == NULL)
	{
		return -1;
	}
	if (_pending > 0)
	{
		memcpy(_output, __framing->output + __framing->outputOffset, _pending);
	}
	memcpy(_output + _pending, __header, __headerLength);
	if (__payloadLength > 0)
	{
		memcpy(_output + _pending + __headerLength, __payload, __payloadLength);
	}
	free(__framing->output);
	__framing->output = _output;
	__framing->outputLength = _length;
	__framing->outputOffset = 0;
	if (__framing->writeDeadline == 0)
	{
		__framing->writeDeadline = protocol_now() + UINT64_C(3000000000);
	}
	return 0;
}

int32_t framing_enqueue_json(framing_t *__framing, const cJSON *__message)
{
	char *_encoded = cJSON_PrintUnformatted(__message);
	if (_encoded == NULL)
	{
		return -1;
	}
	size_t _length = strlen(_encoded);
	int32_t _result = -1;
	if (_length > 0 && _length <= AUDIO_CONTROL_MAX)
	{
		uint8_t _header[4];
		_framing_put(_header, _length, 4);
		_result = _framing_enqueue(__framing, _header, sizeof(_header), (uint8_t *)_encoded, _length);
	}
	free(_encoded);
	return _result;
}

int32_t framing_enqueue_pcm(framing_t *__framing, const protocol_t *__packet)
{
	uint8_t _header[AUDIO_PCM_HEADER] = {0};
	_framing_put(_header, AUDIO_PCM_MAGIC, 4);
	_framing_put(_header + 4, __packet->kind, 4);
	_framing_put(_header + 8, __packet->epoch, 8);
	_framing_put(_header + 16, __packet->frame, 8);
	_framing_put(_header + 24, __packet->timestamp, 8);
	_framing_put(_header + 32, __packet->frames, 4);
	_framing_put(_header + 36, __packet->length, 4);
	return _framing_enqueue(__framing, _header, sizeof(_header), __packet->data, __packet->length);
}

int32_t framing_flush(int32_t __socket, framing_t *__framing)
{
	while (__framing->outputOffset < __framing->outputLength)
	{
		ssize_t _count = send(__socket, __framing->output + __framing->outputOffset, __framing->outputLength - __framing->outputOffset, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (_count < 0 && (errno == EAGAIN || errno == EINTR))
		{
			return 0;
		}
		if (_count <= 0)
		{
			return -1;
		}
		__framing->outputOffset += (size_t)_count;
	}
	free(__framing->output);
	__framing->output = NULL;
	__framing->outputLength = 0;
	__framing->outputOffset = 0;
	__framing->writeDeadline = 0;
	return 0;
}

uint8_t framing_expired(const framing_t *__framing)
{
	uint64_t _now = protocol_now();
	return (__framing->readDeadline != 0 && _now >= __framing->readDeadline) || (__framing->writeDeadline != 0 && _now >= __framing->writeDeadline);
}
