#ifndef AUDIO_PREFERENCES_H
#define AUDIO_PREFERENCES_H
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <pthread.h>
#include <stdint.h>
typedef struct
{
	struct pw_registry *registry;
	struct spa_hook registryListener;
	struct pw_metadata *metadata;
	struct spa_hook metadataListener;
	uint32_t metadataId;
	pthread_mutex_t mutex;
	char sink[256];
	char source[256];
	void (*changed)(void *);
	void *context;
} preferences_t;
int32_t preferences_start(preferences_t *__preferences, struct pw_core *__core, void (*__changed)(void *), void *__context);
void preferences_snapshot(preferences_t *__preferences, char *__sink, size_t __sinkSize, char *__source, size_t __sourceSize);
void preferences_stop(preferences_t *__preferences);
#endif
