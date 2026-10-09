#define _GNU_SOURCE
#include "include/protocol.h"
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

uint64_t protocol_now(void)
{
	struct timespec _timestamp;
	clock_gettime(CLOCK_MONOTONIC, &_timestamp);
	return (uint64_t)_timestamp.tv_sec * UINT64_C(1000000000) + (uint64_t)_timestamp.tv_nsec;
}

static void _protocol_put(uint8_t *__data, uint64_t __value, size_t __width)
{
	for (size_t _index = 0; _index < __width; ++_index)
	{
		__data[_index] = (uint8_t)(__value >> (8 * _index));
	}
}

static uint64_t _protocol_get(const uint8_t *__data, size_t __width)
{
	uint64_t _value = 0;
	for (size_t _index = 0; _index < __width; ++_index)
	{
		_value |= (uint64_t)__data[_index] << (8 * _index);
	}
	return _value;
}

int32_t protocol_connect(const char *__name)
{
	if (__name == NULL || strlen(__name) >= sizeof(((struct sockaddr_un *)0)->sun_path) - 1)
	{
		return -1;
	}
	int32_t _socket = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (_socket < 0)
	{
		return -1;
	}
	struct sockaddr_un _address = {0};
	_address.sun_family = AF_UNIX;
	memcpy(_address.sun_path + 1, __name, strlen(__name));
	if (connect(_socket, (struct sockaddr *)&_address, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(__name))) != 0)
	{
		close(_socket);
		return -1;
	}
	return _socket;
}

int32_t protocol_transfer(int32_t __socket, void *__data, size_t __length, uint8_t __write, int32_t __timeout)
{
	uint8_t *_position = __data;
	uint64_t _deadline = protocol_now() + (uint64_t)__timeout * UINT64_C(1000000);
	while (__length > 0)
	{
		uint64_t _now = protocol_now();
		if (_now >= _deadline)
		{
			return -1;
		}
		struct pollfd _poll = {0};
		_poll.fd = __socket;
		if (__write != 0)
		{
			_poll.events = POLLOUT;
		}
		else
		{
			_poll.events = POLLIN;
		}
		int32_t _result = poll(&_poll, 1, (int32_t)((_deadline - _now) / UINT64_C(1000000)));
		if (_result < 0 && errno == EINTR)
		{
			continue;
		}
		if (_result <= 0)
		{
			return -1;
		}
		ssize_t _count;
		if (__write != 0)
		{
			_count = send(__socket, _position, __length, MSG_NOSIGNAL | MSG_DONTWAIT);
		}
		else
		{
			_count = recv(__socket, _position, __length, MSG_DONTWAIT);
		}
		if (_count < 0 && (errno == EAGAIN || errno == EINTR))
		{
			continue;
		}
		if (_count <= 0)
		{
			return -1;
		}
		_position += (size_t)_count;
		__length -= (size_t)_count;
	}
	return 0;
}

int32_t protocol_send_json(int32_t __socket, const cJSON *__message)
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
		_protocol_put(_header, _length, 4);
		if (protocol_transfer(__socket, _header, sizeof(_header), 1, 3000) == 0)
		{
			_result = protocol_transfer(__socket, _encoded, _length, 1, 3000);
		}
	}
	free(_encoded);
	return _result;
}

cJSON *protocol_receive_json(int32_t __socket, int32_t __timeout)
{
	uint8_t _header[4];
	if (protocol_transfer(__socket, _header, sizeof(_header), 0, __timeout) != 0)
	{
		return NULL;
	}
	size_t _length = (size_t)_protocol_get(_header, 4);
	if (_length == 0 || _length > AUDIO_CONTROL_MAX)
	{
		return NULL;
	}
	char *_encoded = calloc(_length + 1, 1);
	if (_encoded == NULL)
	{
		return NULL;
	}
	cJSON *_message = NULL;
	if (protocol_transfer(__socket, _encoded, _length, 0, __timeout) == 0)
	{
		_message = cJSON_ParseWithLengthOpts(_encoded, _length + 1, NULL, 1);
		if (!cJSON_IsObject(_message))
		{
			cJSON_Delete(_message);
			_message = NULL;
		}
	}
	free(_encoded);
	return _message;
}

int32_t protocol_encode_pcm_header(uint8_t *__header, const protocol_t *__packet)
{
	if (__packet->length > AUDIO_PCM_MAX)
	{
		return -1;
	}
	_protocol_put(__header, AUDIO_PCM_MAGIC, 4);
	_protocol_put(__header + 4, __packet->kind, 4);
	_protocol_put(__header + 8, __packet->epoch, 8);
	_protocol_put(__header + 16, __packet->frame, 8);
	_protocol_put(__header + 24, __packet->timestamp, 8);
	_protocol_put(__header + 32, __packet->frames, 4);
	_protocol_put(__header + 36, __packet->length, 4);
	return 0;
}

int32_t protocol_send_pcm(int32_t __socket, const protocol_t *__packet)
{
	uint8_t _header[AUDIO_PCM_HEADER];
	if (protocol_encode_pcm_header(_header, __packet) != 0 || protocol_transfer(__socket, _header, sizeof(_header), 1, 1000) != 0)
	{
		return -1;
	}
	return protocol_transfer(__socket, (void *)__packet->data, __packet->length, 1, 1000);
}

int32_t protocol_receive_pcm(int32_t __socket, protocol_t *__packet)
{
	uint8_t _header[AUDIO_PCM_HEADER];
	if (protocol_transfer(__socket, _header, sizeof(_header), 0, 1000) != 0)
	{
		return -1;
	}
	uint64_t _kind = _protocol_get(_header + 4, 4);
	uint64_t _length = _protocol_get(_header + 36, 4);
	if (_protocol_get(_header, 4) != AUDIO_PCM_MAGIC || _length > AUDIO_PCM_MAX || (_kind != AUDIO_PCM_DATA && _kind != AUDIO_PCM_CLOCK && _kind != AUDIO_PCM_CREDIT && _kind != AUDIO_PCM_PRESENTATION))
	{
		return -1;
	}
	__packet->kind = (uint32_t)_kind;
	__packet->epoch = _protocol_get(_header + 8, 8);
	__packet->frame = _protocol_get(_header + 16, 8);
	__packet->timestamp = _protocol_get(_header + 24, 8);
	__packet->frames = (uint32_t)_protocol_get(_header + 32, 4);
	__packet->length = (size_t)_length;
	return protocol_transfer(__socket, __packet->data, __packet->length, 0, 1000);
}

int32_t protocol_send_descriptor(int32_t __socket, int32_t __descriptor)
{
	char _sentinel = 'F';
	struct iovec _vector = {&_sentinel, 1};
	uint8_t _ancillary[CMSG_SPACE(sizeof(int32_t))];
	memset(_ancillary, 0, sizeof(_ancillary));
	struct msghdr _message = {0};
	_message.msg_iov = &_vector;
	_message.msg_iovlen = 1;
	_message.msg_control = _ancillary;
	_message.msg_controllen = sizeof(_ancillary);
	struct cmsghdr *_header = CMSG_FIRSTHDR(&_message);
	_header->cmsg_level = SOL_SOCKET;
	_header->cmsg_type = SCM_RIGHTS;
	_header->cmsg_len = CMSG_LEN(sizeof(__descriptor));
	memcpy(CMSG_DATA(_header), &__descriptor, sizeof(__descriptor));
	ssize_t _result = sendmsg(__socket, &_message, MSG_NOSIGNAL | MSG_DONTWAIT);
	if (_result == 1)
	{
		return 1;
	}
	if (_result < 0 && (errno == EAGAIN || errno == EINTR))
	{
		return 0;
	}
	return -1;
}

int32_t protocol_receive_descriptor(int32_t __socket)
{
	struct pollfd _poll = {__socket, POLLIN, 0};
	if (poll(&_poll, 1, 3000) <= 0)
	{
		return -1;
	}
	char _sentinel = 0;
	struct iovec _vector = {&_sentinel, 1};
	uint8_t _ancillary[CMSG_SPACE(sizeof(int32_t) * 8)];
	struct msghdr _message = {0};
	_message.msg_iov = &_vector;
	_message.msg_iovlen = 1;
	_message.msg_control = _ancillary;
	_message.msg_controllen = sizeof(_ancillary);
	ssize_t _received = recvmsg(__socket, &_message, MSG_CMSG_CLOEXEC);
	int32_t _descriptor = -1;
	uint32_t _count = 0;
	for (struct cmsghdr *_header = CMSG_FIRSTHDR(&_message); _header != NULL; _header = CMSG_NXTHDR(&_message, _header))
	{
		if (_header->cmsg_level == SOL_SOCKET && _header->cmsg_type == SCM_RIGHTS)
		{
			size_t _length = (_header->cmsg_len - CMSG_LEN(0)) / sizeof(int32_t);
			for (size_t _index = 0; _index < _length; ++_index)
			{
				int32_t _candidate;
				memcpy(&_candidate, CMSG_DATA(_header) + _index * sizeof(_candidate), sizeof(_candidate));
				if (_count == 0)
				{
					_descriptor = _candidate;
				}
				else
				{
					close(_candidate);
				}
				++_count;
			}
		}
	}
	if (_received != 1 || _sentinel != 'F' || _count != 1 || (_message.msg_flags & MSG_CTRUNC) != 0)
	{
		if (_descriptor >= 0)
		{
			close(_descriptor);
		}
		return -1;
	}
	return _descriptor;
}

const char *protocol_string(const cJSON *__message, const char *__field)
{
	const cJSON *_value = cJSON_GetObjectItemCaseSensitive(__message, __field);
	if (!cJSON_IsString(_value))
	{
		return "";
	}
	return _value->valuestring;
}

uint64_t protocol_number(const cJSON *__message, const char *__field, uint64_t __default)
{
	const cJSON *_value = cJSON_GetObjectItemCaseSensitive(__message, __field);
	if (!cJSON_IsNumber(_value) || !isfinite(_value->valuedouble) || _value->valuedouble < 0 || _value->valuedouble > 9007199254740991.0 || floor(_value->valuedouble) != _value->valuedouble)
	{
		return __default;
	}
	return (uint64_t)_value->valuedouble;
}

uint8_t protocol_boolean(const cJSON *__message, const char *__field)
{
	return (uint8_t)cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(__message, __field));
}

void protocol_set_number(cJSON *__message, const char *__field, uint64_t __value)
{
	cJSON_DeleteItemFromObjectCaseSensitive(__message, __field);
	cJSON_AddNumberToObject(__message, __field, (double)__value);
}

void protocol_set_string(cJSON *__message, const char *__field, const char *__value)
{
	cJSON_DeleteItemFromObjectCaseSensitive(__message, __field);
	cJSON_AddStringToObject(__message, __field, __value);
}

void protocol_set_boolean(cJSON *__message, const char *__field, uint8_t __value)
{
	cJSON_DeleteItemFromObjectCaseSensitive(__message, __field);
	cJSON_AddBoolToObject(__message, __field, __value);
}

cJSON *protocol_hello(const char *__role)
{
	cJSON *_message = cJSON_CreateObject();
	protocol_set_string(_message, "op", "hello");
	protocol_set_string(_message, "role", __role);
	protocol_set_number(_message, "version", AUDIO_VERSION);
	return _message;
}

cJSON *protocol_reply(uint64_t __id, uint8_t __ok, const char *__error)
{
	cJSON *_message = cJSON_CreateObject();
	protocol_set_string(_message, "op", "reply");
	protocol_set_number(_message, "id", __id);
	protocol_set_boolean(_message, "ok", __ok);
	if (__error != NULL)
	{
		protocol_set_string(_message, "error", __error);
	}
	return _message;
}
