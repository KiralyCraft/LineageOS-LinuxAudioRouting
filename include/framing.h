#ifndef AUDIO_FRAMING_H
#define AUDIO_FRAMING_H
#include "protocol.h"
typedef struct
{
	uint8_t header[AUDIO_PCM_HEADER];
	size_t headerLength;
	size_t payloadLength;
	size_t payloadRead;
	uint8_t *payload;
	uint8_t *output;
	size_t outputLength;
	size_t outputOffset;
	size_t outputLimit;
	uint64_t readDeadline;
	uint64_t writeDeadline;
} framing_t;
void framing_clear(framing_t *__framing);
int32_t framing_receive(int32_t __socket, framing_t *__framing, uint8_t __pcm, cJSON **__message, protocol_t *__packet);
int32_t framing_enqueue_json(framing_t *__framing, const cJSON *__message);
int32_t framing_enqueue_pcm(framing_t *__framing, const protocol_t *__packet);
int32_t framing_flush(int32_t __socket, framing_t *__framing);
uint8_t framing_expired(const framing_t *__framing);
#endif
