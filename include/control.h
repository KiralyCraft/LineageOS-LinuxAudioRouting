#ifndef AUDIO_CONTROL_H
#define AUDIO_CONTROL_H
#include "protocol.h"
#include <pthread.h>
typedef void (*control_event_t)(void *__context, const cJSON *__message);
typedef struct
{
	int32_t socket;
	int32_t live;
	uint8_t readerStarted;
	char socketName[100];
	pthread_t reader;
	pthread_mutex_t stateMutex;
	pthread_mutex_t requestMutex;
	pthread_cond_t responseReady;
	uint64_t serial;
	uint64_t waiting;
	cJSON *reply;
	control_event_t event;
	void *context;
} control_t;
int32_t control_start(control_t *__control, const char *__socketName, control_event_t __event, void *__context);
void control_stop(control_t *__control);
cJSON *control_request(control_t *__control, cJSON *__message);
int32_t control_attach(control_t *__control, uint64_t __stream, const char *__token);
uint8_t control_live(const control_t *__control);
#endif
