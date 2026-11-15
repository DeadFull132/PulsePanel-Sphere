/* PulsePanel FIX11: serialize camera TLS/JPEG and Cloud TLS/PNG work.
 * Only one memory-intensive media pipeline may run at once. */
#include "bambu_media_guard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
static SemaphoreHandle_t s_media_lock;
static const char *TAG="PP_MEDIA";

bool pp_media_acquire(uint32_t timeout_ms) {
    /* Initialize once before creating workers (main calls this via init). */
    SemaphoreHandle_t m = s_media_lock;
    if (!m) return false;
    return xSemaphoreTake(m, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void pp_media_release(void) {
    SemaphoreHandle_t m=s_media_lock;
    if (m) (void)xSemaphoreGive(m);
}
void pp_media_init(void) {
    /* Called once by app_main, before camera/cloud tasks start. */
    if (s_media_lock) return;
    s_media_lock=xSemaphoreCreateMutex();
    if (!s_media_lock) ESP_LOGE(TAG,"Mutex allocation failed");
}
