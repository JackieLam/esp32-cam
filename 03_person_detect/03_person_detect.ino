// Demo 3: 实时画面 + 识别画面里有没有人（浏览器端 TensorFlow.js COCO-SSD）
// 硬件: DFRobot ESP32-S3 AI CAM (DFR1154)，摄像头 GC2145
// 注意: GC2145 不支持硬件 JPEG，这里用 RGB565 采集后软件转 JPEG 再推流
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"

// ====== WiFi 配置在 secrets.h（不进 git）======
// 首次使用: cp secrets.h.example secrets.h，然后填入你的 WiFi
#include "secrets.h"

// ====== DFR1154 摄像头引脚（来自 DFRobot 官方 wiki） ======
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM   5
#define SIOD_GPIO_NUM   8
#define SIOC_GPIO_NUM   9
#define Y9_GPIO_NUM     4
#define Y8_GPIO_NUM     6
#define Y7_GPIO_NUM     7
#define Y6_GPIO_NUM    14
#define Y5_GPIO_NUM    17
#define Y4_GPIO_NUM    21
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM    16
#define VSYNC_GPIO_NUM  1
#define HREF_GPIO_NUM   2
#define PCLK_GPIO_NUM  15

#define LED_PIN 3

// ====== 画质 / 帧率调节 ======
// 分辨率越高、JPEG 质量越高 → 越清晰但帧率越低（GC2145 靠 CPU 软件压缩 JPEG）
// 网页上可以实时切换；这里是开机默认值
#define DEFAULT_SIZE   "hvga"
#define DEFAULT_QUALITY 75              // 0~100
static volatile int jpeg_quality = DEFAULT_QUALITY;
static const char *cur_size = "qvga";   // 开机先按 qvga 初始化，再切到 DEFAULT_SIZE

struct SizeOpt { const char *name; framesize_t fs; };
static const SizeOpt SIZES[] = {
  {"qvga", FRAMESIZE_QVGA},  // 320x240
  {"hvga", FRAMESIZE_HVGA},  // 480x320
  {"vga",  FRAMESIZE_VGA},   // 640x480
  {"svga", FRAMESIZE_SVGA},  // 800x600
};

// RGB565 模式下驱动不支持运行时 set_framesize，只能重新初始化摄像头；
// 用一把锁保证重新初始化时没有视频流在取帧
static SemaphoreHandle_t cam_lock;
static bool camera_init(framesize_t fs);   // 定义在下面

static bool apply_size(const char *name) {
  for (auto &o : SIZES) {
    if (strcmp(o.name, name) == 0) {
      if (strcmp(cur_size, name) == 0) return true;
      xSemaphoreTake(cam_lock, portMAX_DELAY);
      esp_camera_deinit();
      bool ok = camera_init(o.fs);
      if (ok) cur_size = o.name;
      else { camera_init(FRAMESIZE_QVGA); cur_size = "qvga"; }  // 失败就退回最小档
      xSemaphoreGive(cam_lock);
      return ok;
    }
  }
  return false;
}

static const char *BOUNDARY = "frame";

// GET /  —— 视频流 + 浏览器端人体识别（TensorFlow.js COCO-SSD）
// 识别在打开网页的电脑/手机上运行，板子只负责推流
static esp_err_t index_handler(httpd_req_t *req) {
  static const char html[] = R"rawliteral(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Person Detector</title>
<style>
  body{margin:0;background:#111;color:#eee;font-family:-apple-system,"PingFang SC",sans-serif;text-align:center}
  #banner{font-size:28px;font-weight:700;padding:14px;transition:background .2s}
  #banner.yes{background:#1b8a3a} #banner.no{background:#333} #banner.wait{background:#555}
  #wrap{position:relative;display:inline-block;max-width:100%;margin-top:10px}
  #v{display:block;max-width:100%;width:800px}
  #c{position:absolute;left:0;top:0;width:100%;height:100%;pointer-events:none}
  #info{color:#aaa;font-size:14px;margin:8px}
  label,.ctl{font-size:14px;color:#ccc} .ctl{margin:6px} select{font-size:14px} a{color:#8cf}
</style></head>
<body>
<div id="banner" class="wait">正在加载识别模型…</div>
<div id="wrap"><img id="v" src="/stream"><canvas id="c"></canvas></div>
<div id="info">-</div>
<div class="ctl">清晰度
  <select id="size">
    <option value="qvga">320×240（最流畅）</option>
    <option value="hvga">480×320（均衡）</option>
    <option value="vga">640×480（清晰）</option>
    <option value="svga">800×600（最清晰，较卡）</option>
  </select>
  画质 <select id="q"><option>50</option><option>65</option><option>75</option><option>85</option><option>95</option></select>
</div>
<label><input type="checkbox" id="all"> 显示所有识别到的物体</label> · <a href="/capture">拍一张照片</a>
<script src="https://cdn.jsdelivr.net/npm/@tensorflow/tfjs@4.22.0/dist/tf.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/@tensorflow-models/coco-ssd@2.2.3/dist/coco-ssd.min.js"></script>
<script>
const v=document.getElementById('v'), c=document.getElementById('c'), ctx=c.getContext('2d');
const banner=document.getElementById('banner'), info=document.getElementById('info'), all=document.getElementById('all');
const MIN_SCORE=0.5;
const sizeSel=document.getElementById('size'), qSel=document.getElementById('q');
async function control(qs){
  const r=await (await fetch('/control?'+qs)).json();
  sizeSel.value=r.size; qSel.value=String(r.q);
}
control('');  // 读取当前设置
sizeSel.onchange=()=>control('size='+sizeSel.value);
qSel.onchange=()=>control('q='+qSel.value);
// 要识别的目标：COCO 80 类之一，例如 'person' 'dog' 'cat' 'car' 'bicycle'
const TARGET='person', TARGET_CN='人', UNIT='个', EMOJI='🧍';
let lastHit=0;
function draw(preds){
  c.width=v.naturalWidth; c.height=v.naturalHeight;
  ctx.clearRect(0,0,c.width,c.height);
  ctx.lineWidth=Math.max(2,c.width/160); ctx.font=`${Math.max(12,c.width/26)}px sans-serif`;
  for(const p of preds){
    const hit=p.class===TARGET;
    if(!hit && !all.checked) continue;
    const [x,y,w,h]=p.bbox, color=hit?'#2ee66b':'#f5c542';
    ctx.strokeStyle=color; ctx.strokeRect(x,y,w,h);
    const label=`${hit?TARGET_CN:p.class} ${(p.score*100).toFixed(0)}%`;
    const tw=ctx.measureText(label).width+8, th=parseInt(ctx.font)+6;
    ctx.fillStyle=color; ctx.fillRect(x,Math.max(0,y-th),tw,th);
    ctx.fillStyle='#000'; ctx.fillText(label,x+4,Math.max(0,y-th)+th-6);
  }
}
async function main(){
  const model=await cocoSsd.load({base:'lite_mobilenet_v2'});
  banner.textContent='模型已加载，等待画面…';
  let frames=0, t0=performance.now();
  while(true){
    if(v.complete && v.naturalWidth>0){
      try{
        const preds=await model.detect(v,10,MIN_SCORE);
        draw(preds);
        const hits=preds.filter(p=>p.class===TARGET);
        if(hits.length) lastHit=performance.now();
        // 0.5 秒内见过目标就算"有"，避免单帧漏检导致闪烁
        const has=performance.now()-lastHit<500;
        banner.className=has?'yes':'no';
        banner.textContent=has?`${EMOJI} 有${TARGET_CN}！（${hits.length||'…'} ${UNIT}）`:`没有${TARGET_CN}`;
        frames++;
        const dt=performance.now()-t0;
        if(dt>1000){ info.textContent=`识别速度 ${(frames*1000/dt).toFixed(1)} 次/秒 · 画面 ${v.naturalWidth}×${v.naturalHeight}`; frames=0; t0=performance.now(); }
      }catch(e){ info.textContent='识别出错: '+e.message+'（请用 Chrome 打开）'; }
    }
    await new Promise(r=>setTimeout(r,66));  // 最多约 15 次/秒，视频本身也就 ~10fps，再快只是浪费 CPU
  }
}
main().catch(e=>{banner.className='wait';banner.textContent='模型加载失败: '+e.message;});
</script>
</body></html>)rawliteral";
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// 取一帧并转成 JPEG
static bool grab_jpeg(uint8_t **jpg, size_t *len) {
  xSemaphoreTake(cam_lock, portMAX_DELAY);
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { xSemaphoreGive(cam_lock); return false; }
  bool ok = frame2jpg(fb, jpeg_quality, jpg, len);
  esp_camera_fb_return(fb);
  xSemaphoreGive(cam_lock);
  return ok;
}

// GET /capture —— 单张照片
static esp_err_t capture_handler(httpd_req_t *req) {
  uint8_t *jpg = nullptr; size_t len = 0;
  if (!grab_jpeg(&jpg, &len)) { httpd_resp_send_500(req); return ESP_FAIL; }
  httpd_resp_set_type(req, "image/jpeg");
  esp_err_t res = httpd_resp_send(req, (const char *)jpg, len);
  free(jpg);
  return res;
}

// 视频流在独立任务里跑，避免占住 HTTP 服务线程（否则网页会打不开）
static const int MAX_STREAMS = 2;
static volatile int active_streams = 0;

static void stream_task(void *arg) {
  httpd_req_t *req = (httpd_req_t *)arg;
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  char part[96];
  while (true) {
    uint8_t *jpg = nullptr; size_t len = 0;
    if (!grab_jpeg(&jpg, &len)) { delay(10); continue; }
    int n = snprintf(part, sizeof(part),
                     "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                     BOUNDARY, (unsigned)len);
    esp_err_t res = httpd_resp_send_chunk(req, part, n);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)jpg, len);
    free(jpg);
    if (res != ESP_OK) break;  // 浏览器关掉了 / 断开了
  }
  httpd_req_async_handler_complete(req);
  active_streams--;
  Serial.printf("视频流断开，当前观看数 %d\n", active_streams);
  vTaskDelete(nullptr);
}

// GET /stream —— MJPEG 视频流（异步）
static esp_err_t stream_handler(httpd_req_t *req) {
  if (active_streams >= MAX_STREAMS) {
    httpd_resp_set_status(req, "503 Busy");
    return httpd_resp_send(req, "too many viewers", HTTPD_RESP_USE_STRLEN);
  }
  httpd_req_t *async_req = nullptr;
  if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) return ESP_FAIL;
  active_streams++;
  if (xTaskCreate(stream_task, "stream", 6144, async_req, 5, nullptr) != pdPASS) {
    active_streams--;
    httpd_req_async_handler_complete(async_req);
    return ESP_FAIL;
  }
  Serial.printf("新的视频流连接，当前观看数 %d\n", active_streams);
  return ESP_OK;
}

// GET /control?size=hvga&q=75 —— 运行时调清晰度
static esp_err_t control_handler(httpd_req_t *req) {
  char query[64] = {0}, val[16];
  bool ok = true;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    if (httpd_query_key_value(query, "size", val, sizeof(val)) == ESP_OK) ok &= apply_size(val);
    if (httpd_query_key_value(query, "q", val, sizeof(val)) == ESP_OK) {
      int q = atoi(val);
      if (q >= 10 && q <= 95) jpeg_quality = q; else ok = false;
    }
  }
  char out[64];
  snprintf(out, sizeof(out), "{\"ok\":%s,\"size\":\"%s\",\"q\":%d}", ok ? "true" : "false", cur_size, jpeg_quality);
  Serial.printf("清晰度设置: %s\n", out);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static void start_server() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_open_sockets = 7;
  cfg.lru_purge_enable = true;   // 连接满了自动踢掉最久没用的
  cfg.send_wait_timeout = 3;     // 客户端掉线 3 秒内释放
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &cfg) != ESP_OK) { Serial.println("HTTP 服务启动失败"); return; }
  httpd_uri_t routes[] = {
    {"/",        HTTP_GET, index_handler,   nullptr},
    {"/capture", HTTP_GET, capture_handler, nullptr},
    {"/stream",  HTTP_GET, stream_handler,  nullptr},
    {"/control", HTTP_GET, control_handler, nullptr},
  };
  for (auto &r : routes) httpd_register_uri_handler(server, &r);
}

static bool camera_init(framesize_t fs) {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_RGB565;   // GC2145 没有硬件 JPEG
  c.frame_size   = fs;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.fb_count     = 2;
  c.grab_mode    = CAMERA_GRAB_LATEST;
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) Serial.printf("esp_camera_init 失败: 0x%x\n", err);
  return err == ESP_OK;
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  delay(1000);
  Serial.println("\n== ESP32-S3 AI CAM demo ==");

  cam_lock = xSemaphoreCreateMutex();
  if (!camera_init(FRAMESIZE_QVGA) || !apply_size(DEFAULT_SIZE)) {
    Serial.println("摄像头初始化失败（检查 PSRAM 是否设为 OPI PSRAM）");
    return;
  }
  Serial.printf("摄像头 OK，默认 %s / 质量 %d\n", cur_size, jpeg_quality);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("连接 WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));  // 连接中 LED 闪烁
    delay(300);
    Serial.print(".");
  }
  digitalWrite(LED_PIN, HIGH);  // 连上了 LED 常亮
  Serial.printf("\nWiFi 已连接，浏览器打开:  http://%s/\n", WiFi.localIP().toString().c_str());

  start_server();
}

void loop() { delay(1000); }
