#define _GNU_SOURCE
#include "../include/transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int __argc, char **__argv)
{
	if (__argc != 4)
	{
		return 2;
	}
	control_t _control;
	if (control_start(&_control, __argv[1], NULL, NULL) != 0)
	{
		control_stop(&_control);
		return 1;
	}
	cJSON *_request = cJSON_CreateObject();
	protocol_set_string(_request, "op", "list");
	cJSON *_inventory = control_request(&_control, _request);
	cJSON_Delete(_request);
	uint64_t _generation = protocol_number(_inventory, "generation", 0);
	cJSON_Delete(_inventory);
	uint8_t _headset = strcmp(__argv[2], "capture-headset") == 0;
	uint8_t _capture = strcmp(__argv[2], "capture") == 0 || _headset != 0;
	uint8_t _expectIncomplete = strcmp(__argv[2], "playback-incomplete") == 0;
	cJSON *_device = cJSON_CreateObject();
	protocol_set_string(_device, "key", __argv[3]);
	protocol_set_string(_device, "direction", "output");
	if (_capture != 0)
	{
		protocol_set_string(_device, "direction", "input");
	}
	protocol_set_string(_device, "format", "f32le");
	protocol_set_number(_device, "rate", 48000);
	if (_headset != 0)
	{
		protocol_set_number(_device, "rate", 16000);
		protocol_set_string(_device, "format", "s16le");
		protocol_set_string(_device, "profile", "headset");
	}
	protocol_set_number(_device, "channels", 1);
	transport_t _transport;
	int32_t _result = transport_start(&_transport, &_control, _device, _generation, NULL, NULL);
	cJSON_Delete(_device);
	uint8_t _samples[1024];
	size_t _offset = 0;
	uint64_t _deadline = protocol_now() + UINT64_C(3000000000);
	while (_result == 0 && _offset < 20000 && protocol_now() < _deadline && transport_live(&_transport) != 0)
	{
		size_t _length = sizeof(_samples);
		if (_length > 20000 - _offset)
		{
			_length = 20000 - _offset;
		}
		if (_capture != 0)
		{
			if (__atomic_load_n(&_transport.ready, __ATOMIC_ACQUIRE) == 0 || ring_available(&_transport.ring) < _length)
			{
				struct timespec _wait = {0, 1000000};
				nanosleep(&_wait, NULL);
				continue;
			}
			if (ring_read(&_transport.ring, _samples, _length) != _length)
			{
				_result = -1;
				break;
			}
		}
		for (size_t _index = 0; _index < _length; ++_index)
		{
			uint8_t _expected = (uint8_t)(((_offset + _index) * 31) ^ ((_offset + _index) >> 8));
			if (_capture != 0 && _samples[_index] != _expected)
			{
				_result = -1;
				break;
			}
			_samples[_index] = _expected;
		}
		if (_capture == 0)
		{
			if (ring_write(&_transport.ring, _samples, _length) != _length)
			{
				_result = -1;
				break;
			}
			transport_wake(&_transport);
			struct timespec _wait = {0, 5300000};
			nanosleep(&_wait, NULL);
		}
		_offset += _length;
	}
	if (_offset != 20000)
	{
		_result = -1;
	}
	if (_capture == 0)
	{
		uint8_t _incomplete = transport_drain(&_transport) != 0;
		if (_incomplete != _expectIncomplete)
		{
			_result = -1;
		}
	}
	transport_stop(&_transport);
	control_stop(&_control);
	if (_result != 0)
	{
		fprintf(stderr, "Raw PCM transport fixture failed at byte %zu\n", _offset);
		return 1;
	}
	puts("PASS production PCM transport bit exact including final partial packet");
	return 0;
}
