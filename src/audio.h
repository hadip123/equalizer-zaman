#pragma once
#include <stdint.h>

#define AUDIO_NUM_BARS 64

int  audio_init(void);
void audio_cleanup(void);
int  audio_get_frequencies(float *bars, int count);

extern const char *audio_source_name;
