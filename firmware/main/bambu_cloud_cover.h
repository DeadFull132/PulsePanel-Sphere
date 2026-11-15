#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  PP_CLOUD_DISABLED=0, PP_CLOUD_NO_TOKEN, PP_CLOUD_NO_WIFI,
  PP_CLOUD_WAIT_TIME, PP_CLOUD_WAIT_PRINTER, PP_CLOUD_LOADING,
  PP_CLOUD_READY, PP_CLOUD_NO_COVER, PP_CLOUD_AUTH_FAILED,
  PP_CLOUD_HTTP_ERROR, PP_CLOUD_FORMAT_UNSUPPORTED,
  PP_CLOUD_LOW_MEMORY, PP_CLOUD_RATE_LIMITED
} pp_cloud_state_t;

typedef struct {
  uint8_t *pixels; /* RGB565_LE allocated in PSRAM, caller owns */
  size_t size;
  uint16_t width, height;
} pp_cloud_frame_t;

void pp_cloud_init(void);
void pp_cloud_wifi(bool enabled);
void pp_cloud_set_active(bool active);
pp_cloud_state_t pp_cloud_get_state(void);
const char *pp_cloud_state_text(pp_cloud_state_t state);
bool pp_cloud_take(pp_cloud_frame_t *out);
void pp_cloud_release(pp_cloud_frame_t *frame);
/* Experimental: token is stored in regular NVS, not suitable for production. */
bool pp_cloud_set_token(const char *token);
bool pp_cloud_has_token(void);
void pp_cloud_forget_token(void);
