#define _GNU_SOURCE
#include "include/control.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

uint8_t control_live(const control_t *__control)
{
	return __atomic_load_n(&__control->live, __ATOMIC_ACQUIRE) != 0;
}

static void *_control_reader(void *__context)
{
	control_t *_control = __context;
	while (control_live(_control) != 0)
	{
		struct pollfd _poll = {_control->socket, POLLIN, 0};
		int32_t _result = poll(&_poll, 1, 250);
		if (_result < 0 && errno == EINTR)
		{
			continue;
		}
		if (_result < 0 || (_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
		{
			break;
		}
		if ((_poll.revents & POLLIN) == 0)
		{
			continue;
		}
		cJSON *_message = protocol_receive_json(_control->socket, 3000);
		if (_message == NULL)
		{
			break;
		}
		if (strcmp(protocol_string(_message, "op"), "reply") == 0)
		{
			pthread_mutex_lock(&_control->stateMutex);
			if (_control->waiting != 0 && protocol_number(_message, "id", 0) == _control->waiting)
			{
				cJSON_Delete(_control->reply);
				_control->reply = _message;
				_message = NULL;
				pthread_cond_broadcast(&_control->responseReady);
			}
			pthread_mutex_unlock(&_control->stateMutex);
		}
		else if (_control->event != NULL)
		{
			_control->event(_control->context, _message);
		}
		cJSON_Delete(_message);
	}
	__atomic_store_n(&_control->live, 0, __ATOMIC_RELEASE);
	pthread_mutex_lock(&_control->stateMutex);
	pthread_cond_broadcast(&_control->responseReady);
	pthread_mutex_unlock(&_control->stateMutex);
	if (_control->event != NULL)
	{
		cJSON *_message = cJSON_CreateObject();
		protocol_set_string(_message, "op", "disconnected");
		_control->event(_control->context, _message);
		cJSON_Delete(_message);
	}
	return NULL;
}

int32_t control_start(control_t *__control, const char *__socketName, control_event_t __event, void *__context)
{
	memset(__control, 0, sizeof(*__control));
	__control->socket = -1;
	__control->event = __event;
	__control->context = __context;
	snprintf(__control->socketName, sizeof(__control->socketName), "%s", __socketName);
	pthread_mutex_init(&__control->stateMutex, NULL);
	pthread_mutex_init(&__control->requestMutex, NULL);
	pthread_condattr_t _attributes;
	pthread_condattr_init(&_attributes);
	pthread_condattr_setclock(&_attributes, CLOCK_MONOTONIC);
	pthread_cond_init(&__control->responseReady, &_attributes);
	pthread_condattr_destroy(&_attributes);
	__control->socket = protocol_connect(__socketName);
	if (__control->socket < 0)
	{
		return -1;
	}
	cJSON *_hello = protocol_hello("linux");
	int32_t _result = protocol_send_json(__control->socket, _hello);
	cJSON_Delete(_hello);
	if (_result != 0)
	{
		return -1;
	}
	cJSON *_reply = protocol_receive_json(__control->socket, 3000);
	uint8_t _ready = strcmp(protocol_string(_reply, "op"), "ready") == 0;
	cJSON_Delete(_reply);
	if (_ready == 0)
	{
		return -1;
	}
	__atomic_store_n(&__control->live, 1, __ATOMIC_RELEASE);
	if (pthread_create(&__control->reader, NULL, _control_reader, __control) != 0)
	{
		__atomic_store_n(&__control->live, 0, __ATOMIC_RELEASE);
		return -1;
	}
	__control->readerStarted = 1;
	return 0;
}

void control_stop(control_t *__control)
{
	__atomic_store_n(&__control->live, 0, __ATOMIC_RELEASE);
	if (__control->socket >= 0)
	{
		shutdown(__control->socket, SHUT_RDWR);
	}
	if (__control->readerStarted != 0)
	{
		pthread_join(__control->reader, NULL);
	}
	if (__control->socket >= 0)
	{
		close(__control->socket);
	}
	cJSON_Delete(__control->reply);
	pthread_cond_destroy(&__control->responseReady);
	pthread_mutex_destroy(&__control->requestMutex);
	pthread_mutex_destroy(&__control->stateMutex);
}

cJSON *control_request(control_t *__control, cJSON *__message)
{
	pthread_mutex_lock(&__control->requestMutex);
	pthread_mutex_lock(&__control->stateMutex);
	__control->waiting = ++__control->serial;
	protocol_set_number(__message, "id", __control->waiting);
	cJSON_Delete(__control->reply);
	__control->reply = NULL;
	pthread_mutex_unlock(&__control->stateMutex);
	int32_t _sent = protocol_send_json(__control->socket, __message);
	pthread_mutex_lock(&__control->stateMutex);
	struct timespec _deadline;
	clock_gettime(CLOCK_MONOTONIC, &_deadline);
	_deadline.tv_sec += 7;
	while (_sent == 0 && control_live(__control) != 0 && __control->reply == NULL)
	{
		if (pthread_cond_timedwait(&__control->responseReady, &__control->stateMutex, &_deadline) == ETIMEDOUT)
		{
			break;
		}
	}
	cJSON *_reply = __control->reply;
	__control->reply = NULL;
	__control->waiting = 0;
	pthread_mutex_unlock(&__control->stateMutex);
	pthread_mutex_unlock(&__control->requestMutex);
	return _reply;
}

int32_t control_attach(control_t *__control, uint64_t __stream, const char *__token)
{
	int32_t _socket = protocol_connect(__control->socketName);
	if (_socket < 0)
	{
		return -1;
	}
	cJSON *_hello = protocol_hello("data");
	protocol_set_string(_hello, "side", "linux");
	protocol_set_number(_hello, "stream", __stream);
	protocol_set_string(_hello, "token", __token);
	int32_t _result = protocol_send_json(_socket, _hello);
	cJSON_Delete(_hello);
	cJSON *_reply = NULL;
	if (_result == 0)
	{
		_reply = protocol_receive_json(_socket, 3000);
	}
	uint8_t _ready = strcmp(protocol_string(_reply, "op"), "ready") == 0;
	cJSON_Delete(_reply);
	if (_ready == 0)
	{
		close(_socket);
		return -1;
	}
	int32_t _descriptor = protocol_receive_descriptor(_socket);
	close(_socket);
	return _descriptor;
}
