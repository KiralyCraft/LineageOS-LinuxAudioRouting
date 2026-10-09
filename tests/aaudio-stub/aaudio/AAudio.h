/* Only the AAudio surface used by aaudio_policy.c; test implementation below. */
#ifndef AUDIO_TEST_AAUDIO_H
#define AUDIO_TEST_AAUDIO_H
#include <stdint.h>
#define AAUDIO_OK 0
#define AAUDIO_PERFORMANCE_MODE_NONE 10
#define AAUDIO_PERFORMANCE_MODE_LOW_LATENCY 12
typedef struct
{
	int32_t expectedPolicy;
	int32_t mode;
	int32_t result;
} AAudioStreamBuilder;
typedef struct
{
	int32_t unused;
} AAudioStream;
int32_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *__builder, AAudioStream **__stream);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *__builder, int32_t __mode);
#endif
