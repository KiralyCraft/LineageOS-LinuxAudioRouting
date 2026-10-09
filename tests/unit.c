#define _GNU_SOURCE
#include "../include/framing.h"
#include "../include/ring.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void _unit_require(uint8_t __condition, const char *__description)
{
	if (__condition == 0)
	{
		fprintf(stderr, "FAIL %s\n", __description);
		exit(1);
	}
}

static void _unit_ring(void)
{
	ring_t _ring;
	_unit_require(ring_init(&_ring, 32) == 0, "ring allocation");
	uint8_t _input[32];
	for (size_t _index = 0; _index < sizeof(_input); ++_index)
	{
		_input[_index] = (uint8_t)(_index * 7);
	}
	uint8_t _output[32];
	for (uint32_t _round = 0; _round < 10000; ++_round)
	{
		_unit_require(ring_write(&_ring, _input, 27) == 27, "wrapped producer");
		_unit_require(ring_write(&_ring, _input, 6) == 0, "overflow leaves existing data intact");
		_unit_require(ring_available(&_ring) == 27, "overflow does not advance cursor");
		_unit_require(ring_read(&_ring, _output, 27) == 27, "wrapped consumer");
		_unit_require(memcmp(_input, _output, 27) == 0, "bit exact ring");
	}
	_unit_require(ring_write(&_ring, _input, 32) == 32, "full queue");
	ring_discard(&_ring);
	_unit_require(ring_read(&_ring, _output, 32) == 0, "explicit retired generation discarded");
	ring_destroy(&_ring);
	puts("PASS bounded ring, exact wraparound and explicit retirement");
}

static void _unit_framing(void)
{
	int32_t _pair[2];
	_unit_require(socketpair(AF_UNIX, SOCK_STREAM, 0, _pair) == 0, "framing socketpair");
	framing_t _framing = {0};
	cJSON *_message = NULL;
	uint8_t _bytes[] = {2, 0, 0, 0, '{', '}'};
	for (size_t _index = 0; _index < sizeof(_bytes); ++_index)
	{
		_unit_require(send(_pair[0], &_bytes[_index], 1, MSG_NOSIGNAL) == 1, "fragment send");
		int32_t _received = framing_receive(_pair[1], &_framing, 0, &_message, NULL);
		if (_index == sizeof(_bytes) - 1)
		{
			_unit_require(_received == 1 && cJSON_IsObject(_message), "fragmented JSON accepted once complete");
		}
		else
		{
			_unit_require(_received == 0 && _message == NULL, "partial frame not processed");
		}
	}
	cJSON_Delete(_message);
	_framing.readDeadline = protocol_now() - 1;
	_unit_require(framing_expired(&_framing), "watchdog expiry never grants readiness");
	framing_clear(&_framing);
	uint8_t _oversized[] = {1, 0, 1, 0};
	_unit_require(send(_pair[0], _oversized, 4, MSG_NOSIGNAL) == 4, "oversize header send");
	_unit_require(framing_receive(_pair[1], &_framing, 0, &_message, NULL) == -1, "oversized control rejected");
	framing_clear(&_framing);
	close(_pair[0]);
	close(_pair[1]);
	puts("PASS fragmented framing, bounds and partial-message deadline");
}

static void _unit_descriptor(void)
{
	int32_t _control[2];
	int32_t _pcm[2];
	_unit_require(socketpair(AF_UNIX, SOCK_STREAM, 0, _control) == 0, "control pair");
	_unit_require(socketpair(AF_UNIX, SOCK_STREAM, 0, _pcm) == 0, "PCM pair");
	_unit_require(protocol_send_descriptor(_control[0], _pcm[0]) == 1, "descriptor transfer");
	int32_t _received = protocol_receive_descriptor(_control[1]);
	_unit_require(_received >= 0 && (fcntl(_received, F_GETFD) & FD_CLOEXEC) != 0, "received FD CLOEXEC");
	close(_pcm[0]);
	protocol_t *_input = calloc(1, sizeof(*_input));
	protocol_t *_output = calloc(1, sizeof(*_output));
	_unit_require(_input != NULL && _output != NULL, "PCM fixture allocation");
	_input->kind = AUDIO_PCM_DATA;
	_input->epoch = 17;
	_input->frame = 9876;
	_input->timestamp = protocol_now();
	_input->frames = 480;
	_input->length = 3840;
	for (size_t _index = 0; _index < _input->length; ++_index)
	{
		_input->data[_index] = (uint8_t)((_index * 31) ^ (_index >> 8));
	}
	_unit_require(protocol_send_pcm(_received, _input) == 0, "raw PCM send");
	_unit_require(protocol_receive_pcm(_pcm[1], _output) == 0, "raw PCM receive");
	_unit_require(_output->epoch == 17 && _output->frame == 9876 && _output->length == 3840 && memcmp(_input->data, _output->data, 3840) == 0, "raw PCM bit exact including arbitrary float bits");
	free(_input);
	free(_output);
	close(_received);
	close(_pcm[1]);
	close(_control[0]);
	close(_control[1]);
	puts("PASS SCM_RIGHTS ownership and raw PCM roundtrip");
}

int main(void)
{
	_unit_ring();
	_unit_framing();
	_unit_descriptor();
	return 0;
}
