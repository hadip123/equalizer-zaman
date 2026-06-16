#pragma once
#include "renderer.h"

int  mpris_init(void);
void mpris_poll(TrackInfo *info);
void mpris_cleanup(void);
