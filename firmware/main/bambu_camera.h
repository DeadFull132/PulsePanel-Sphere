#ifndef PULSEPANEL_BAMBU_CAMERA_H
#define PULSEPANEL_BAMBU_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A1 mini / A1 local JPEG camera, receive-only. Never send printer controls.
 * The network worker owns pending buffers until taken by the UI thread.
 * Caller frees a taken frame after LVGL no longer refers to it. */
typedef enum {
    CAM_DISABLED = 0,
    CAM_NO_PRINTER,
    CAM_WAIT_WIFI,
    CAM_CONNECTING,
    CAM_TLS_ERROR,
    CAM_AUTH_ERROR,
    CAM_WAIT_FRAME,
    CAM_FRAME_READY,
    CAM_LOW_MEMORY,
    CAM_FRAME_TOO_LARGE,
    CAM_BAD_JPEG,
    CAM_DECODE_ERROR,
    CAM_NETWORK_ERROR
} cam_state_t;

typedef struct {
    uint8_t *pixels;
    size_t len;
    uint16_t width;
    uint16_t height;
} cam_frame_t;

void bambu_camera_init(void);
void bambu_camera_set_enabled(bool enabled);
void bambu_camera_set_wifi(bool ready);
cam_state_t bambu_camera_state(void);
const char *bambu_camera_state_text(cam_state_t state);
/* Non-blocking: RGB565 image in PSRAM, scaled to ~320 x 176. */
bool bambu_camera_take(cam_frame_t *frame);
void bambu_camera_release(cam_frame_t *frame);
#endif
