#define _GNU_SOURCE
#include "include/framing.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define BROKER_PEERS 64
#define BROKER_STREAMS 16
#define BROKER_PENDING 32
#define BROKER_NEW 0
#define BROKER_LINUX 1
#define BROKER_HELPER 2
#define BROKER_DATA 3
#define BROKER_TRANSFERRED 4

typedef struct
{
	int32_t socket;
	uint32_t uid;
	uint32_t role;
	uint64_t serial;
	uint64_t handshakeDeadline;
	uint64_t transferStream;
	int32_t descriptor;
	uint8_t androidSide;
	framing_t framing;
} broker_peer_t;

typedef struct
{
	uint8_t live;
	uint64_t id;
	uint64_t epoch;
	uint64_t owner;
	int32_t linuxDescriptor;
	int32_t androidDescriptor;
	uint8_t linuxTransferred;
	uint8_t androidTransferred;
	uint32_t rate;
	uint32_t channels;
	uint8_t capture;
	uint64_t attachDeadline;
	char token[33];
} broker_stream_t;

typedef struct
{
	uint64_t forwarded;
	uint64_t original;
	uint64_t owner;
	uint64_t stream;
	uint64_t deadline;
} broker_pending_t;

typedef struct
{
	int32_t listener;
	int32_t helper;
	uint32_t linuxUid;
	uint8_t isolatedTest;
	uint8_t suspended;
	uint64_t serial;
	uint64_t generation;
	char socketName[100];
	char reason[128];
	cJSON *devices;
	broker_peer_t peers[BROKER_PEERS];
	broker_stream_t streams[BROKER_STREAMS];
	broker_pending_t pending[BROKER_PENDING];
} broker_t;

static volatile sig_atomic_t _broker_quit;

static void _broker_signal(int __signal)
{
	(void)__signal;
	_broker_quit = 1;
}

static int32_t _broker_send(broker_t *__broker, int32_t __peer, const cJSON *__message)
{
	if (__peer < 0 || __peer >= BROKER_PEERS || __broker->peers[__peer].socket < 0)
	{
		return -1;
	}
	return framing_enqueue_json(&__broker->peers[__peer].framing, __message);
}

static int32_t _broker_owner(broker_t *__broker, uint64_t __serial)
{
	for (int32_t _index = 0; _index < BROKER_PEERS; ++_index)
	{
		if (__broker->peers[_index].socket >= 0 && __broker->peers[_index].serial == __serial)
		{
			return _index;
		}
	}
	return -1;
}

static uint32_t _broker_app_uid(const broker_t *__broker)
{
	if (__broker->isolatedTest != 0)
	{
		return __broker->linuxUid;
	}
	FILE *_packages = fopen("/data/system/packages.list", "re");
	if (_packages == NULL)
	{
		return UINT32_MAX;
	}
	char _line[2048];
	uint32_t _result = UINT32_MAX;
	while (fgets(_line, sizeof(_line), _packages) != NULL)
	{
		char _package[256];
		uint32_t _uid;
		if (sscanf(_line, "%255s %" SCNu32, _package, &_uid) == 2 && strcmp(_package, "dev.kiraly.linuxaudio") == 0)
		{
			_result = (uint32_t)_uid;
			break;
		}
	}
	fclose(_packages);
	return _result;
}

static cJSON *_broker_inventory(const broker_t *__broker)
{
	cJSON *_message = cJSON_CreateObject();
	protocol_set_string(_message, "op", "inventory");
	protocol_set_number(_message, "generation", __broker->generation);
	protocol_set_boolean(_message, "helper", __broker->helper >= 0);
	protocol_set_boolean(_message, "suspended", __broker->suspended);
	protocol_set_string(_message, "reason", __broker->reason);
	cJSON_AddItemToObject(_message, "devices", cJSON_Duplicate(__broker->devices, 1));
	return _message;
}

static void _broker_broadcast(broker_t *__broker, const cJSON *__message)
{
	for (int32_t _index = 0; _index < BROKER_PEERS; ++_index)
	{
		if (__broker->peers[_index].socket >= 0 && __broker->peers[_index].role == BROKER_LINUX)
		{
			if (_broker_send(__broker, _index, __message) != 0)
			{
				shutdown(__broker->peers[_index].socket, SHUT_RDWR);
			}
		}
	}
}

static void _broker_announce(broker_t *__broker)
{
	cJSON *_message = _broker_inventory(__broker);
	_broker_broadcast(__broker, _message);
	cJSON_Delete(_message);
}

static void _broker_stream_close(broker_t *__broker, broker_stream_t *__stream)
{
	if (__stream->live == 0)
	{
		return;
	}
	__stream->live = 0;
	for (int32_t _index = 0; _index < BROKER_PEERS; ++_index)
	{
		broker_peer_t *_peer = &__broker->peers[_index];
		if (_peer->role == BROKER_DATA && _peer->transferStream == __stream->id && _peer->socket >= 0)
		{
			_peer->descriptor = -1;
			shutdown(_peer->socket, SHUT_RDWR);
		}
	}
	int32_t _descriptors[2] = {__stream->linuxDescriptor, __stream->androidDescriptor};
	for (size_t _index = 0; _index < 2; ++_index)
	{
		int32_t _descriptor = _descriptors[_index];
		if (_descriptor >= 0)
		{
			shutdown(_descriptor, SHUT_RDWR);
			close(_descriptor);
		}
	}
	__stream->linuxDescriptor = -1;
	__stream->androidDescriptor = -1;
	cJSON *_message = cJSON_CreateObject();
	protocol_set_string(_message, "op", "close");
	protocol_set_number(_message, "stream", __stream->id);
	_broker_send(__broker, __broker->helper, _message);
	protocol_set_string(_message, "op", "stream_closed");
	protocol_set_number(_message, "epoch", __stream->epoch);
	_broker_broadcast(__broker, _message);
	cJSON_Delete(_message);
}

static void _broker_peer_close(broker_t *__broker, int32_t __index)
{
	broker_peer_t *_peer = &__broker->peers[__index];
	if (_peer->socket < 0)
	{
		return;
	}
	uint8_t _helperLost = __broker->helper == __index;
	close(_peer->socket);
	_peer->socket = -1;
	framing_clear(&_peer->framing);
	if (_helperLost != 0)
	{
		__broker->helper = -1;
		cJSON_Delete(__broker->devices);
		__broker->devices = cJSON_CreateArray();
		__broker->suspended = 0;
		++__broker->generation;
		snprintf(__broker->reason, sizeof(__broker->reason), "Start the Android Linux Audio app");
	}
	for (size_t _index = 0; _index < BROKER_STREAMS; ++_index)
	{
		broker_stream_t *_stream = &__broker->streams[_index];
		if (_stream->live != 0 && (_helperLost != 0 || _stream->owner == _peer->serial || (_peer->role == BROKER_DATA && _stream->id == _peer->transferStream)))
		{
			_broker_stream_close(__broker, _stream);
		}
	}
	for (size_t _index = 0; _index < BROKER_PENDING; ++_index)
	{
		broker_pending_t *_pending = &__broker->pending[_index];
		if (_pending->forwarded != 0 && (_pending->owner == _peer->serial || _helperLost != 0))
		{
			cJSON *_message = protocol_reply(_pending->original, 0, "Audio session disconnected");
			_broker_send(__broker, _broker_owner(__broker, _pending->owner), _message);
			cJSON_Delete(_message);
			_pending->forwarded = 0;
		}
	}
	if (_helperLost != 0)
	{
		_broker_announce(__broker);
	}
}

static broker_stream_t *_broker_stream_find(broker_t *__broker, uint64_t __id)
{
	for (size_t _index = 0; _index < BROKER_STREAMS; ++_index)
	{
		if (__broker->streams[_index].live != 0 && __broker->streams[_index].id == __id)
		{
			return &__broker->streams[_index];
		}
	}
	return NULL;
}

static int32_t _broker_attach(broker_t *__broker, int32_t __index, const cJSON *__message)
{
	broker_stream_t *_stream = _broker_stream_find(__broker, protocol_number(__message, "stream", 0));
	broker_peer_t *_peer = &__broker->peers[__index];
	if (_stream == NULL || strcmp(protocol_string(__message, "token"), _stream->token) != 0)
	{
		return -1;
	}
	const char *_side = protocol_string(__message, "side");
	if (strcmp(_side, "linux") == 0 && (_peer->uid == __broker->linuxUid || _peer->uid == 0) && _stream->linuxDescriptor >= 0 && _stream->linuxTransferred == 0)
	{
		_peer->descriptor = _stream->linuxDescriptor;
		_peer->androidSide = 0;
		_stream->linuxTransferred = 1;
	}
	else if (strcmp(_side, "android") == 0 && _peer->uid == _broker_app_uid(__broker) && _stream->androidDescriptor >= 0 && _stream->androidTransferred == 0)
	{
		_peer->descriptor = _stream->androidDescriptor;
		_peer->androidSide = 1;
		_stream->androidTransferred = 1;
	}
	else
	{
		return -1;
	}
	_peer->role = BROKER_DATA;
	_peer->transferStream = _stream->id;
	_peer->handshakeDeadline = 0;
	cJSON *_ready = cJSON_CreateObject();
	protocol_set_string(_ready, "op", "ready");
	protocol_set_number(_ready, "version", AUDIO_VERSION);
	int32_t _result = _broker_send(__broker, __index, _ready);
	cJSON_Delete(_ready);
	return _result;
}

static int32_t _broker_hello(broker_t *__broker, int32_t __index, const cJSON *__message)
{
	if (strcmp(protocol_string(__message, "op"), "hello") != 0 || protocol_number(__message, "version", 0) != AUDIO_VERSION)
	{
		return -1;
	}
	const char *_role = protocol_string(__message, "role");
	broker_peer_t *_peer = &__broker->peers[__index];
	if (strcmp(_role, "data") == 0)
	{
		return _broker_attach(__broker, __index, __message);
	}
	if (strcmp(_role, "helper") == 0 && _peer->uid == _broker_app_uid(__broker) && __broker->helper < 0)
	{
		__broker->helper = __index;
		_peer->role = BROKER_HELPER;
	}
	else if (strcmp(_role, "linux") == 0 && (_peer->uid == __broker->linuxUid || _peer->uid == 0))
	{
		_peer->role = BROKER_LINUX;
	}
	else
	{
		return -1;
	}
	_peer->handshakeDeadline = 0;
	cJSON *_ready = cJSON_CreateObject();
	protocol_set_string(_ready, "op", "ready");
	protocol_set_number(_ready, "version", AUDIO_VERSION);
	int32_t _result = _broker_send(__broker, __index, _ready);
	cJSON_Delete(_ready);
	if (_peer->role == BROKER_LINUX)
	{
		cJSON *_inventory = _broker_inventory(__broker);
		_result |= _broker_send(__broker, __index, _inventory);
		cJSON_Delete(_inventory);
	}
	return _result;
}

static int32_t _broker_helper_message(broker_t *__broker, const cJSON *__message)
{
	const char *_operation = protocol_string(__message, "op");
	if (strcmp(_operation, "route") == 0)
	{
		broker_stream_t *_stream = _broker_stream_find(__broker, protocol_number(__message, "stream", 0));
		if (_stream != NULL && _stream->epoch == protocol_number(__message, "epoch", 0) && protocol_boolean(__message, "verified"))
		{
			_broker_broadcast(__broker, __message);
		}
	}
	else if (strcmp(_operation, "inventory") == 0)
	{
		const cJSON *_devices = cJSON_GetObjectItemCaseSensitive(__message, "devices");
		if (!cJSON_IsArray(_devices) || cJSON_GetArraySize(_devices) > 64)
		{
			return -1;
		}
		if (cJSON_Compare(__broker->devices, _devices, 1) && __broker->suspended == protocol_boolean(__message, "suspended") && strcmp(__broker->reason, protocol_string(__message, "reason")) == 0)
		{
			return 0;
		}
		cJSON *_copy = cJSON_Duplicate(_devices, 1);
		if (_copy == NULL)
		{
			return -1;
		}
		cJSON_Delete(__broker->devices);
		__broker->devices = _copy;
		__broker->suspended = protocol_boolean(__message, "suspended");
		snprintf(__broker->reason, sizeof(__broker->reason), "%s", protocol_string(__message, "reason"));
		++__broker->generation;
		_broker_announce(__broker);
	}
	else if (strcmp(_operation, "suspend") == 0)
	{
		__broker->suspended = 1;
		snprintf(__broker->reason, sizeof(__broker->reason), "%s", protocol_string(__message, "reason"));
		_broker_announce(__broker);
		for (size_t _index = 0; _index < BROKER_STREAMS; ++_index)
		{
			_broker_stream_close(__broker, &__broker->streams[_index]);
		}
	}
	else if (strcmp(_operation, "stream_closed") == 0)
	{
		broker_stream_t *_stream = _broker_stream_find(__broker, protocol_number(__message, "stream", 0));
		if (_stream != NULL && _stream->epoch == protocol_number(__message, "epoch", 0))
		{
			_broker_broadcast(__broker, __message);
			_broker_stream_close(__broker, _stream);
		}
	}
	else if (strcmp(_operation, "reply") == 0)
	{
		for (size_t _index = 0; _index < BROKER_PENDING; ++_index)
		{
			broker_pending_t *_pending = &__broker->pending[_index];
			if (_pending->forwarded == 0 || _pending->forwarded != protocol_number(__message, "id", 0))
			{
				continue;
			}
			cJSON *_reply = cJSON_Duplicate(__message, 1);
			protocol_set_number(_reply, "id", _pending->original);
			_broker_send(__broker, _broker_owner(__broker, _pending->owner), _reply);
			broker_stream_t *_stream = _broker_stream_find(__broker, _pending->stream);
			if (_stream != NULL && protocol_boolean(__message, "ok") == 0)
			{
				_broker_stream_close(__broker, _stream);
			}
			cJSON_Delete(_reply);
			_pending->forwarded = 0;
			break;
		}
	}
	return 0;
}

static int32_t _broker_token(char *__token)
{
	uint8_t _bytes[16];
	if (getrandom(_bytes, sizeof(_bytes), 0) != (ssize_t)sizeof(_bytes))
	{
		return -1;
	}
	for (size_t _index = 0; _index < sizeof(_bytes); ++_index)
	{
		snprintf(__token + 2 * _index, 3, "%02x", _bytes[_index]);
	}
	return 0;
}

static const char *_broker_forward(broker_t *__broker, int32_t __index, cJSON *__message)
{
	if (__broker->helper < 0)
	{
		return "Start the Android Linux Audio app";
	}
	if (__broker->suspended != 0)
	{
		return "Android audio is suspended for a call or focus loss";
	}
	broker_pending_t *_pending = NULL;
	for (size_t _index = 0; _index < BROKER_PENDING; ++_index)
	{
		if (__broker->pending[_index].forwarded == 0)
		{
			_pending = &__broker->pending[_index];
			break;
		}
	}
	if (_pending == NULL)
	{
		return "Request limit reached";
	}
	broker_stream_t *_stream = NULL;
	if (strcmp(protocol_string(__message, "op"), "open") == 0)
	{
		if (protocol_number(__message, "generation", 0) != __broker->generation)
		{
			return "Stale device inventory";
		}
		const cJSON *_device = NULL;
		const cJSON *_candidate;
		cJSON_ArrayForEach(_candidate, __broker->devices)
		{
			if (strcmp(protocol_string(_candidate, "key"), protocol_string(__message, "endpoint")) == 0 && protocol_boolean(_candidate, "available") != 0)
			{
				_device = _candidate;
				break;
			}
		}
		if (_device == NULL)
		{
			return "Selected endpoint unavailable";
		}
		for (size_t _index = 0; _index < BROKER_STREAMS; ++_index)
		{
			if (__broker->streams[_index].live == 0)
			{
				_stream = &__broker->streams[_index];
				break;
			}
		}
		if (_stream == NULL)
		{
			return "Stream limit reached";
		}
		memset(_stream, 0, sizeof(*_stream));
		_stream->linuxDescriptor = -1;
		_stream->androidDescriptor = -1;
		_stream->rate = (uint32_t)protocol_number(_device, "rate", 48000);
		_stream->channels = (uint32_t)protocol_number(_device, "channels", 2);
		if (_stream->rate < 8000 || _stream->rate > 96000 || _stream->channels == 0 || _stream->channels > 2 || _broker_token(_stream->token) != 0)
		{
			return "Invalid endpoint sample specification";
		}
		_stream->capture = strcmp(protocol_string(_device, "direction"), "input") == 0;
		int32_t _pair[2];
		if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, _pair) != 0)
		{
			return "Cannot allocate direct PCM channel";
		}
		_stream->linuxDescriptor = _pair[0];
		_stream->androidDescriptor = _pair[1];
		uint32_t _sampleBytes = 4;
		if (strcmp(protocol_string(_device, "format"), "s16le") == 0)
		{
			_sampleBytes = 2;
		}
		int32_t _bufferSize = (int32_t)((size_t)_stream->rate * _stream->channels * _sampleBytes * 20 / 1000);
		setsockopt(_pair[0], SOL_SOCKET, SO_SNDBUF, &_bufferSize, sizeof(_bufferSize));
		setsockopt(_pair[1], SOL_SOCKET, SO_SNDBUF, &_bufferSize, sizeof(_bufferSize));
		_stream->id = ++__broker->serial;
		_stream->epoch = ++__broker->serial;
		_stream->owner = __broker->peers[__index].serial;
		_stream->live = 1;
		_stream->attachDeadline = protocol_now() + UINT64_C(7000000000);
		protocol_set_number(__message, "stream", _stream->id);
		protocol_set_number(__message, "epoch", _stream->epoch);
		protocol_set_number(__message, "rate", _stream->rate);
		protocol_set_number(__message, "channels", _stream->channels);
		protocol_set_string(__message, "format", protocol_string(_device, "format"));
		protocol_set_string(__message, "token", _stream->token);
	}
	_pending->original = protocol_number(__message, "id", 0);
	_pending->forwarded = ++__broker->serial;
	_pending->owner = __broker->peers[__index].serial;
	_pending->stream = 0;
	if (_stream != NULL)
	{
		_pending->stream = _stream->id;
	}
	_pending->deadline = protocol_now() + UINT64_C(5000000000);
	protocol_set_number(__message, "id", _pending->forwarded);
	if (_broker_send(__broker, __broker->helper, __message) != 0)
	{
		_pending->forwarded = 0;
		if (_stream != NULL)
		{
			_broker_stream_close(__broker, _stream);
		}
		return "Helper control backpressure";
	}
	return NULL;
}

static int32_t _broker_linux_message(broker_t *__broker, int32_t __index, cJSON *__message)
{
	const char *_operation = protocol_string(__message, "op");
	uint64_t _request = protocol_number(__message, "id", 0);
	cJSON *_reply = NULL;
	if (strcmp(_operation, "status") == 0 || strcmp(_operation, "list") == 0)
	{
		_reply = _broker_inventory(__broker);
		protocol_set_string(_reply, "op", "reply");
		protocol_set_number(_reply, "id", _request);
		protocol_set_boolean(_reply, "ok", 1);
	}
	else if (strcmp(_operation, "close") == 0)
	{
		broker_stream_t *_stream = _broker_stream_find(__broker, protocol_number(__message, "stream", 0));
		uint8_t _owned = _stream != NULL && _stream->owner == __broker->peers[__index].serial;
		if (_owned != 0)
		{
			_broker_stream_close(__broker, _stream);
		}
		_reply = protocol_reply(_request, _owned, NULL);
	}
	else if (strcmp(_operation, "open") == 0 || strcmp(_operation, "profile") == 0)
	{
		const char *_error = _broker_forward(__broker, __index, __message);
		if (_error != NULL)
		{
			_reply = protocol_reply(_request, 0, _error);
		}
	}
	else
	{
		_reply = protocol_reply(_request, 0, "Unknown operation");
	}
	int32_t _result = 0;
	if (_reply != NULL)
	{
		_result = _broker_send(__broker, __index, _reply);
		cJSON_Delete(_reply);
	}
	return _result;
}

static int32_t _broker_finish_descriptor(broker_t *__broker, int32_t __index)
{
	broker_peer_t *_peer = &__broker->peers[__index];
	broker_stream_t *_stream = _broker_stream_find(__broker, _peer->transferStream);
	if (_stream == NULL || _peer->descriptor < 0)
	{
		return -1;
	}
	int32_t _result = protocol_send_descriptor(_peer->socket, _peer->descriptor);
	if (_result != 1)
	{
		return _result;
	}
	close(_peer->descriptor);
	_peer->descriptor = -1;
	if (_peer->androidSide != 0)
	{
		_stream->androidDescriptor = -1;
	}
	else
	{
		_stream->linuxDescriptor = -1;
	}
	_peer->role = BROKER_TRANSFERRED;
	if (_stream->linuxDescriptor < 0 && _stream->androidDescriptor < 0)
	{
		cJSON *_activate = cJSON_CreateObject();
		protocol_set_string(_activate, "op", "activate");
		protocol_set_number(_activate, "stream", _stream->id);
		_broker_send(__broker, __broker->helper, _activate);
		cJSON_Delete(_activate);
	}
	_broker_peer_close(__broker, __index);
	return 0;
}

static void _broker_watchdog(broker_t *__broker)
{
	uint64_t _now = protocol_now();
	for (size_t _index = 0; _index < BROKER_PENDING; ++_index)
	{
		broker_pending_t *_pending = &__broker->pending[_index];
		if (_pending->forwarded != 0 && _now >= _pending->deadline)
		{
			cJSON *_reply = protocol_reply(_pending->original, 0, "Android response timed out");
			_broker_send(__broker, _broker_owner(__broker, _pending->owner), _reply);
			cJSON_Delete(_reply);
			broker_stream_t *_stream = _broker_stream_find(__broker, _pending->stream);
			if (_stream != NULL)
			{
				_broker_stream_close(__broker, _stream);
			}
			_pending->forwarded = 0;
		}
	}
	for (size_t _index = 0; _index < BROKER_STREAMS; ++_index)
	{
		broker_stream_t *_stream = &__broker->streams[_index];
		if (_stream->live != 0 && (_stream->linuxDescriptor >= 0 || _stream->androidDescriptor >= 0) && _now >= _stream->attachDeadline)
		{
			_broker_stream_close(__broker, _stream);
		}
	}
	for (int32_t _index = 0; _index < BROKER_PEERS; ++_index)
	{
		broker_peer_t *_peer = &__broker->peers[_index];
		if (_peer->socket >= 0 && (framing_expired(&_peer->framing) != 0 || (_peer->handshakeDeadline != 0 && _now >= _peer->handshakeDeadline)))
		{
			_broker_peer_close(__broker, _index);
		}
	}
}

static void _broker_accept(broker_t *__broker)
{
	int32_t _socket = accept4(__broker->listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (_socket < 0)
	{
		return;
	}
	struct ucred _credentials;
	socklen_t _length = sizeof(_credentials);
	if (getsockopt(_socket, SOL_SOCKET, SO_PEERCRED, &_credentials, &_length) != 0)
	{
		close(_socket);
		return;
	}
	for (int32_t _index = 0; _index < BROKER_PEERS; ++_index)
	{
		broker_peer_t *_peer = &__broker->peers[_index];
		if (_peer->socket < 0)
		{
			memset(_peer, 0, sizeof(*_peer));
			_peer->socket = _socket;
			_peer->uid = (uint32_t)_credentials.uid;
			_peer->serial = ++__broker->serial;
			_peer->handshakeDeadline = protocol_now() + UINT64_C(5000000000);
			return;
		}
	}
	close(_socket);
}

static int32_t _broker_run(broker_t *__broker)
{
	__broker->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un _address = {0};
	_address.sun_family = AF_UNIX;
	memcpy(_address.sun_path + 1, __broker->socketName, strlen(__broker->socketName));
	if (__broker->listener < 0 || bind(__broker->listener, (struct sockaddr *)&_address, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(__broker->socketName))) != 0 || listen(__broker->listener, 16) != 0)
	{
		perror("audio broker bind");
		return 1;
	}
	puts("READY linux-audiod ABI=1");
	fflush(stdout);
	while (_broker_quit == 0)
	{
		struct pollfd _poll[BROKER_PEERS + 1] = {0};
		_poll[0].fd = __broker->listener;
		_poll[0].events = POLLIN;
		for (size_t _index = 0; _index < BROKER_PEERS; ++_index)
		{
			_poll[_index + 1].fd = __broker->peers[_index].socket;
			_poll[_index + 1].events = POLLIN;
			if (__broker->peers[_index].role == BROKER_DATA)
			{
				_poll[_index + 1].events = POLLOUT;
			}
			if (__broker->peers[_index].framing.outputLength > __broker->peers[_index].framing.outputOffset)
			{
				_poll[_index + 1].events |= POLLOUT;
			}
		}
		if (poll(_poll, BROKER_PEERS + 1, 50) < 0 && errno != EINTR)
		{
			break;
		}
		if ((_poll[0].revents & POLLIN) != 0)
		{
			_broker_accept(__broker);
		}
		for (int32_t _index = 0; _index < BROKER_PEERS; ++_index)
		{
			broker_peer_t *_peer = &__broker->peers[_index];
			if (_peer->socket < 0 || _peer->socket != _poll[_index + 1].fd)
			{
				continue;
			}
			int32_t _result = 0;
			if ((_poll[_index + 1].revents & POLLIN) != 0)
			{
				cJSON *_message = NULL;
				_result = framing_receive(_peer->socket, &_peer->framing, 0, &_message, NULL);
				if (_result == 1)
				{
					if (_peer->role == BROKER_NEW)
					{
						_result = _broker_hello(__broker, _index, _message);
					}
					else if (_peer->role == BROKER_HELPER)
					{
						_result = _broker_helper_message(__broker, _message);
					}
					else if (_peer->role == BROKER_LINUX)
					{
						_result = _broker_linux_message(__broker, _index, _message);
					}
					else
					{
						_result = -1;
					}
				}
				cJSON_Delete(_message);
			}
			if (_result >= 0 && framing_flush(_peer->socket, &_peer->framing) != 0)
			{
				_result = -1;
			}
			if (_result >= 0 && _peer->role == BROKER_DATA && _peer->framing.outputLength == 0)
			{
				_result = _broker_finish_descriptor(__broker, _index);
			}
			if (_result < 0 || (_poll[_index + 1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
			{
				_broker_peer_close(__broker, _index);
			}
		}
		_broker_watchdog(__broker);
	}
	for (int32_t _index = 0; _index < BROKER_PEERS; ++_index)
	{
		_broker_peer_close(__broker, _index);
	}
	close(__broker->listener);
	return 0;
}

int main(int __argc, char **__argv)
{
	broker_t *_broker = calloc(1, sizeof(*_broker));
	if (_broker == NULL)
	{
		return 1;
	}
	_broker->linuxUid = 4000;
	_broker->helper = -1;
	_broker->generation = 1;
	_broker->devices = cJSON_CreateArray();
	snprintf(_broker->socketName, sizeof(_broker->socketName), "%s", AUDIO_SOCKET);
	for (int32_t _index = 0; _index < BROKER_PEERS; ++_index)
	{
		_broker->peers[_index].socket = -1;
	}
	for (int32_t _index = 1; _index < __argc; ++_index)
	{
		if (strcmp(__argv[_index], "--socket") == 0 && _index + 1 < __argc)
		{
			snprintf(_broker->socketName, sizeof(_broker->socketName), "%s", __argv[++_index]);
		}
		else if (strcmp(__argv[_index], "--linux-uid") == 0 && _index + 1 < __argc)
		{
			_broker->linuxUid = (uint32_t)strtoul(__argv[++_index], NULL, 10);
		}
		else if (strcmp(__argv[_index], "--test") == 0 && _index + 1 < __argc && strcmp(__argv[++_index], "isolated") == 0)
		{
			_broker->isolatedTest = 1;
		}
		else
		{
			fprintf(stderr, "usage: linux-audiod [--linux-uid UID] [--socket ABSTRACT_NAME]\n");
			return 2;
		}
	}
	if ((_broker->isolatedTest != 0 && strcmp(_broker->socketName, AUDIO_SOCKET) == 0) || (getuid() != 0 && _broker->isolatedTest == 0))
	{
		fputs("Root required; tests require a separate socket\n", stderr);
		return 1;
	}
	signal(SIGTERM, _broker_signal);
	signal(SIGINT, _broker_signal);
	signal(SIGPIPE, SIG_IGN);
	int32_t _result = _broker_run(_broker);
	cJSON_Delete(_broker->devices);
	free(_broker);
	return _result;
}
