#ifndef AUDIO_PROTOCOL_H
#define AUDIO_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#include "../vendor/cJSON.h"
#define AUDIO_VERSION 1
#define AUDIO_SOCKET "linux-audio-broker-v1"
#define AUDIO_CONTROL_MAX 65536
#define AUDIO_PCM_MAX 65536
#define AUDIO_PCM_HEADER 40
#define AUDIO_PCM_MAGIC UINT32_C(0x50445541)
#define AUDIO_PCM_DATA 1
#define AUDIO_PCM_CLOCK 2
typedef struct
{
	uint32_t kind;
	uint64_t epoch;
	uint64_t frame;
	uint64_t timestamp;
	uint32_t frames;
	size_t length;
	uint8_t data[AUDIO_PCM_MAX];
} protocol_t;
uint64_t protocol_now(void);
int32_t protocol_connect(const char *__name);
int32_t protocol_transfer(int32_t __socket, void *__data, size_t __length, uint8_t __write, int32_t __timeout);
int32_t protocol_send_json(int32_t __socket, const cJSON *__message);
cJSON *protocol_receive_json(int32_t __socket, int32_t __timeout);
int32_t protocol_send_pcm(int32_t __socket, const protocol_t *__packet);
int32_t protocol_receive_pcm(int32_t __socket, protocol_t *__packet);
int32_t protocol_send_descriptor(int32_t __socket, int32_t __descriptor);
int32_t protocol_receive_descriptor(int32_t __socket);
cJSON *protocol_hello(const char *__role);
cJSON *protocol_reply(uint64_t __id, uint8_t __ok, const char *__error);
uint64_t protocol_number(const cJSON *__message, const char *__field, uint64_t __default);
const char *protocol_string(const cJSON *__message, const char *__field);
uint8_t protocol_boolean(const cJSON *__message, const char *__field);
void protocol_set_number(cJSON *__message, const char *__field, uint64_t __value);
void protocol_set_string(cJSON *__message, const char *__field, const char *__value);
void protocol_set_boolean(cJSON *__message, const char *__field, uint8_t __value);
#endif
