#pragma once
#include <stdbool.h>
#include <stdint.h>
/* One memory-intensive media session at a time: camera OR cloud download. */
void pp_media_init(void);
bool pp_media_acquire(uint32_t timeout_ms);
void pp_media_release(void);
