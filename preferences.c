#define _GNU_SOURCE
#include "include/preferences.h"
#include "include/protocol.h"
#include <stdio.h>
#include <string.h>

static int32_t _preferences_property(void *__context, uint32_t __subject, const char *__key, const char *__type, const char *__value)
{
	(void)__type;
	preferences_t *_preferences = __context;
	if (__subject != PW_ID_CORE)
	{
		return 0;
	}
	cJSON *_value = NULL;
	if (__value != NULL)
	{
		_value = cJSON_Parse(__value);
	}
	const char *_name = protocol_string(_value, "name");
	uint8_t _changed = 0;
	pthread_mutex_lock(&_preferences->mutex);
	char *_target = NULL;
	if (__key == NULL)
	{
		_changed = _preferences->sink[0] != '\0' || _preferences->source[0] != '\0';
		_preferences->sink[0] = '\0';
		_preferences->source[0] = '\0';
	}
	else if (strcmp(__key, "default.audio.sink") == 0)
	{
		_target = _preferences->sink;
	}
	else if (strcmp(__key, "default.audio.source") == 0)
	{
		_target = _preferences->source;
	}
	if (_target != NULL && strcmp(_target, _name) != 0)
	{
		snprintf(_target, sizeof(_preferences->sink), "%s", _name);
		_changed = 1;
	}
	pthread_mutex_unlock(&_preferences->mutex);
	cJSON_Delete(_value);
	if (_changed && _preferences->changed != NULL)
	{
		_preferences->changed(_preferences->context);
	}
	return 0;
}

static const struct pw_metadata_events _preferences_metadataEvents = {.version = PW_VERSION_METADATA_EVENTS, .property = _preferences_property};

static void _preferences_global(void *__context, uint32_t __id, uint32_t __permissions, const char *__type, uint32_t __version, const struct spa_dict *__properties)
{
	(void)__permissions;
	preferences_t *_preferences = __context;
	if (_preferences->metadata != NULL || __properties == NULL || strcmp(__type, PW_TYPE_INTERFACE_Metadata) != 0)
	{
		return;
	}
	const char *_name = spa_dict_lookup(__properties, PW_KEY_METADATA_NAME);
	if (_name == NULL || strcmp(_name, "default") != 0)
	{
		return;
	}
	if (__version > PW_VERSION_METADATA)
	{
		__version = PW_VERSION_METADATA;
	}
	_preferences->metadata = pw_registry_bind(_preferences->registry, __id, __type, __version, 0);
	if (_preferences->metadata != NULL)
	{
		_preferences->metadataId = __id;
		pw_metadata_add_listener(_preferences->metadata, &_preferences->metadataListener, &_preferences_metadataEvents, _preferences);
	}
}

static void _preferences_remove(void *__context, uint32_t __id)
{
	preferences_t *_preferences = __context;
	if (_preferences->metadata != NULL && _preferences->metadataId == __id)
	{
		spa_hook_remove(&_preferences->metadataListener);
		pw_proxy_destroy((struct pw_proxy *)_preferences->metadata);
		_preferences->metadata = NULL;
		_preferences_property(_preferences, PW_ID_CORE, NULL, NULL, NULL);
	}
}

static const struct pw_registry_events _preferences_registryEvents = {.version = PW_VERSION_REGISTRY_EVENTS, .global = _preferences_global, .global_remove = _preferences_remove};

int32_t preferences_start(preferences_t *__preferences, struct pw_core *__core, void (*__changed)(void *), void *__context)
{
	memset(__preferences, 0, sizeof(*__preferences));
	if (pthread_mutex_init(&__preferences->mutex, NULL) != 0)
	{
		return -1;
	}
	__preferences->changed = __changed;
	__preferences->context = __context;
	__preferences->registry = pw_core_get_registry(__core, PW_VERSION_REGISTRY, 0);
	if (__preferences->registry == NULL)
	{
		pthread_mutex_destroy(&__preferences->mutex);
		return -1;
	}
	pw_registry_add_listener(__preferences->registry, &__preferences->registryListener, &_preferences_registryEvents, __preferences);
	return 0;
}

void preferences_snapshot(preferences_t *__preferences, char *__sink, size_t __sinkSize, char *__source, size_t __sourceSize)
{
	pthread_mutex_lock(&__preferences->mutex);
	snprintf(__sink, __sinkSize, "%s", __preferences->sink);
	snprintf(__source, __sourceSize, "%s", __preferences->source);
	pthread_mutex_unlock(&__preferences->mutex);
}

void preferences_stop(preferences_t *__preferences)
{
	/* The PipeWire loop is stopped before listeners and their state retire. */
	if (__preferences->metadata != NULL)
	{
		spa_hook_remove(&__preferences->metadataListener);
		pw_proxy_destroy((struct pw_proxy *)__preferences->metadata);
	}
	spa_hook_remove(&__preferences->registryListener);
	pw_proxy_destroy((struct pw_proxy *)__preferences->registry);
	pthread_mutex_destroy(&__preferences->mutex);
}
