/* ESP32-CAM hotspot + image server, for the common AI Thinker ESP32-CAM.
   Join Wi-Fi ESP32-CAM-Local / camera123, then open http://192.168.4.1/
   Snapshot: /capture   MJPEG: http://192.168.4.1:81/stream
*/
#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"

static const char *AP_SSID = "ESP32-CAM-Local";
static const char *AP_PASSWORD = "camera123";

// AI Thinker ESP32-CAM pin map
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

static httpd_handle_t cameraServer = nullptr;
static httpd_handle_t streamServer = nullptr;
static const char *STREAM_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char *BOUNDARY = "\r\n--frame\r\n";
static const char *PART_HEADER = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t homeHandler(httpd_req_t *req) {
  const char page[] = "<!doctype html><html><meta name='viewport' content='width=device-width'>"
    "<title>ESP32-CAM</title><body style='font-family:sans-serif'><h2>ESP32-CAM local camera</h2>"
    "<p><a href='/capture'>JPEG snapshot</a> | <a href='http://192.168.4.1:81/stream'>Live stream</a></p>"
    "<img src='http://192.168.4.1:81/stream' style='width:100%;max-width:800px'></body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captureHandler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera capture failed");
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t result = httpd_resp_send(req, reinterpret_cast<const char *>(fb->buf), fb->len);
  esp_camera_fb_return(fb);
  return result;
}

static esp_err_t streamHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, STREAM_TYPE);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return ESP_FAIL;
    char header[64];
    int len = snprintf(header, sizeof(header), PART_HEADER, static_cast<unsigned>(fb->len));
    esp_err_t result = httpd_resp_send_chunk(req, BOUNDARY, strlen(BOUNDARY));
    if (result == ESP_OK) result = httpd_resp_send_chunk(req, header, len);
    if (result == ESP_OK) result = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(fb->buf), fb->len);
    esp_camera_fb_return(fb);
    if (result != ESP_OK) break;
    delay(120);
  }
  return ESP_OK;
}

static void startWebServers() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  httpd_uri_t home = { .uri = "/", .method = HTTP_GET, .handler = homeHandler, .user_ctx = nullptr };
  httpd_uri_t capture = { .uri = "/capture", .method = HTTP_GET, .handler = captureHandler, .user_ctx = nullptr };
  if (httpd_start(&cameraServer, &cfg) == ESP_OK) {
    httpd_register_uri_handler(cameraServer, &home);
    httpd_register_uri_handler(cameraServer, &capture);
  }
  cfg.server_port = 81;
  cfg.ctrl_port = 32769;
  httpd_uri_t stream = { .uri = "/stream", .method = HTTP_GET, .handler = streamHandler, .user_ctx = nullptr };
  if (httpd_start(&streamServer, &cfg) == ESP_OK) httpd_register_uri_handler(streamServer, &stream);
}

void setup() {
  Serial.begin(115200);
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM; c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000; c.pixel_format = PIXFORMAT_JPEG;
  if (psramFound()) {
    c.frame_size = FRAMESIZE_VGA; c.jpeg_quality = 12; c.fb_count = 2;
    c.fb_location = CAMERA_FB_IN_PSRAM; c.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    c.frame_size = FRAMESIZE_QVGA; c.jpeg_quality = 15; c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_DRAM; c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) { Serial.printf("Camera init failed: 0x%x\n", err); return; }

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, 1, false, 2)) {
    Serial.println("Could not start camera hotspot"); return;
  }
  Serial.printf("Wi-Fi: %s  Password: %s\n", AP_SSID, AP_PASSWORD);
  Serial.println("Open http://192.168.4.1/");
  Serial.println("Stream http://192.168.4.1:81/stream");
  startWebServers();
}

void loop() { delay(1000); }
