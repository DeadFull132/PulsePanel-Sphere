/* PulsePanel Sphere: optional, independently implemented Bambu Cloud cover POC.
 * The account API is UNOFFICIAL, subject to permission, rate limits, changes.
 * There is NO automatic account login or password collection in this module.
 * Stores an existing access token in ordinary NVS (not production safe).
 * Fetches a cloud cover only while the cover screen is visible, at most every
 * 15 minutes. JPG/PNG accepted if the cover fits memory limits.
 */
#include "bambu_cloud_cover.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "png.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "nvs.h"
#include "cJSON.h"
#include "printer_monitor.h"

#define CLOUD_JSON_MAX 20000U
#define CLOUD_JPEG_MAX (220U * 1024U)
#define CLOUD_IMG_SIDE 160U
#define CLOUD_PSRAM_RESERVE (2U * 1024U * 1024U)
#define CLOUD_DMA_RESERVE (48U * 1024U)
#define CLOUD_REQUEST_INTERVAL_MS (15U * 60U * 1000U)
#define CLOUD_MAX_TOKEN_LEN 1799U
static const char *TAG = "PP_CLOUD_COVER";
static bool s_started=false;
static volatile bool s_wifi=false,s_active=false;
static pp_cloud_state_t s_state=PP_CLOUD_DISABLED;
static portMUX_TYPE s_mu=portMUX_INITIALIZER_UNLOCKED;
static pp_cloud_frame_t s_ready={0};
static uint32_t s_last_attempt=0;

typedef struct { uint8_t *buffer; size_t capacity,used; bool too_large; } http_sink_t;
static void state_set(pp_cloud_state_t st) {
  portENTER_CRITICAL(&s_mu); s_state=st; portEXIT_CRITICAL(&s_mu);
}
pp_cloud_state_t pp_cloud_get_state(void) {
  portENTER_CRITICAL(&s_mu); pp_cloud_state_t v=s_state; portEXIT_CRITICAL(&s_mu);return v;
}
const char *pp_cloud_state_text(pp_cloud_state_t s) {
  switch(s){
    case PP_CLOUD_DISABLED:return "CHMURA WYLACZONA";
    case PP_CLOUD_NO_TOKEN:return "BRAK TOKENU CLOUD";
    case PP_CLOUD_NO_WIFI:return "BRAK WI-FI";
    case PP_CLOUD_WAIT_TIME:return "CZEKAM NA CZAS NTP";
    case PP_CLOUD_WAIT_PRINTER:return "DODAJ DRUKARKE A1 MINI";
    case PP_CLOUD_LOADING:return "POBIERAM MINIATURE";
    case PP_CLOUD_READY:return "OKLADKA Z CHMURY";
    case PP_CLOUD_NO_COVER:return "BRAK MINIATURY W CLOUD";
    case PP_CLOUD_AUTH_FAILED:return "TOKEN WYGASL / BRAK DOSTEPU";
    case PP_CLOUD_FORMAT_UNSUPPORTED:return "INNY FORMAT NIZ JPEG";
    case PP_CLOUD_LOW_MEMORY:return "ZA MALO PAMIECI DMA/PSRAM";
    case PP_CLOUD_RATE_LIMITED:return "LIMIT ZAPYTAN CLOUD";
    default:return "BLAD BAMBULAB CLOUD";
  }
}
void pp_cloud_release(pp_cloud_frame_t *f) {
  if(!f)return; if(f->pixels)heap_caps_free(f->pixels);memset(f,0,sizeof(*f));
}
bool pp_cloud_take(pp_cloud_frame_t *out) {
  if(!out)return false;
  portENTER_CRITICAL(&s_mu);
  bool valid=s_ready.pixels!=NULL;
  if(valid){ *out=s_ready;memset(&s_ready,0,sizeof(s_ready)); }
  portEXIT_CRITICAL(&s_mu);return valid;
}
static bool mem_ok(void){
 return heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>CLOUD_PSRAM_RESERVE &&
   heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA)>CLOUD_DMA_RESERVE;
}
static esp_err_t http_event(esp_http_client_event_t *evt){
  if(evt->event_id!=HTTP_EVENT_ON_DATA)return ESP_OK;
  http_sink_t *dst=(http_sink_t *)evt->user_data;
  if(!dst || !evt->data || evt->data_len<0)return ESP_FAIL;
  if((size_t)evt->data_len>dst->capacity-dst->used){dst->too_large=true;return ESP_FAIL;}
  memcpy(dst->buffer+dst->used,evt->data,(size_t)evt->data_len);
  dst->used+=(size_t)evt->data_len;return ESP_OK;
}
/* Return HTTP response status, 0 on transport errors. Caller bounds output. */
static int get_https(const char *url,const char *token,http_sink_t *dst){
  esp_http_client_config_t cfg={
    .url=url,.method=HTTP_METHOD_GET,.timeout_ms=9000,
    .event_handler=http_event,.user_data=dst,
    .crt_bundle_attach=esp_crt_bundle_attach,
    .disable_auto_redirect=true,
    .buffer_size=2048
  };
  esp_http_client_handle_t c=esp_http_client_init(&cfg);
  if(!c)return 0;
  esp_http_client_set_header(c,"User-Agent","PulsePanel-Sphere-Experimental/0.1");
  char *auth=NULL;
  size_t authsize=0;
  if(token){
    /* Do not print cloud credentials to logs or a public repository. */
    size_t n=strlen(token);authsize=n+8;
    auth=heap_caps_malloc(authsize,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!auth){esp_http_client_cleanup(c);return 0;}
    memcpy(auth,"Bearer ",7);memcpy(auth+7,token,n+1);
    esp_http_client_set_header(c,"Authorization",auth);
  }
  esp_err_t ret=esp_http_client_perform(c);
  int status=(ret==ESP_OK && !dst->too_large)?esp_http_client_get_status_code(c):0;
  esp_http_client_cleanup(c);
  if(auth){memset(auth,0,authsize);heap_caps_free(auth);}
  return status;
}
static bool allowed_cover_domain(const char *url){
  if(strncmp(url,"https://",8)!=0)return false;
  const char *h=url+8;const char *end=strpbrk(h,"/:?#");
  size_t n=end?(size_t)(end-h):strlen(h);
  if(n<8||n>200)return false;
  char domain[202];memcpy(domain,h,n);domain[n]=0;
  /* Reject credentials, IP literals and unsolicited endpoints. */
  if(strchr(domain,'@')||strchr(domain,'['))return false;
  const char *suffixes[]={".aliyuncs.com",".bambulab.com",".makerworld.com"};
  for(size_t i=0;i<sizeof(suffixes)/sizeof(suffixes[0]);i++){
    size_t m=strlen(suffixes[i]);
    if(n>m && strcmp(domain+n-m,suffixes[i])==0)return true;
  }
  return false;
}
static bool load_token(char *out,size_t max){
  nvs_handle_t h;
  if(nvs_open("pp_config",NVS_READONLY,&h)!=ESP_OK)return false;
  size_t n=max;esp_err_t r=nvs_get_str(h,"cloud_token",out,&n);
  nvs_close(h);return r==ESP_OK && n>1 && n<max;
}
bool pp_cloud_has_token(void){
  nvs_handle_t h;
  if(nvs_open("pp_config",NVS_READONLY,&h)!=ESP_OK)return false;
  size_t n=0;esp_err_t e=nvs_get_str(h,"cloud_token",NULL,&n);nvs_close(h);
  return e==ESP_OK && n>1 && n<CLOUD_MAX_TOKEN_LEN+2;
}
bool pp_cloud_set_token(const char *token){
  if(!token)return false;
  size_t n=strnlen(token,CLOUD_MAX_TOKEN_LEN+1);
  if(n<40||n>CLOUD_MAX_TOKEN_LEN)return false;
  for(size_t i=0;i<n;i++){
    unsigned char c=(unsigned char)token[i];
    if(c<=32||c>=127)return false;
  }
  nvs_handle_t h;if(nvs_open("pp_config",NVS_READWRITE,&h)!=ESP_OK)return false;
  esp_err_t e=nvs_set_str(h,"cloud_token",token);
  if(e==ESP_OK)e=nvs_commit(h);
  nvs_close(h);s_last_attempt=0;return e==ESP_OK;
}
void pp_cloud_forget_token(void){
  nvs_handle_t h;if(nvs_open("pp_config",NVS_READWRITE,&h)==ESP_OK){
    (void)nvs_erase_key(h,"cloud_token");(void)nvs_commit(h);nvs_close(h);
  }
  s_last_attempt=0;state_set(PP_CLOUD_NO_TOKEN);
}
static bool read_printer_serial(char *out,size_t limit){
  nvs_handle_t h;if(nvs_open("pp_config",NVS_READONLY,&h)!=ESP_OK)return false;
  size_t n=limit;esp_err_t e=nvs_get_str(h,"pr_serial",out,&n);nvs_close(h);
  if(e!=ESP_OK||n<6||n>=limit)return false;
  for(size_t i=0;out[i];i++){
    if(!((out[i]>='A'&&out[i]<='Z')||(out[i]>='a'&&out[i]<='z')||
      (out[i]>='0'&&out[i]<='9')||out[i]=='-'||out[i]=='_'))return false;
  }
  return true;
}
static bool jpeg_probe(const uint8_t *p,size_t size,uint16_t *w,uint16_t *h){
 if(size<8||p[0]!=255||p[1]!=0xd8||p[size-2]!=255||p[size-1]!=0xd9)return false;
 for(size_t i=2;i+4<size;){
  if(p[i++]!=255)return false;
  while(i<size&&p[i]==255)i++;
  if(i>=size)return false;
  uint8_t kind=p[i++]; if(kind==0xda)break;
  if(kind==0xd8||kind==0xd9||kind==1||(kind>=0xd0&&kind<=0xd7))continue;
  if(i+2>size)return false;
  size_t len=((size_t)p[i]<<8)|p[i+1];if(len<2||len>size-i)return false;
  if(kind==0xc0 && len>=7){
   uint16_t H=((uint16_t)p[i+3]<<8)|p[i+4],W=((uint16_t)p[i+5]<<8)|p[i+6];
   if(W<20||H<20||W>2048||H>2048)return false;
   *w=W;*h=H;return true;
  }
  i+=len;
 }
 return false;
}
static bool decode_jpeg(const uint8_t *jpg,size_t n,pp_cloud_frame_t *out){
 uint16_t srcw=0,srch=0;
 if(!jpeg_probe(jpg,n,&srcw,&srch))return false;
 uint32_t divisor=(srcw>srch?srcw:srch);
 uint32_t factor=(CLOUD_IMG_SIDE*256U)/divisor;
 if(factor>256)factor=256;
 uint16_t w=(uint16_t)((srcw*factor/256)&~7U);
 uint16_t h=(uint16_t)((srch*factor/256)&~7U);
 if(w<16||h<16||w>CLOUD_IMG_SIDE||h>CLOUD_IMG_SIDE)return false;
 jpeg_dec_config_t cfg=DEFAULT_JPEG_DEC_CONFIG();
 cfg.output_type=JPEG_PIXEL_FORMAT_RGB565_LE;
 cfg.scale.width=w;cfg.scale.height=h;
 jpeg_dec_handle_t dh=NULL;
 if(jpeg_dec_open(&cfg,&dh)!=JPEG_ERR_OK)return false;
 jpeg_dec_io_t io={.inbuf=(uint8_t*)jpg,.inbuf_len=(int)n};
 jpeg_dec_header_info_t info={0};
 uint8_t *buf=NULL;bool ok=false;
 do{
  if(jpeg_dec_parse_header(dh,&io,&info)!=JPEG_ERR_OK)break;
  if(info.width!=srcw||info.height!=srch)break;
  int bytes=0;
  if(jpeg_dec_get_outbuf_len(dh,&bytes)!=JPEG_ERR_OK)break;
  if(bytes<(int)(w*h*2U)||bytes>(int)(CLOUD_IMG_SIDE*CLOUD_IMG_SIDE*2U))break;
  buf=heap_caps_aligned_alloc(16,((size_t)bytes+15U)&~15U,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!buf)break;
  io.outbuf=buf;
  if(jpeg_dec_process(dh,&io)!=JPEG_ERR_OK)break;
  out->pixels=buf;out->size=(size_t)bytes;
  out->width=w;out->height=h;buf=NULL;ok=true;
 }while(0);
 if(buf)heap_caps_free(buf);
 (void)jpeg_dec_close(dh);return ok;
}
/* Simplified libpng API: decode in PSRAM, then nearest-neighbor shrink to
 * 160x160 RGB565. No full-size image is handed to LVGL or SPI DMA. */
static bool decode_png(const uint8_t *bytes,size_t n,pp_cloud_frame_t *out)
{
  if(n<24 || memcmp(bytes,"\x89PNG\r\n\x1a\n",8)!=0)return false;
  png_image im={0};im.version=PNG_IMAGE_VERSION;
  if(!png_image_begin_read_from_memory(&im,bytes,n))return false;
  bool ok=false;
  uint8_t *rgb=NULL,*small=NULL;
  do {
    if(im.width<24 || im.height<24 || im.width>768 || im.height>768)break;
    size_t srcbytes=(size_t)im.width*im.height*3U;
    size_t free_psram=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if(free_psram<=CLOUD_PSRAM_RESERVE || srcbytes>free_psram-CLOUD_PSRAM_RESERVE)break;
    rgb=heap_caps_malloc(srcbytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!rgb)break;
    im.format=PNG_FORMAT_RGB;
    if(!png_image_finish_read(&im,NULL,rgb,0,NULL))break;
    unsigned short w=(im.width>CLOUD_IMG_SIDE)?CLOUD_IMG_SIDE:(unsigned short)im.width;
    unsigned short h=(im.height>CLOUD_IMG_SIDE)?CLOUD_IMG_SIDE:(unsigned short)im.height;
    size_t sz=(size_t)w*h*2U;
    small=heap_caps_malloc(sz,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!small)break;
    for(unsigned y=0;y<h;y++){
      unsigned sy=(unsigned)((uint64_t)y*im.height/h);
      for(unsigned x=0;x<w;x++){
        unsigned sx=(unsigned)((uint64_t)x*im.width/w);
        const uint8_t *p=rgb+3U*((size_t)sy*im.width+sx);
        uint16_t v=(uint16_t)(((uint16_t)(p[0]>>3)<<11)|((uint16_t)(p[1]>>2)<<5)|(p[2]>>3));
        size_t at=2U*((size_t)y*w+x);
        small[at]=(uint8_t)v;small[at+1]=(uint8_t)(v>>8);
      }
    }
    out->pixels=small;out->size=sz;out->width=w;out->height=h;
    small=NULL;ok=true;
  }while(0);
  if(rgb)heap_caps_free(rgb);
  if(small)heap_caps_free(small);
  png_image_free(&im);
  return ok;
}

static void submit(pp_cloud_frame_t *incoming){
 portENTER_CRITICAL(&s_mu);
 pp_cloud_frame_t old=s_ready;s_ready=*incoming;
 memset(incoming,0,sizeof(*incoming));
 portEXIT_CRITICAL(&s_mu);pp_cloud_release(&old);
}
static void fetch_cloud_cover(void){
 if(!mem_ok()){state_set(PP_CLOUD_LOW_MEMORY);return;}
 char serial[41]={0};
 if(!read_printer_serial(serial,sizeof(serial))){state_set(PP_CLOUD_WAIT_PRINTER);return;}
 char *token=heap_caps_calloc(1,CLOUD_MAX_TOKEN_LEN+2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!token){state_set(PP_CLOUD_LOW_MEMORY);return;}
 if(!load_token(token,CLOUD_MAX_TOKEN_LEN+2)){
    heap_caps_free(token);state_set(PP_CLOUD_NO_TOKEN);return;
 }
 state_set(PP_CLOUD_LOADING);
 uint8_t *json=heap_caps_malloc(CLOUD_JSON_MAX+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!json){state_set(PP_CLOUD_LOW_MEMORY);goto finish;}
 char endpoint[180];
 snprintf(endpoint,sizeof(endpoint),"https://api.bambulab.com/v1/user-service/my/tasks?deviceId=%s&limit=1",serial);
 http_sink_t j={.buffer=json,.capacity=CLOUD_JSON_MAX,.used=0};
 int status=get_https(endpoint,token,&j);
 if(status==401||status==403){state_set(PP_CLOUD_AUTH_FAILED);goto clean_json;}
 if(status==429){state_set(PP_CLOUD_RATE_LIMITED);goto clean_json;}
 if(status!=200){state_set(PP_CLOUD_HTTP_ERROR);goto clean_json;}
 json[j.used]=0;
 cJSON *root=cJSON_Parse((char *)json);
 if(!root){state_set(PP_CLOUD_HTTP_ERROR);goto clean_json;}
 cJSON *items=cJSON_GetObjectItemCaseSensitive(root,"hits");
 if(!cJSON_IsArray(items))items=cJSON_GetObjectItemCaseSensitive(root,"tasks");
 cJSON *item=cJSON_IsArray(items)?cJSON_GetArrayItem(items,0):NULL;
 cJSON *device=cJSON_GetObjectItemCaseSensitive(item,"deviceId");
 cJSON *cover=cJSON_GetObjectItemCaseSensitive(item,"cover");
 if(!cJSON_IsString(cover))cover=cJSON_GetObjectItemCaseSensitive(item,"thumbnail");
 char url[1000]={0};
 bool has_cover=cJSON_IsString(device)&&device->valuestring&&strcmp(device->valuestring,serial)==0&&
  cJSON_IsString(cover)&&cover->valuestring&&strlen(cover->valuestring)<sizeof(url)&&
  allowed_cover_domain(cover->valuestring);
 if(has_cover)snprintf(url,sizeof(url),"%s",cover->valuestring);
 cJSON_Delete(root);
 if(!has_cover){state_set(PP_CLOUD_NO_COVER);goto clean_json;}
 if(!mem_ok()){state_set(PP_CLOUD_LOW_MEMORY);goto clean_json;}
 uint8_t *jpg=heap_caps_malloc(CLOUD_JPEG_MAX,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!jpg){state_set(PP_CLOUD_LOW_MEMORY);goto clean_json;}
 http_sink_t im={.buffer=jpg,.capacity=CLOUD_JPEG_MAX,.used=0};
 int code=get_https(url,NULL,&im);
 if(code!=200){state_set(PP_CLOUD_HTTP_ERROR);heap_caps_free(jpg);goto clean_json;}
 pp_cloud_frame_t decoded={0};
 bool decoded_ok=false;
 if(im.used>=8 && memcmp(jpg,"\x89PNG\r\n\x1a\n",8)==0)
   decoded_ok=decode_png(jpg,im.used,&decoded);
 else
   decoded_ok=decode_jpeg(jpg,im.used,&decoded);
 if(!decoded_ok)state_set(PP_CLOUD_FORMAT_UNSUPPORTED);
 else {submit(&decoded);state_set(PP_CLOUD_READY);}
 heap_caps_free(jpg);
 clean_json:heap_caps_free(json);
 finish:memset(token,0,strlen(token));heap_caps_free(token);
}
static void cloud_worker(void *arg){
 (void)arg;
 for(;;){
  if(!s_active){vTaskDelay(pdMS_TO_TICKS(750));continue;}
  if(!s_wifi){state_set(PP_CLOUD_NO_WIFI);vTaskDelay(pdMS_TO_TICKS(1500));continue;}
  if(!printer_monitor_configured()){state_set(PP_CLOUD_WAIT_PRINTER);vTaskDelay(pdMS_TO_TICKS(2500));continue;}
  if(time(NULL)<1704067200){state_set(PP_CLOUD_WAIT_TIME);vTaskDelay(pdMS_TO_TICKS(2500));continue;}
  if(!pp_cloud_has_token()){state_set(PP_CLOUD_NO_TOKEN);vTaskDelay(pdMS_TO_TICKS(2500));continue;}
  uint32_t now=xTaskGetTickCount()*portTICK_PERIOD_MS;
  if(!s_last_attempt || (uint32_t)(now-s_last_attempt)>CLOUD_REQUEST_INTERVAL_MS){
    s_last_attempt=now ? now : 1;
    fetch_cloud_cover();
  }
  vTaskDelay(pdMS_TO_TICKS(1000));
 }
}
void pp_cloud_wifi(bool enabled){s_wifi=enabled;}
void pp_cloud_set_active(bool active){s_active=active;if(!active)state_set(PP_CLOUD_DISABLED);}
void pp_cloud_init(void){
 if(s_started)return;
 if(xTaskCreatePinnedToCore(cloud_worker,"pp_cloud_preview",12288,NULL,2,NULL,0)==pdPASS)
   s_started=true;
 else ESP_LOGE(TAG,"Cloud task creation failed");
}
