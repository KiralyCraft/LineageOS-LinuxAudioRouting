#ifndef AUDIO_AAUDIO_POLICY_H
#define AUDIO_AAUDIO_POLICY_H
#include <aaudio/AAudio.h>
#include <stdint.h>
int32_t aaudio_policy_open(AAudioStreamBuilder *__builder, AAudioStream **__stream, uint8_t __preferMmap);
#endif
