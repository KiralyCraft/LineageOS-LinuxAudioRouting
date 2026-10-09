#include "include/control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int __argc, char **__argv)
{
	const char *_socketName = AUDIO_SOCKET;
	int32_t _index = 1;
	if (__argc > 3 && strcmp(__argv[1], "--socket") == 0)
	{
		_socketName = __argv[2];
		_index = 3;
	}
	const char *_operation = "status";
	if (_index < __argc)
	{
		_operation = __argv[_index++];
	}
	if (strcmp(_operation, "--help") == 0)
	{
		puts("linux-audioctl [status|list|profile ENDPOINT stereo|headset]");
		return 0;
	}
	control_t _control;
	if (control_start(&_control, _socketName, NULL, NULL) != 0)
	{
		fputs("Audio broker unavailable; install/start the Linux Audio module\n", stderr);
		control_stop(&_control);
		return 1;
	}
	cJSON *_request = cJSON_CreateObject();
	protocol_set_string(_request, "op", _operation);
	if (strcmp(_operation, "profile") == 0)
	{
		if (__argc - _index != 2)
		{
			cJSON_Delete(_request);
			control_stop(&_control);
			return 2;
		}
		protocol_set_string(_request, "endpoint", __argv[_index]);
		protocol_set_string(_request, "profile", __argv[_index + 1]);
	}
	cJSON *_reply = control_request(&_control, _request);
	cJSON_Delete(_request);
	int32_t _result = 1;
	if (_reply != NULL)
	{
		char *_text = cJSON_Print(_reply);
		if (_text != NULL)
		{
			puts(_text);
			free(_text);
		}
		if (protocol_boolean(_reply, "ok") != 0)
		{
			_result = 0;
		}
	}
	cJSON_Delete(_reply);
	control_stop(&_control);
	return _result;
}
