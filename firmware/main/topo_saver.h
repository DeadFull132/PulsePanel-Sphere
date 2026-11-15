#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

/* Screensaver: original procedural monochrome topographic animation.
 * Source is generated locally, NOT downloaded from YouTube.
 * Only lv_line objects and static point arrays; no video framebuffer. */
void pp_topo_init(lv_obj_t *screen, uint8_t normal_brightness,
                  bool (*may_enter)(void));
bool pp_topo_active(void);
void pp_topo_set_normal_brightness(uint8_t normal_brightness);
