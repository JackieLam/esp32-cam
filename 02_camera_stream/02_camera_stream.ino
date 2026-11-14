// Demo 2: 摄像头实时画面 —— 浏览器打开 http://<板子IP>/ 就能看到视频
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
// 分辨率越小、JPEG 质量越低 → 帧率越高（GC2145 靠 CPU 软件压缩 JPEG）
// 可选: FRAMESIZE_QQVGA(160x120) / FRAMESIZE_QVGA(320x240) / FRAMESIZE_HVGA(480x320) / FRAMESIZE_VGA(640x480)
#define FRAME_SIZE   FRAMESIZE_QVGA
#define JPEG_QUALITY 60   // 0~100，越低越快、越糊

static const char *BOUNDARY = "frame";

// GET /  —— 一个极简网页，里面嵌入视频流
static esp_err_t index_handler(httpd_req_t *req) {
  static const char html[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'>"
    "<title>ESP32-S3 AI CAM</title></head>"
    "<body style='margin:0;background:#111;color:#eee;font-family:sans-serif;text-align:center'>"
    "<h3>ESP32-S3 AI CAM</h3><img src='/stream' style='max-width:100%'>"
    "<p><a style='color:#8cf' href='/capture'>拍一张照片</a></p></body></html>";
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// 取一帧并转成 JPEG
static bool grab_jpeg(uint8_t **jpg, size_t *len) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;
  bool ok = frame2jpg(fb, JPEG_QUALITY, jpg, len);
  esp_camera_fb_return(fb);
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
  };
  for (auto &r : routes) httpd_register_uri_handler(server, &r);
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  delay(1000);
  Serial.println("\n== ESP32-S3 AI CAM demo ==");

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
  c.frame_size   = FRAME_SIZE;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.fb_count     = 2;
  c.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("摄像头初始化失败: 0x%x （检查 PSRAM 是否设为 OPI PSRAM）\n", err);
    return;
  }
  Serial.println("摄像头 OK");

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
