/* PulsePanel Sphere / Topo 466: memory-conservative procedural screensaver.
 * The 4K reference is an artistic reference only: hardware is 466 x 466.
 * No frame images, file decoder, network streaming or per-frame allocations.
 */
#include "topo_saver.h"
#include <math.h>
#include <stdint.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "bsp/display.h"

#define TOP_LAYERS 15
#define TOP_POINTS 49
#define TOP_SIDE 466
#define TOP_IDLE_US (120LL * 1000000LL)
#define TOP_BLACK_US (600LL * 1000000LL)
#define TOP_FRAME_US (400LL * 1000LL) /* 2.5 slow steps per second */
#define TOP_MIN_DMA 32768U
#define PI2 6.28318530718f

static const char *TOPO_TAG = "PPTopo";
static lv_obj_t *topo_surface;
static lv_obj_t *contour[TOP_LAYERS];
static lv_point_precise_t coords[TOP_LAYERS][TOP_POINTS];
static bool sleeping=false;
static bool fully_dark=false;
static bool finger_was_down=false;
static uint8_t saved_brightness=65;
static int64_t activity_us=0, animation_us=0;
static bool (*allowed_fn)(void);

static void topo_wake(void)
{
    if (!sleeping) return;
    sleeping=false;
    fully_dark=false;
    activity_us=esp_timer_get_time();
    (void)bsp_display_brightness_set(saved_brightness);
    lv_obj_add_flag(topo_surface, LV_OBJ_FLAG_HIDDEN);
}

static void on_press(lv_event_t *event)
{
    (void)event;
    topo_wake();
    lv_indev_t *indev=lv_indev_active();
    if (indev) lv_indev_wait_release(indev); /* consume first touch */
}

static void draw_contours(int64_t now_us)
{
    /* Once DMA is pressured, leave the current frame still. Do not
     * allocate another frame and do not bring back ESP_ERR_NO_MEM. */
    if (heap_caps_get_largest_free_block(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL)<TOP_MIN_DMA)
        return;
    /* Seamless, slowly changing contour groups (unique/generated artwork).
     * Always update static arrays only, never realloc points. */
    float phase=(float)(now_us % 24000000LL)/24000000.0f*PI2;
    for (int i=0; i<TOP_LAYERS; ++i) {
        int group=(i<7) ? 0 : ((i<11) ? 1 : 2);
        int band=(group==0) ? i : ((group==1) ? i-7 : i-11);
        float cx=(group==0) ? 197.0f : ((group==1) ? 399.0f : 5.0f);
        float cy=(group==0) ? 151.0f : ((group==1) ? 343.0f : 394.0f);
        float base=(group==0) ? 49.0f+band*34.0f : 42.0f+band*35.0f;
        float shift=(group==0 ? 1.0f : (group==1 ? -1.0f : 2.0f));
        for(int j=0;j<TOP_POINTS-1;++j) {
            float a=PI2*(float)j/(TOP_POINTS-1);
            float bump=1.0f + 0.16f*sinf(3.0f*a+phase*shift+group) +
                       0.08f*cosf(5.0f*a-phase+group*2.0f);
            float r=base*bump;
            coords[i][j].x=(int16_t)(cx+r*cosf(a)+11.0f*sinf(phase+group));
            coords[i][j].y=(int16_t)(cy+r*sinf(a)+8.0f*cosf(phase+group));
        }
        coords[i][TOP_POINTS-1]=coords[i][0];
        /* Resend same fixed array. LVGL holds only the pointer. */
        lv_line_set_points(contour[i], coords[i], TOP_POINTS);
    }
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (!topo_surface) return;
    int64_t now=esp_timer_get_time();
    bool down=false;
    lv_indev_t *pressed_device=NULL;
    for(lv_indev_t *indev=lv_indev_get_next(NULL); indev; indev=lv_indev_get_next(indev)) {
        if(lv_indev_get_type(indev)==LV_INDEV_TYPE_POINTER &&
           lv_indev_get_state(indev)==LV_INDEV_STATE_PRESSED) {
            down=true;
            pressed_device=indev;
            break;
        }
    }
    if(down) {
        activity_us=now;
        if(sleeping) {
            topo_wake();
            if(pressed_device) lv_indev_wait_release(pressed_device);
        }
    }
    finger_was_down=down;
    (void)finger_was_down;
    if(!sleeping) {
        if(allowed_fn && !allowed_fn()) { activity_us=now; return; }
        if(now-activity_us<TOP_IDLE_US) return;
        sleeping=true;
        fully_dark=false;
        animation_us=0;
        lv_obj_remove_flag(topo_surface, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(topo_surface);
        (void)bsp_display_brightness_set(12);
        ESP_LOGI(TOPO_TAG, "Topographic screensaver enabled; DMA-safe line mode");
    }
    if(now-activity_us >= TOP_BLACK_US) {
        if(!fully_dark) {
            fully_dark=true;
            (void)bsp_display_brightness_set(0);
            ESP_LOGI(TOPO_TAG,"AMOLED off after extended idle");
        }
        return;
    }
    if(!fully_dark && (animation_us==0 || now-animation_us>=TOP_FRAME_US)) {
        draw_contours(now);
        animation_us=now;
    }
}

void pp_topo_init(lv_obj_t *screen, uint8_t normal_brightness,
                  bool (*may_enter)(void))
{
    allowed_fn=may_enter;
    saved_brightness=normal_brightness;
    activity_us=esp_timer_get_time();
    topo_surface=lv_obj_create(screen);
    lv_obj_set_size(topo_surface, TOP_SIDE, TOP_SIDE);
    lv_obj_align(topo_surface,LV_ALIGN_CENTER,0,0);
    lv_obj_set_style_bg_color(topo_surface,lv_color_black(),0);
    lv_obj_set_style_bg_opa(topo_surface,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(topo_surface,0,0);
    lv_obj_set_style_radius(topo_surface,0,0);
    lv_obj_set_style_pad_all(topo_surface,0,0);
    lv_obj_remove_flag(topo_surface,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(topo_surface,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(topo_surface,on_press,LV_EVENT_PRESSED,NULL);
    for(int i=0;i<TOP_LAYERS;i++) {
        contour[i]=lv_line_create(topo_surface);
        lv_obj_set_size(contour[i],TOP_SIDE,TOP_SIDE);
        lv_obj_align(contour[i],LV_ALIGN_TOP_LEFT,0,0);
        lv_obj_set_style_line_color(contour[i],lv_color_white(),0);
        lv_obj_set_style_line_width(contour[i],2,0);
        lv_obj_set_style_line_opa(contour[i],LV_OPA_80,0);
        lv_obj_set_style_bg_opa(contour[i],LV_OPA_TRANSP,0);
        lv_obj_remove_flag(contour[i],LV_OBJ_FLAG_CLICKABLE);
        lv_line_set_points(contour[i],coords[i],TOP_POINTS);
    }
    lv_obj_add_flag(topo_surface,LV_OBJ_FLAG_HIDDEN);
    (void)lv_timer_create(tick,50,NULL); /* poll touch cheaply; animate at 2.5 fps */
}

bool pp_topo_active(void) { return sleeping; }
void pp_topo_set_normal_brightness(uint8_t value) { saved_brightness=value; }
