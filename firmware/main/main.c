/* PulsePanel Sphere HYBRID v0.7 — experimental firmware
 * Target: Waveshare ESP32-S3-Touch-AMOLED-1.75 (NOT 1.75C)
 * Independent source. No PrintSphere / BambuSphere code included.
 * Zero-app customer onboarding: captive portal, touch UI, clock, focus and sound.
 * PC telemetry is optional, NOT available from Wi-Fi alone.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "topo_saver.h"
#include "lvgl.h"
#include "esp_codec_dev.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "printer_monitor.h"

#define PAGE_CLOCK 0
#define PAGE_PC 1
#define PAGE_FOCUS 2
#define PAGE_SOUND 3
#define PAGE_NETWORK 4
#define PAGE_PRINTER 5
#define PAGE_PRINTER_WELCOME 6
#define PAGE_PRINTER_SETUP 7
#define PAGE_COUNT 8
#define UDP_DISCOVERY_PORT 38555
#define PC_TIMEOUT_MS 5500
#define FOCUS_DEFAULT 1500
#define CAPTIVE_DNS_PORT 53
static const char *TAG = "PPSphere";
static lv_obj_t *pages[PAGE_COUNT];
static lv_obj_t *time_label, *date_label, *focus_label, *focus_state;
static lv_obj_t *net_status, *net_ip, *net_setup, *net_extra, *net_pair_value;
static lv_obj_t *speaker_state;
static lv_obj_t *print_status, *print_percent, *print_time, *print_temps, *print_layers, *print_file, *print_ams;
static lv_obj_t *print_setup_url;
static lv_obj_t *print_connect_detail, *print_progress_arc;
static int printer_choice = -1;
static int64_t printer_config_until_us = 0;
static lv_obj_t *pc_fps, *pc_frametime, *pc_low, *pc_game, *pc_cpu, *pc_gpu, *pc_ram, *pc_temps, *pc_state;
static char pairing_pin[9] = {0};
typedef struct {
    int fps, low, frame_x10, cpu, gpu, ram, cpu_temp, gpu_temp;
    char game[33];
    int64_t last_update_us;
} pc_telemetry_t;
static pc_telemetry_t pc_data = {.fps=-1,.low=-1,.frame_x10=-1,.cpu=-1,.gpu=-1,.ram=-1,.cpu_temp=-1,.gpu_temp=-1};
static portMUX_TYPE pc_mux = portMUX_INITIALIZER_UNLOCKED;
static int invalid_pin_attempts = 0;
static int64_t pin_lock_until_us = 0;
static lv_obj_t *focus_button_text;
static int page = PAGE_CLOCK;
static bool focus_running = false;
static int focus_left = FOCUS_DEFAULT;
static volatile bool speaker_busy = false;
static bool wifi_ready = false;
static volatile bool setup = false;
static bool setup_requested = false;
static int disconnects = 0;
static char ip[16] = "--.--.--.--";
static char ap_name[28], ap_pass[12];
static httpd_handle_t server = NULL;
static char ssid[33], password[65];
static int64_t wifi_reset_armed_until = 0;
static int64_t factory_reset_armed_until = 0;
static lv_obj_t *wifi_reset_button_label;
static lv_obj_t *factory_reset_button_label;
static void restart_task(void *arg);

static lv_obj_t *label(lv_obj_t *parent, const char *value, int y, int width,
                       uint32_t color, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, width);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(l, value);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}

/* FIX8: LVGL labels should only be invalidated if text really changes.
 * Every old screen used to rewrite every label once per second, even while
 * hidden. On this PSRAM-backed SPI AMOLED that generates avoidable flushes. */
static void pp_label_set_text_if_changed(lv_obj_t *obj, const char *value)
{
    if (!obj || !value) return;
    const char *old = lv_label_get_text(obj);
    if (!old || strcmp(old, value) != 0) lv_label_set_text(obj, value);
}

static void pp_label_set_text_fmt_if_changed(lv_obj_t *obj, const char *fmt, ...)
{
    char out[160];
    va_list args;
    va_start(args, fmt);
    int count = vsnprintf(out, sizeof(out), fmt, args);
    va_end(args);
    if (count >= 0) pp_label_set_text_if_changed(obj, out);
}

static bool is_main_page(int dest)
{
    return dest >= PAGE_CLOCK && dest <= PAGE_PRINTER;
}

/* Disable automatic screensaver while configuring Wi-Fi/printer. */
static bool topo_allowed(void)
{
    return !setup && is_main_page(page);
}

static void change_page(int dest)
{
    if (dest < 0 || dest >= PAGE_COUNT) return;
    page = dest;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i == page) lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void swipe_cb(lv_event_t *event)
{
    (void)event;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if (!is_main_page(page)) return;
    if (direction == LV_DIR_LEFT) {
        change_page((page + 1) % (PAGE_PRINTER + 1));
        lv_indev_wait_release(indev);
    } else if (direction == LV_DIR_RIGHT) {
        change_page((page + PAGE_PRINTER) % (PAGE_PRINTER + 1));
        lv_indev_wait_release(indev);
    }
}

/* Fallback navigation: taps work even when the user swipes too slowly
 * for LVGL's gesture recognition threshold. These are used only on the
 * six normal pages; printer setup screens keep their dedicated buttons. */
static void previous_page_press(lv_event_t *event)
{
    (void)event;
    if (is_main_page(page)) change_page((page + PAGE_PRINTER) % (PAGE_PRINTER + 1));
}

static void next_page_press(lv_event_t *event)
{
    (void)event;
    if (is_main_page(page)) change_page((page + 1) % (PAGE_PRINTER + 1));
}

static void sound_task(void *arg)
{
    (void)arg;
    esp_codec_dev_handle_t codec = bsp_audio_codec_speaker_init();
    if (codec) {
        esp_codec_dev_sample_info_t fmt = { .sample_rate = 24000, .channel = 2, .bits_per_sample = 16 };
        if (esp_codec_dev_open(codec, &fmt) == ESP_OK) {
            (void)esp_codec_dev_set_out_vol(codec, 35.0f);
            /* Stereo PCM triangle wave with soft envelope; no copyrighted sample. */
            int16_t buffer[320 * 2];
            const int total = 24000 / 3;
            for (int start = 0; start < total; start += 320) {
                int n = total - start;
                if (n > 320) n = 320;
                for (int i = 0; i < n; i++) {
                    int at = start + i;
                    uint32_t p = ((uint64_t)at * 784U * 65536U / 24000U) & 65535U;
                    int wave = p < 32768U ? (int)p : (int)(65535U - p);
                    wave = (wave - 16384) / 5;
                    int fade = (at < 320 ? at : (total - at < 320 ? total - at : 320));
                    int16_t sample = (int16_t)(wave * fade / 320);
                    buffer[i * 2] = sample;
                    buffer[i * 2 + 1] = sample;
                }
                if (esp_codec_dev_write(codec, buffer, n * 4) != ESP_OK) break;
            }
            (void)esp_codec_dev_close(codec);
        } else ESP_LOGW(TAG, "Audio codec could not open");
    } else ESP_LOGW(TAG, "Speaker codec not available (speaker may not be connected)");
    speaker_busy = false;
    vTaskDelete(NULL);
}

static void play_tone(void)
{
    if (speaker_busy) return;
    speaker_busy = true;
    if (xTaskCreate(sound_task, "pp_tone", 8192, NULL, 4, NULL) != pdPASS)
        speaker_busy = false;
}

static void sound_press(lv_event_t *event)
{
    (void)event;
    play_tone();
    lv_label_set_text(speaker_state, "Dzwiek testowy (ok. 0,3 s)");
}

static void focus_press(lv_event_t *event)
{
    (void)event;
    if (focus_left == 0) focus_left = FOCUS_DEFAULT;
    focus_running = !focus_running;
    lv_label_set_text(focus_button_text, focus_running ? "PAUZA" : "START");
    lv_label_set_text(focus_state, focus_running ? "SESJA TRWA" : "GOTOWY");
}

static void reset_press(lv_event_t *event)
{
    (void)event;
    focus_left = FOCUS_DEFAULT;
    focus_running = false;
    lv_label_set_text(focus_label, "25:00");
    lv_label_set_text(focus_state, "GOTOWY");
    lv_label_set_text(focus_button_text, "START");
}

static lv_obj_t *pill(lv_obj_t *parent, const char *text,
                      int x, int y, int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_align(btn, LV_ALIGN_CENTER, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x205266), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x66cfe1), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 22, 0);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    label(btn, text, 0, w - 10, 0xeffcff, &lv_font_montserrat_20);
    return btn;
}

/* Reset Wi-Fi from touch only, requiring two taps to avoid accidental resets.
 * This clears ONLY customer Wi-Fi credentials, not device identity or PC PIN. */
static void reset_wifi_press(lv_event_t *event)
{
    (void)event;
    if (setup) return;
    int64_t now = esp_timer_get_time();
    if (now > wifi_reset_armed_until) {
        wifi_reset_armed_until = now + 8000000LL;
        if (wifi_reset_button_label) lv_label_set_text(wifi_reset_button_label, "POTWIERDZ");
        return;
    }
    wifi_reset_armed_until = 0;
    nvs_handle_t h;
    if (nvs_open("pp_config", NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_erase_key(h, "ssid");
        (void)nvs_erase_key(h, "pass");
        (void)nvs_commit(h);
        nvs_close(h);
    }
    if (wifi_reset_button_label) lv_label_set_text(wifi_reset_button_label, "RESTART...");
    xTaskCreate(restart_task, "pp_reset_wifi", 2048, NULL, 3, NULL);
}

/* Fully reset personal data before handing the device to another customer.
   Separate from Wi-Fi reset, which intentionally preserves printer details. */
static void factory_reset_press(lv_event_t *event)
{
    (void)event;
    int64_t now=esp_timer_get_time();
    if (now > factory_reset_armed_until) {
        factory_reset_armed_until=now+8000000LL;
        if (factory_reset_button_label) lv_label_set_text(factory_reset_button_label,"POTWIERDZ RESET");
        return;
    }
    factory_reset_armed_until=0;
    nvs_handle_t h;
    if (nvs_open("pp_config",NVS_READWRITE,&h)==ESP_OK) {
        const char *keys[]={"ssid","pass","pc_pin","pr_choice","pr_ip","pr_serial","pr_code"};
        for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++) (void)nvs_erase_key(h,keys[i]);
        (void)nvs_commit(h);
        nvs_close(h);
    }
    if (factory_reset_button_label) lv_label_set_text(factory_reset_button_label,"RESET...");
    xTaskCreate(restart_task,"pp_factory",2048,NULL,3,NULL);
}

static void save_printer_choice(uint8_t selected)
{
    nvs_handle_t h;
    if (nvs_open("pp_config", NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, "pr_choice", selected);
    (void)nvs_commit(h);
    nvs_close(h);
    printer_choice = (int)selected;
}

static void printer_setup_page(void)
{
    if (!wifi_ready || setup) return;
    printer_config_until_us = esp_timer_get_time() + 600000000LL; /* 10 minutes */
    char url[70];
    snprintf(url, sizeof(url), "http://%s/printer", ip);
    lv_label_set_text(print_setup_url, url);
    change_page(PAGE_PRINTER_SETUP);
}

static void printer_yes_press(lv_event_t *event)
{
    (void)event;
    save_printer_choice(1);
    printer_setup_page();
}

static void printer_no_press(lv_event_t *event)
{
    (void)event;
    save_printer_choice(0);
    change_page(PAGE_CLOCK);
}

static void printer_change_press(lv_event_t *event)
{
    (void)event;
    printer_setup_page();
}

static void printer_back_press(lv_event_t *event)
{
    (void)event;
    printer_config_until_us = 0;
    change_page(PAGE_PRINTER);
}

/* Premium circular AMOLED layout: common surface, dividers, page index.
 * Draw decorations first, under labels and controls, so they never steal touch. */
static void theme_surface(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, 330, 280);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0d2130), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x23465a), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 35, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *accent = lv_obj_create(parent);
    lv_obj_set_size(accent, 62, 4);
    lv_obj_align(accent, LV_ALIGN_CENTER, 0, -142);
    lv_obj_set_style_bg_color(accent, lv_color_hex(0x73e5ee), 0);
    lv_obj_set_style_border_width(accent, 0, 0);
    lv_obj_set_style_radius(accent, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(accent, 0, 0);
    lv_obj_remove_flag(accent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(accent, LV_OBJ_FLAG_CLICKABLE);
}

static void create_ui(void)
{
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(0x050e18), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < PAGE_COUNT; i++) {
        pages[i] = lv_obj_create(root);
        lv_obj_set_size(pages[i], 466, 466);
        lv_obj_center(pages[i]);
        lv_obj_set_style_bg_color(pages[i], lv_color_hex(0x050e18), 0);
        lv_obj_set_style_border_width(pages[i], 0, 0);
        lv_obj_set_style_pad_all(pages[i], 0, 0);
        lv_obj_set_style_radius(pages[i], 0, 0);
        lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_SCROLLABLE);
        /* LVGL bubbles gestures upward by default. Stop them at this
         * page, where swipe_cb is registered, instead of losing them
         * on the top-level screen object (which has no swipe callback). */
        lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(pages[i], swipe_cb, LV_EVENT_GESTURE, NULL);
        theme_surface(pages[i]);
        label(pages[i], "PULSE  /  PANEL", -178, 300, 0x73e5ee, &lv_font_montserrat_20);
        if (i != PAGE_NETWORK && i != PAGE_PRINTER && i != PAGE_PRINTER_SETUP)
            label(pages[i], "DOTKNIJ STRZALKI  <  >", 179, 230, 0x668899, &lv_font_montserrat_20);
    }
    label(pages[PAGE_CLOCK], "ZEGAR  /  ONLINE", -109, 280, 0x9abcd0, &lv_font_montserrat_20);
    time_label = label(pages[PAGE_CLOCK], "--:--", -22, 310, 0xffffff, &lv_font_montserrat_48);
    date_label = label(pages[PAGE_CLOCK], "Czekam na godzine", 37, 310, 0x83e6ef, &lv_font_montserrat_20);
    label(pages[PAGE_CLOCK], "SYNCHRONIZACJA PRZEZ WIFI", 99, 304,
          0x7c9bab, &lv_font_montserrat_20);

    label(pages[PAGE_PC], "GAMING  /  LIVE", -120, 290, 0x96bbc9, &lv_font_montserrat_20);
    pc_fps = label(pages[PAGE_PC], "-- FPS", -71, 310, 0xffffff, &lv_font_montserrat_48);
    pc_game = label(pages[PAGE_PC], "PC: WYMAGA NADAJNIKA", -23, 310, 0x75e2ee, &lv_font_montserrat_20);
    pc_frametime = label(pages[PAGE_PC], "FRAME: -- ms", 13, 310, 0xa5becc, &lv_font_montserrat_20);
    pc_low = label(pages[PAGE_PC], "1% LOW: --", 44, 310, 0xa5becc, &lv_font_montserrat_20);
    pc_cpu = label(pages[PAGE_PC], "CPU: --", 78, 290, 0x91e2ec, &lv_font_montserrat_20);
    pc_gpu = label(pages[PAGE_PC], "GPU: --", 108, 290, 0x91e2ec, &lv_font_montserrat_20);
    pc_ram = label(pages[PAGE_PC], "RAM: --", 138, 290, 0x91e2ec, &lv_font_montserrat_20);
    pc_temps = label(pages[PAGE_PC], "", 165, 330, 0xa3bdc7, &lv_font_montserrat_20);
    pc_state = label(pages[PAGE_PC], "", 193, 320, 0x8199aa, &lv_font_montserrat_20);

    label(pages[PAGE_FOCUS], "FOCUS / POMODORO", -107, 340, 0x86adbc, &lv_font_montserrat_20);
    focus_label = label(pages[PAGE_FOCUS], "25:00", -33, 330, 0xffffff, &lv_font_montserrat_48);
    focus_state = label(pages[PAGE_FOCUS], "GOTOWY", 25, 260, 0x7ddec5, &lv_font_montserrat_20);
    lv_obj_t *start = pill(pages[PAGE_FOCUS], "START", -74, 103, 140, 56, focus_press);
    focus_button_text = lv_obj_get_child(start, 0);
    pill(pages[PAGE_FOCUS], "RESET", 74, 103, 140, 56, reset_press);

    label(pages[PAGE_SOUND], "AUDIO / GLOSNIK", -105, 350, 0x91bac6, &lv_font_montserrat_20);
    label(pages[PAGE_SOUND], "ES8311", -44, 320, 0xffffff, &lv_font_montserrat_48);
    speaker_state = label(pages[PAGE_SOUND], "Dotknij, aby odtworzyc dzwiek", 22, 370,
                          0x9dbbc8, &lv_font_montserrat_20);
    pill(pages[PAGE_SOUND], "TEST DZWIEKU", 0, 104, 270, 64, sound_press);

    /* FIX6: Keep the pairing PIN in its own large, unobstructed row.
     * The old "KOD PC" line (y=121) overlapped the Wi-Fi button
     * (centre y=144, height=55), which was painted later and hid the PIN.
     * The new positions also respect the circular 466x466 display. */
    label(pages[PAGE_NETWORK], "USTAWIENIA SIECI", -122, 345, 0x97bcca, &lv_font_montserrat_20);
    net_status = label(pages[PAGE_NETWORK], "Uruchamianie...", -82, 364, 0xe7f9fe, &lv_font_montserrat_28);
    net_ip = label(pages[PAGE_NETWORK], "", -43, 350, 0x65deef, &lv_font_montserrat_20);
    net_setup = label(pages[PAGE_NETWORK], "", -10, 355, 0xafc9d4, &lv_font_montserrat_20);
    net_extra = label(pages[PAGE_NETWORK], "KOD PAROWANIA PC", 33, 320, 0x97b5c3, &lv_font_montserrat_20);
    net_pair_value = label(pages[PAGE_NETWORK], "--------", 81, 350, 0xeaffff, &lv_font_montserrat_48);
    lv_obj_set_style_text_letter_space(net_pair_value, 2, 0);
    lv_obj_t *reset_wifi = pill(pages[PAGE_NETWORK], "ZMIEN WI-FI", 0, 144, 210, 42, reset_wifi_press);
    wifi_reset_button_label = lv_obj_get_child(reset_wifi, 0);
    lv_obj_t *reset_all = pill(pages[PAGE_NETWORK], "RESET FABRYCZNY", 0, 191, 196, 32, factory_reset_press);
    factory_reset_button_label = lv_obj_get_child(reset_all, 0);
    label(pages[PAGE_PRINTER], "BAMBU LAB  /  LIVE", -124, 310, 0x8cd3e1, &lv_font_montserrat_20);
    print_status = label(pages[PAGE_PRINTER], "BRAK DRUKARKI", -96, 310, 0x9fe2ec, &lv_font_montserrat_20);
    /* Decorative progress ring; not a touch control. */
    print_progress_arc = lv_arc_create(pages[PAGE_PRINTER]);
    lv_obj_set_size(print_progress_arc, 194, 194);
    lv_obj_align(print_progress_arc, LV_ALIGN_CENTER, 0, -24);
    lv_arc_set_range(print_progress_arc, 0, 100);
    lv_arc_set_value(print_progress_arc, 0);
    lv_obj_set_style_arc_color(print_progress_arc, lv_color_hex(0x294858), LV_PART_MAIN);
    lv_obj_set_style_arc_color(print_progress_arc, lv_color_hex(0x75e5ed), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(print_progress_arc, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_width(print_progress_arc, 11, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(print_progress_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_remove_flag(print_progress_arc, LV_OBJ_FLAG_CLICKABLE);
    print_percent = label(pages[PAGE_PRINTER], "--%", -24, 220, 0xffffff, &lv_font_montserrat_48);
    print_file = label(pages[PAGE_PRINTER], "--", 50, 310, 0x88e4ee, &lv_font_montserrat_20);
    print_time = label(pages[PAGE_PRINTER], "Pozostalo: -- min", 78, 310, 0xbed9e4, &lv_font_montserrat_20);
    print_temps = label(pages[PAGE_PRINTER], "Dysza: --   Stol: --", 106, 310, 0xa8c9d7, &lv_font_montserrat_20);
    print_layers = label(pages[PAGE_PRINTER], "Warstwa: -- / --", 134, 310, 0xa8c9d7, &lv_font_montserrat_20);
    print_ams = label(pages[PAGE_PRINTER], "AMS: --", 159, 260, 0x809fad, &lv_font_montserrat_20);
    print_connect_detail = label(pages[PAGE_PRINTER], "", -74, 310, 0xb0ccd9, &lv_font_montserrat_20);
    pill(pages[PAGE_PRINTER], "DODAJ / ZMIEN", 0, 198, 162, 30, printer_change_press);

    label(pages[PAGE_PRINTER_WELCOME], "MASZ DRUKARKE", -103, 355, 0xe2f5fa, &lv_font_montserrat_28);
    label(pages[PAGE_PRINTER_WELCOME], "BAMBU LAB?", -60, 345, 0x77dfee, &lv_font_montserrat_28);
    label(pages[PAGE_PRINTER_WELCOME], "Mozesz dodac ja pozniej.", 6, 350, 0x93b2be, &lv_font_montserrat_20);
    pill(pages[PAGE_PRINTER_WELCOME], "TAK", -83, 85, 145, 66, printer_yes_press);
    pill(pages[PAGE_PRINTER_WELCOME], "NIE", 83, 85, 145, 66, printer_no_press);

    label(pages[PAGE_PRINTER_SETUP], "DODAJ DRUKARKE", -111, 375, 0xb5edf4, &lv_font_montserrat_28);
    label(pages[PAGE_PRINTER_SETUP], "Na telefonie otworz:", -57, 355, 0xabc4d0, &lv_font_montserrat_20);
    print_setup_url = label(pages[PAGE_PRINTER_SETUP], "Czekam na Wi-Fi", -10, 410, 0x6ed7e9, &lv_font_montserrat_20);
    label(pages[PAGE_PRINTER_SETUP], "Wpisz IP, serial i kod LAN", 36, 374, 0xc1d6df, &lv_font_montserrat_20);
    label(pages[PAGE_PRINTER_SETUP], "Masz 10 min. Ustawienia zostana zapisane.", 78, 400, 0x88a9b7, &lv_font_montserrat_20);
    pill(pages[PAGE_PRINTER_SETUP], "POWROT", 0, 143, 220, 58, printer_back_press);

    /* Persistent visible touch navigation for all six main pages.
     * Place these last so they are above the labels in hit-testing.
     * Narrow hit regions stay inside the round 466x466 viewport. */
    for (int i = PAGE_CLOCK; i <= PAGE_PRINTER; ++i) {
        lv_obj_t *left = pill(pages[i], "<", -180, -5, 50, 72, previous_page_press);
        lv_obj_t *right = pill(pages[i], ">", 180, -5, 50, 72, next_page_press);
        lv_obj_set_style_bg_opa(left, LV_OPA_30, 0);
        lv_obj_set_style_bg_opa(right, LV_OPA_30, 0);
    }
    change_page(PAGE_CLOCK);
}

static void network_text(const char *status, const char *line1, const char *line2)
{
    if (bsp_display_lock(600) == ESP_OK) {
        lv_label_set_text(net_status, status);
        lv_label_set_text(net_ip, line1);
        lv_label_set_text(net_setup, line2);
        if (setup) {
            lv_label_set_text(net_extra, "OTWORZ W TELEFONIE");
            lv_obj_set_style_text_font(net_pair_value, &lv_font_montserrat_28, 0);
            lv_label_set_text(net_pair_value, "192.168.4.1");
        } else {
            lv_label_set_text(net_extra, "KOD PAROWANIA PC");
            lv_obj_set_style_text_font(net_pair_value, &lv_font_montserrat_48, 0);
            lv_label_set_text(net_pair_value, pairing_pin[0] ? pairing_pin : "--------");
        }
        bsp_display_unlock();
    }
}

static bool read_form(const char *form, const char *key, char *dst, size_t capacity)
{
    size_t k = strlen(key);
    const char *part = form;
    while (part && *part) {
        if (!strncmp(part, key, k) && part[k] == '=') {
            part += k + 1;
            size_t n = 0;
            while (*part && *part != '&') {
                unsigned char ch = (unsigned char)*part++;
                if (ch == '+') ch = ' ';
                else if (ch == '%') {
                    if (!part[0] || !part[1]) return false;
                    char hex[3] = { part[0], part[1], 0 };
                    char *end;
                    unsigned long v = strtoul(hex, &end, 16);
                    if (*end || v > 255) return false;
                    ch = (unsigned char)v;
                    part += 2;
                }
                if (!ch || n + 1 >= capacity) return false;
                dst[n++] = (char)ch;
            }
            dst[n] = 0;
            return true;
        }
        part = strchr(part, '&');
        if (part) part++;
    }
    return false;
}

static esp_err_t root_http(httpd_req_t *request)
{
    if (!setup) {
        httpd_resp_set_type(request, "text/html; charset=utf-8");
        return httpd_resp_sendstr(request,
             "<!doctype html><meta charset='utf-8'><title>PulsePanel</title>"
             "<h1>PulsePanel Sphere</h1><p>Ekran dziala samodzielnie.</p>"
             "<p>Zegar, dotyk, Focus, dzwiek i monitoring PC sa na wyswietlaczu.</p>"
             "<p>Dane PC przyjmowane sa tylko z lokalnej sieci po sparowaniu PIN.</p>");
    }
    const char *html = "<!doctype html><html lang='pl'><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>PulsePanel | Pierwsze uruchomienie</title>"
        "<style>*{box-sizing:border-box}body{font:17px system-ui;background:#08121b;color:#e7f7ff;"
        "max-width:440px;margin:36px auto;padding:20px}h1{font-size:29px}"
        "p{color:#bad2dc;line-height:1.5}label{font-weight:650}input,button{width:100%;padding:15px;"
        "font:inherit;border-radius:10px;margin:9px 0 19px;border:1px solid #48606d}"
        "input{background:#fff;color:#14202a}button{background:#6ed7e9;color:#03131b;"
        "font-weight:750;border:0}small{color:#92b4c4}</style>"
        "<h1>PulsePanel Sphere</h1>"
        "<p>Witaj! Polacz monitorek z domowym Wi-Fi 2,4 GHz."
        " Niczego nie musisz instalowac na telefonie ani komputerze.</p>"
        "<form method='POST' action='/configure'>"
        "<label for='ssid'>Nazwa domowej sieci Wi-Fi</label>"
        "<input id='ssid' name='ssid' maxlength='32' autocomplete='off' required placeholder='np. MojDom'>"
        "<label for='password'>Haslo Wi-Fi</label>"
        "<input id='password' name='password' maxlength='63' type='password' autocomplete='off' placeholder='Haslo routera'>"
        "<button>Polacz PulsePanel</button></form>"
        "<small>Po zapisaniu urzadzenie zapamieta siec i polaczy sie automatycznie."
        " Jesli telefon nie wyswietlil tej strony, otworz http://192.168.4.1</small>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_sendstr(request, html);
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1600));
    esp_restart();
}

static esp_err_t configure_http(httpd_req_t *request)
{
    if (!setup) { httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "Not in setup mode"); return ESP_FAIL; }
    if (request->content_len < 6 || request->content_len > 510) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Form too long"); return ESP_FAIL;
    }
    char body[512] = {0};
    int total = 0;
    while (total < request->content_len) {
        int r = httpd_req_recv(request, body + total, request->content_len - total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        total += r;
    }
    char new_ssid[33] = {0}, new_pass[65] = {0};
    if (!read_form(body, "ssid", new_ssid, sizeof(new_ssid)) || !new_ssid[0] ||
        !read_form(body, "password", new_pass, sizeof(new_pass))) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid SSID or password"); return ESP_FAIL;
    }
    nvs_handle_t h;
    if (nvs_open("pp_config", NVS_READWRITE, &h) != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Storage error"); return ESP_FAIL;
    }
    esp_err_t res = nvs_set_str(h, "ssid", new_ssid);
    if (res == ESP_OK) res = nvs_set_str(h, "pass", new_pass);
    if (res == ESP_OK) res = nvs_commit(h);
    nvs_close(h);
    memset(new_pass, 0, sizeof(new_pass));
    if (res != ESP_OK) { httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Save error"); return ESP_FAIL; }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_sendstr(request, "<meta charset='utf-8'><h2>Wi-Fi zapisane.</h2><p>Ekran uruchomi sie ponownie.</p>");
    xTaskCreate(restart_task, "pp_reset", 2048, NULL, 3, NULL);
    return ESP_OK;
}


/* Printer credentials are configured only after the on-screen button was
 * pressed. No cloud login is needed; this form is LOCAL HTTP, so use a trusted
 * LAN and do not expose the ESP32 to the public internet. */
static bool printer_form_allowed(void)
{
    return wifi_ready && !setup && printer_config_until_us > esp_timer_get_time();
}
static esp_err_t printer_get_http(httpd_req_t *req)
{
    if (!printer_form_allowed()) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Otworz ustawienia drukarki na ekranie PulsePanel");
        return ESP_FAIL;
    }
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    return httpd_resp_sendstr(req,
        "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>PulsePanel - drukarka</title>"
        "<style>body{font:17px system-ui;background:#071521;color:#e8f8fd;max-width:430px;"
        "margin:30px auto;padding:18px}input,button{display:block;box-sizing:border-box;width:100%;"
        "font:inherit;padding:13px;margin:10px 0 20px;border-radius:9px}button{background:#6ed7e9;"
        "border:0;font-weight:bold}p{line-height:1.55;color:#bad0dc}</style>"
        "<h1>Polacz drukarke Bambu Lab</h1>"
        "<p>Drukarka i PulsePanel musza byc w tej samej sieci LAN. Odczyt jest tylko do monitorowania.</p>"
        "<form action='/printer' method='post'>"
        "<label>Adres IP drukarki<input name='ip' inputmode='decimal' placeholder='192.168.1.50' maxlength='15' required></label>"
        "<label>Numer seryjny drukarki<input name='serial' placeholder='Z ustawien drukarki' maxlength='40' required></label>"
        "<label>Kod dostepu LAN<input name='code' type='password' maxlength='32' autocomplete='off' required></label>"
        "<button>Zapisz i polacz</button></form>"
        "<p>Adres IP, serial i kod LAN odczytaj na drukarce w ustawieniach sieci. "
        "Dostep MQTT moze zalezec od jej modelu i firmware. Kod zostaje zapisany w pamieci PulsePanel.</p>");
}
static esp_err_t printer_post_http(httpd_req_t *req)
{
    if (!printer_form_allowed()) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,"Konfiguracja wygasla - uzyj dotyku");
        return ESP_FAIL;
    }
    if (req->content_len < 14 || req->content_len > 500) {
        httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Nieprawidlowe dane"); return ESP_FAIL;
    }
    char body[512]={0}; int total=0;
    while (total < req->content_len) {
        int r=httpd_req_recv(req,body+total,req->content_len-total);
        if (r==HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r<=0) return ESP_FAIL;
        total+=r;
    }
    char addr[16]={0}, serial[41]={0}, code[33]={0};
    bool ok=read_form(body,"ip",addr,sizeof(addr)) &&
        read_form(body,"serial",serial,sizeof(serial)) &&
        read_form(body,"code",code,sizeof(code)) &&
        printer_monitor_store(addr,serial,code);
    memset(code,0,sizeof(code)); memset(body,0,sizeof(body));
    if (!ok) {
        httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Sprawdz adres IP, numer seryjny i kod LAN");
        return ESP_FAIL;
    }
    printer_config_until_us=0;
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_sendstr(req,"<meta charset='utf-8'><h2>Drukarka zapisana.</h2>"
        "<p>PulsePanel uruchomi sie ponownie i sprobuje polaczyc sie automatycznie.</p>");
    xTaskCreate(restart_task,"pp_pr_restart",2048,NULL,3,NULL);
    return ESP_OK;
}

/* Simple captive portal DNS responder: in setup mode resolves A queries to 192.168.4.1.
 * Many phones then offer a browser setup page without installing an app.
 * It cannot intercept HTTPS and popup behavior is phone/OS-dependent. */
static void captive_dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) { ESP_LOGE(TAG, "Captive DNS socket error"); vTaskDelete(NULL); return; }
    struct sockaddr_in listener = {0};
    listener.sin_family = AF_INET;
    listener.sin_port = htons(CAPTIVE_DNS_PORT);
    listener.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&listener, sizeof(listener)) < 0) {
        ESP_LOGE(TAG, "Captive DNS bind error");
        close(sock); vTaskDelete(NULL); return;
    }
    uint8_t packet[512], answer[512];
    for (;;) {
        struct sockaddr_in client = {0};
        socklen_t clen = sizeof(client);
        int n = recvfrom(sock, packet, sizeof(packet), 0, (struct sockaddr *)&client, &clen);
        if (!setup || n < 18 || n > 480) continue;
        /* One standard DNS question, query (not response), class IN, type A. */
        if ((packet[2] & 0x80) || packet[4] != 0 || packet[5] != 1) continue;
        int offset = 12;
        bool valid = true;
        while (offset < n && packet[offset]) {
            uint8_t len = packet[offset];
            if (len > 63 || offset + 1 + len >= n) { valid = false; break; }
            offset += 1 + len;
        }
        if (!valid || offset + 5 > n) continue;
        offset++;
        if (packet[offset] != 0 || packet[offset+1] != 1 ||
            packet[offset+2] != 0 || packet[offset+3] != 1) continue;
        int qend = offset + 4;
        if (qend + 16 > (int)sizeof(answer)) continue;
        memcpy(answer, packet, qend);
        answer[2] = 0x81; answer[3] = 0x80; /* Standard response, no error */
        answer[4] = 0; answer[5] = 1;  /* 1 question */
        answer[6] = 0; answer[7] = 1;  /* 1 answer */
        answer[8] = answer[9] = answer[10] = answer[11] = 0;
        const uint8_t record[16] = {
            0xC0, 0x0C, 0, 1, 0, 1,    /* compressed name, A, IN */
            0, 0, 0, 30, 0, 4,         /* TTL 30 s, length 4 */
            192, 168, 4, 1
        };
        memcpy(answer + qend, record, sizeof(record));
        sendto(sock, answer, qend + sizeof(record), 0,
               (struct sockaddr *)&client, clen);
    }
}

/* Unknown captive-portal probe URLs should lead to the setup page. */
static esp_err_t captive_http_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (setup) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_sendstr(req, "<meta charset='utf-8'>Otworz konfiguracje Wi-Fi");
    }
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_sendstr(req, "Not found");
}

/* Telemetry is accepted only from a paired computer, on the local network.
 * The pairing code stays on the device screen and is never sent by discovery. */
static void init_pairing_pin(void)
{
    nvs_handle_t h;
    if (nvs_open("pp_config", NVS_READWRITE, &h) != ESP_OK) return;
    size_t capacity = sizeof(pairing_pin);
    if (nvs_get_str(h, "pc_pin", pairing_pin, &capacity) != ESP_OK || strlen(pairing_pin) != 8) {
        snprintf(pairing_pin, sizeof(pairing_pin), "%08lu", 10000000UL + (unsigned long)(esp_random() % 90000000UL));
        (void)nvs_set_str(h, "pc_pin", pairing_pin);
        (void)nvs_commit(h);
    }
    nvs_close(h);
}

static esp_err_t status_http(httpd_req_t *request)
{
    if (!wifi_ready || setup) {
        httpd_resp_send_custom_err(request, "503 Service Unavailable", "Wi-Fi offline");
        return ESP_FAIL;
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request,
        "{\"product\":\"PulsePanel Sphere\",\"version\":7,\"wifi\":true,\"pairing\":\"on-device\"}");
}

static bool json_percent(const cJSON *root, const char *name, int *dst)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsNull(v) || !v) { *dst = -1; return true; }
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 100) return false;
    *dst = v->valueint;
    return true;
}

static bool json_metric(const cJSON *root, const char *name, int *dst, int min, int max)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsNull(v) || !v) { *dst = -1; return true; }
    if (!cJSON_IsNumber(v) || v->valuedouble < min || v->valuedouble > max) return false;
    *dst = v->valueint;
    return true;
}

static esp_err_t pc_metrics_http(httpd_req_t *request)
{
    if (setup || !wifi_ready) {
        httpd_resp_send_custom_err(request, "503 Service Unavailable", "Offline");
        return ESP_FAIL;
    }
    int64_t now = esp_timer_get_time();
    if (now < pin_lock_until_us) {
        httpd_resp_set_status(request, "429 Too Many Requests");
        return httpd_resp_sendstr(request, "PIN locked temporarily");
    }
    char pin[20] = {0};
    if (httpd_req_get_hdr_value_str(request, "X-PulsePanel-PIN", pin, sizeof(pin)) != ESP_OK) pin[0] = 0;
    unsigned char diff = 0;
    for (int i = 0; i < 8; i++) diff |= (unsigned char)(pin[i] ^ pairing_pin[i]);
    if (strlen(pin) != 8 || diff != 0 || !pairing_pin[0]) {
        if (++invalid_pin_attempts >= 5) {
            pin_lock_until_us = now + 60000000LL;
            invalid_pin_attempts = 0;
        }
        httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "Incorrect device PIN");
        return ESP_FAIL;
    }
    invalid_pin_attempts = 0;
    if (request->content_len < 2 || request->content_len > 510) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid size");
        return ESP_FAIL;
    }
    char body[512] = {0};
    int total = 0;
    while (total < request->content_len) {
        int got = httpd_req_recv(request, body + total, request->content_len - total);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) return ESP_FAIL;
        total += got;
    }
    cJSON *obj = cJSON_ParseWithLength(body, total);
    pc_telemetry_t snapshot = {.fps=-1,.low=-1,.frame_x10=-1,.cpu=-1,.gpu=-1,.ram=-1,.cpu_temp=-1,.gpu_temp=-1};
    const cJSON *ver = obj ? cJSON_GetObjectItemCaseSensitive(obj, "v") : NULL;
    const cJSON *game = obj ? cJSON_GetObjectItemCaseSensitive(obj, "game") : NULL;
    bool valid = obj && cJSON_IsNumber(ver) && ver->valueint == 1 &&
      json_metric(obj, "fps", &snapshot.fps, 0, 1000) &&
      json_metric(obj, "low", &snapshot.low, 0, 1000) &&
      json_metric(obj, "frame_x10", &snapshot.frame_x10, 0, 30000) &&
      json_percent(obj, "cpu", &snapshot.cpu) &&
      json_percent(obj, "gpu", &snapshot.gpu) &&
      json_percent(obj, "ram", &snapshot.ram) &&
      json_metric(obj, "cpu_temp", &snapshot.cpu_temp, -30, 150) &&
      json_metric(obj, "gpu_temp", &snapshot.gpu_temp, -30, 150);
    if (game && !cJSON_IsNull(game)) {
        valid = valid && cJSON_IsString(game) && game->valuestring && strlen(game->valuestring) <= 32;
        if (valid) {
            for (const char *p = game->valuestring; *p; ++p) {
                if ((unsigned char)*p < 32 || (unsigned char)*p > 126) { valid = false; break; }
            }
            if (valid) snprintf(snapshot.game, sizeof(snapshot.game), "%s", game->valuestring);
        }
    }
    cJSON_Delete(obj);
    if (!valid) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid telemetry");
        return ESP_FAIL;
    }
    snapshot.last_update_us = now;
    portENTER_CRITICAL(&pc_mux);
    pc_data = snapshot;
    portEXIT_CRITICAL(&pc_mux);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

/* Read-only diagnostics. Never return IP, serial, access code or credentials. */
static esp_err_t printer_status_http(httpd_req_t *req)
{
    if (!wifi_ready || setup) {
        httpd_resp_send_custom_err(req, "503 Service Unavailable", "Wi-Fi offline");
        return ESP_FAIL;
    }
    printer_snapshot_t snap;
    printer_monitor_copy(&snap);
    char body[280];
    snprintf(body, sizeof(body),
        "{\"configured\":%s,\"online\":%s,\"phase\":%d,\"progress\":%d}",
        snap.configured ? "true" : "false", snap.connected ? "true" : "false",
        (int)snap.connection_state, snap.connected ? snap.progress : -1);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static void discovery_task(void *arg)
{
    (void)arg;
    while (true) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
        struct sockaddr_in listen_addr = {0};
        listen_addr.sin_family = AF_INET;
        listen_addr.sin_port = htons(UDP_DISCOVERY_PORT);
        listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
            close(sock); vTaskDelay(pdMS_TO_TICKS(5000)); continue;
        }
        while (true) {
            char data[80] = {0};
            struct sockaddr_storage src = {0};
            socklen_t len = sizeof(src);
            int n = recvfrom(sock, data, sizeof(data)-1, 0, (struct sockaddr *)&src, &len);
            if (n > 0 && wifi_ready && !setup && !strcmp(data, "PULSEPANEL_DISCOVER_V1")) {
                char result[128];
                snprintf(result, sizeof(result), "{\"product\":\"PulsePanel Sphere\",\"version\":7,\"ip\":\"%s\"}", ip);
                sendto(sock, result, strlen(result), 0, (struct sockaddr *)&src, len);
            }
        }
    }
}

static void start_server(void)
{
    if (server) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    if (httpd_start(&server, &cfg) != ESP_OK) return;
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_http };
    httpd_uri_t config = { .uri = "/configure", .method = HTTP_POST, .handler = configure_http };
    httpd_uri_t status = { .uri = "/api/v1/status", .method = HTTP_GET, .handler = status_http };
    httpd_uri_t metrics = { .uri = "/api/v1/metrics", .method = HTTP_POST, .handler = pc_metrics_http };
    httpd_uri_t pd = { .uri = "/api/v1/printer/status", .method = HTTP_GET, .handler = printer_status_http };
    httpd_uri_t pg = { .uri = "/printer", .method = HTTP_GET, .handler = printer_get_http };
    httpd_uri_t pp = { .uri = "/printer", .method = HTTP_POST, .handler = printer_post_http };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &config));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &metrics));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &pd));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &pg));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &pp));
    ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captive_http_404));
}

static void start_setup(void)
{
    setup = true;
    wifi_ready = false;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ap_name, sizeof(ap_name), "PulsePanel-%02X%02X", mac[4], mac[5]);
    snprintf(ap_pass, sizeof(ap_pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
    (void)esp_wifi_stop();
    wifi_config_t wifi = {0};
    memcpy(wifi.ap.ssid, ap_name, strlen(ap_name));
    snprintf((char *)wifi.ap.password, sizeof(wifi.ap.password), "%s", ap_pass);
    wifi.ap.ssid_len = strlen(ap_name);
    wifi.ap.channel = 1;
    wifi.ap.max_connection = 2;
    wifi.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    char line1[65], line2[80];
    snprintf(line1, sizeof(line1), "%s", ap_name);
    snprintf(line2, sizeof(line2), "Haslo: %s", ap_pass);
    network_text("KONFIGURACJA WI-FI", line1, line2);
    if (bsp_display_lock(400) == ESP_OK) {
        change_page(PAGE_NETWORK);
        bsp_display_unlock();
    }
    start_server();
}

static void start_setup_task(void *unused)
{
    (void)unused;
    start_setup();
    vTaskDelete(NULL);
}

static void got_wifi(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

static void wifi_events(void *context, esp_event_base_t base, int32_t event, void *data)
{
    (void)context;
    if (base == WIFI_EVENT && event == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event == WIFI_EVENT_STA_DISCONNECTED && !setup) {
        wifi_ready = false;
        printer_monitor_offline();
        if (++disconnects >= 10 && !setup_requested) {
            setup_requested = true;
            xTaskCreate(start_setup_task, "pp_ap", 4096, NULL, 5, NULL);
        } else if (!setup_requested) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && event == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got = data;
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&got->ip_info.ip));
        wifi_ready = true;
        disconnects = 0;
        char line[50];
        snprintf(line, sizeof(line), "IP: %s", ip);
        network_text("WI-FI POLACZONE", line, "Zegar ustawia sie przez internet");
        if (!esp_sntp_enabled()) got_wifi();
        start_server();
        printer_monitor_on_wifi();
        if (printer_choice == -1 && bsp_display_lock(400) == ESP_OK) {
            change_page(PAGE_PRINTER_WELCOME);
            bsp_display_unlock();
        }
    }
}

static void init_network(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_events, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_events, NULL));
    nvs_handle_t h;
    bool configured = false;
    if (nvs_open("pp_config", NVS_READONLY, &h) == ESP_OK) {
        size_t a = sizeof(ssid), b = sizeof(password);
        configured = (nvs_get_str(h, "ssid", ssid, &a) == ESP_OK &&
                      nvs_get_str(h, "pass", password, &b) == ESP_OK && ssid[0]);
        nvs_close(h);
    }
    if (!configured) {
        start_setup();
        return;
    }
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, ssid, strlen(ssid));
    /* ESP-IDF wifi_sta_config_t.password has exactly 64 bytes.
     * A 64-digit hexadecimal PSK is valid and does not fit with a trailing NUL.
     * The structure was zero-initialized above; copy at most the array size.
     */
    size_t wifi_password_len = strnlen(password, sizeof(cfg.sta.password));
    memcpy(cfg.sta.password, password, wifi_password_len);
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    memset(password, 0, sizeof(password));
    network_text("LACZENIE WI-FI", ssid, "Jesli sie nie polaczy: tryb konfiguracji");
}

void app_main(void)
{
    esp_err_t n = nvs_flash_init();
    if (n == ESP_ERR_NVS_NO_FREE_PAGES || n == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        n = nvs_flash_init();
    }
    ESP_ERROR_CHECK(n);
    init_pairing_pin();
    printer_monitor_init();
    nvs_handle_t choice_store;
    if (nvs_open("pp_config",NVS_READONLY,&choice_store)==ESP_OK) {
        uint8_t choice=0;
        if (nvs_get_u8(choice_store,"pr_choice",&choice)==ESP_OK) printer_choice=choice;
        nvs_close(choice_store);
    }
    if (!bsp_display_start()) {
        ESP_LOGE(TAG, "No display (wrong board version?)");
        return;
    }
    (void)bsp_display_brightness_set(68);
    if (bsp_display_lock(1000) != ESP_OK) return;
    create_ui();
    pp_topo_init(lv_screen_active(), 75, topo_allowed);
    bsp_display_unlock();
    init_network();
    if (xTaskCreate(captive_dns_task, "pp_captive_dns", 4096, NULL, 3, NULL) != pdPASS)
        ESP_LOGW(TAG, "Captive DNS task creation failed");
    if (xTaskCreate(discovery_task, "pp_udp", 4096, NULL, 3, NULL) != pdPASS)
        ESP_LOGW(TAG, "Discovery task creation failed");
    unsigned int memory_log_ticks = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        printer_monitor_poll();
        if (++memory_log_ticks >= 15) {
            memory_log_ticks = 0;
            const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            const size_t dma_max = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            ESP_LOGI(TAG, "FIX8 RAM: DMA free=%u largest=%u, PSRAM free=%u",
                     (unsigned)dma_free, (unsigned)dma_max, (unsigned)psram_free);
            if (dma_max < 32768)
                ESP_LOGW(TAG, "FIX8: low contiguous DMA memory; display may stall");
        }
        time_t now;
        time(&now);
        struct tm tm;
        localtime_r(&now, &tm);
        char hour[16] = "--:--", date[70] = "Brak synchronizacji";
        if (tm.tm_year + 1900 >= 2025) {
            strftime(hour, sizeof(hour), "%H:%M", &tm);
            strftime(date, sizeof(date), "%d.%m.%Y", &tm);
        }
        if (focus_running && focus_left > 0) {
            focus_left--;
            if (!focus_left) {
                focus_running = false;
                play_tone();
            }
        }
        if (bsp_display_lock(400) == ESP_OK) {
            /* Only update visible LVGL objects. Other pages refresh on first
             * visible tick, avoiding unnecessary draw buffer pressure. */
            if (!pp_topo_active()) {
            if (page == PAGE_CLOCK) {
                pp_label_set_text_if_changed(time_label, hour);
                pp_label_set_text_if_changed(date_label, date);
            } else if (page == PAGE_FOCUS) {
                pp_label_set_text_fmt_if_changed(focus_label, "%02d:%02d", focus_left / 60, focus_left % 60);
                if (!focus_left) {
                    pp_label_set_text_if_changed(focus_state, "KONIEC SESJI");
                    pp_label_set_text_if_changed(focus_button_text, "OD NOWA");
                }
            } else if (page == PAGE_PC) {
                pc_telemetry_t snap;
                portENTER_CRITICAL(&pc_mux);
                snap = pc_data;
                portEXIT_CRITICAL(&pc_mux);
                bool pc_online = snap.last_update_us > 0 &&
                  (esp_timer_get_time() - snap.last_update_us) < PC_TIMEOUT_MS * 1000LL;
                if (pc_online && snap.fps >= 0) pp_label_set_text_fmt_if_changed(pc_fps, "%d FPS", snap.fps);
                else pp_label_set_text_if_changed(pc_fps, "-- FPS");
                pp_label_set_text_if_changed(pc_game, !pc_online ? "PC: WYMAGA NADAJNIKA" :
                    (snap.game[0] ? snap.game : "PULSEPANEL PC"));
                if (pc_online && snap.frame_x10 >= 0)
                    pp_label_set_text_fmt_if_changed(pc_frametime, "FRAME: %d.%d ms", snap.frame_x10 / 10, snap.frame_x10 % 10);
                else pp_label_set_text_if_changed(pc_frametime, "FRAME: -- ms");
                if (pc_online && snap.low >= 0) pp_label_set_text_fmt_if_changed(pc_low, "LOW~: %d FPS", snap.low);
                else pp_label_set_text_if_changed(pc_low, "LOW~: --");
                if (pc_online && snap.cpu >= 0) pp_label_set_text_fmt_if_changed(pc_cpu, "CPU: %d%%", snap.cpu);
                else pp_label_set_text_if_changed(pc_cpu, "CPU: --");
                if (pc_online && snap.gpu >= 0) pp_label_set_text_fmt_if_changed(pc_gpu, "GPU: %d%%", snap.gpu);
                else pp_label_set_text_if_changed(pc_gpu, "GPU: --");
                if (pc_online && snap.ram >= 0) pp_label_set_text_fmt_if_changed(pc_ram, "RAM: %d%%", snap.ram);
                else pp_label_set_text_if_changed(pc_ram, "RAM: --");
                if (pc_online && (snap.cpu_temp >= 0 || snap.gpu_temp >= 0)) {
                    char temps[80];
                    /* Temperature values must come from sensors, never estimate. */
                    if (snap.cpu_temp >= 0 && snap.gpu_temp >= 0)
                        snprintf(temps, sizeof(temps), "TEMP CPU: %d C   GPU: %d C", snap.cpu_temp, snap.gpu_temp);
                    else if (snap.cpu_temp >= 0)
                        snprintf(temps, sizeof(temps), "TEMP CPU: %d C", snap.cpu_temp);
                    else snprintf(temps, sizeof(temps), "TEMP GPU: %d C", snap.gpu_temp);
                    pp_label_set_text_if_changed(pc_temps, temps);
                } else pp_label_set_text_if_changed(pc_temps, "");
                pp_label_set_text_if_changed(pc_state, pc_online ? "WIFI PC: ONLINE" : "PC: BRAK DANYCH");
            } else if (page == PAGE_PRINTER) {
                printer_snapshot_t printer;
                printer_monitor_copy(&printer);
                pp_label_set_text_if_changed(print_status,
                    !printer.configured ? "DODAJ DRUKARKE" :
                    !printer.connected ? "BAMBU / POLACZENIE" :
                    (printer.state[0] ? printer.state : "DRUKARKA ONLINE"));
                pp_label_set_text_if_changed(print_connect_detail,
                    printer.connected ? "POSTEP WYDRUKU" : printer_monitor_status_text(printer.connection_state));
                const int displayed_progress = printer.connected && printer.progress>=0 ? printer.progress : 0;
                if (lv_arc_get_value(print_progress_arc) != displayed_progress)
                    lv_arc_set_value(print_progress_arc, displayed_progress);
                if (printer.connected && printer.progress >= 0)
                    pp_label_set_text_fmt_if_changed(print_percent,"%d%%",printer.progress);
                else pp_label_set_text_if_changed(print_percent,"--%");
                pp_label_set_text_if_changed(print_file,printer.connected && printer.filename[0] ? printer.filename : "--");
                if (printer.connected && printer.minutes_left>=0)
                    pp_label_set_text_fmt_if_changed(print_time,"Pozostalo: %d min",printer.minutes_left);
                else pp_label_set_text_if_changed(print_time,"Pozostalo: -- min");
                if (printer.connected && printer.nozzle_x10>=0 && printer.bed_x10>=0)
                    pp_label_set_text_fmt_if_changed(print_temps,"Dysza: %d C   Stol: %d C",printer.nozzle_x10/10,printer.bed_x10/10);
                else pp_label_set_text_if_changed(print_temps,"Dysza: --   Stol: --");
                if (printer.connected && printer.layer_current>=0 && printer.layer_total>=0)
                    pp_label_set_text_fmt_if_changed(print_layers,"Warstwa: %d / %d",printer.layer_current,printer.layer_total);
                else pp_label_set_text_if_changed(print_layers,"Warstwa: -- / --");
                if (printer.connected && printer.tray_now>=0 && printer.tray_now<254)
                    pp_label_set_text_fmt_if_changed(print_ams,"AMS: slot %d",printer.tray_now);
                else pp_label_set_text_if_changed(print_ams,"AMS: --");
            }
            } /* Screensaver hides background updates */
            bsp_display_unlock();
        }
    }
}
