#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <esp_http_client.h>

extern "C" esp_err_t esp_crt_bundle_attach(void *conf);

// ===========================
// Select camera model in board_config.h
// ===========================
#if __has_include("board_config.h")
#include "board_config.h"
#elif __has_include("camera_pins.h")
#include "camera_pins.h"
#else
#error "Camera board configuration file is missing."
#endif

// ===========================
// Enter your WiFi credentials
// ===========================
const char *ssid = "nest_wifi";
const char *password = "0043128208";

// Configure these with a Telegram bot token and the destination chat ID.
const char *telegramBotToken = "8808996429:AAHPr90njCkX0Lgg_iqgqKTX17b0yBPwm9w";
const char *telegramChatId = "-5557236171";

constexpr int PIR_GPIO_NUM = 13;
constexpr unsigned long FALLBACK_PHOTO_INTERVAL_MS = 30000;

void startCameraServer();
void setupLedFlash();

static bool writeHttpData(esp_http_client_handle_t client, const char *data, size_t length) {
  size_t written = 0;
  while (written < length) {
    int result = esp_http_client_write(client, data + written, length - written);
    if (result <= 0) {
      return false;
    }
    written += result;
  }
  return true;
}

static bool sendTelegramPhoto(camera_fb_t *frame) {
  if (telegramBotToken[0] == '\0' || telegramChatId[0] == '\0') {
    Serial.println("Telegram not configured: set telegramBotToken and telegramChatId in src/main.cpp");
    return false;
  }

  const String boundary = "----ESP32CAMMotionBoundary";
  const String url = String("https://api.telegram.org/bot") + telegramBotToken + "/sendPhoto";
  const String contentType = "multipart/form-data; boundary=" + boundary;
  const String header =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
    String(telegramChatId) + "\r\n--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"photo\"; filename=\"motion.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n";
  const String footer = "\r\n--" + boundary + "--\r\n";
  const size_t contentLength = header.length() + frame->len + footer.length();

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 20000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    Serial.println("Telegram request initialization failed");
    return false;
  }

  bool requestWritten = false;
  if (esp_http_client_set_method(client, HTTP_METHOD_POST) == ESP_OK &&
      esp_http_client_set_header(client, "Content-Type", contentType.c_str()) == ESP_OK &&
      esp_http_client_open(client, contentLength) == ESP_OK) {
    requestWritten = writeHttpData(client, header.c_str(), header.length()) &&
                     writeHttpData(client, reinterpret_cast<const char *>(frame->buf), frame->len) &&
                     writeHttpData(client, footer.c_str(), footer.length());
  }

  bool sent = false;
  if (requestWritten) {
    esp_http_client_fetch_headers(client);
    const int statusCode = esp_http_client_get_status_code(client);
    char response[512] = {};
    const int responseLength = esp_http_client_read_response(client, response, sizeof(response) - 1);
    if (responseLength > 0) {
      response[responseLength] = '\0';
    }
    sent = statusCode == 200 && responseLength > 0 && strstr(response, "\"ok\":true") != NULL;
    if (!sent) {
      Serial.printf("Telegram photo send failed (HTTP %d)\n", statusCode);
      if (responseLength > 0) {
        Serial.printf("Telegram response: %s\n", response);
      } else {
        Serial.println("Telegram returned an empty response body");
      }
    }
  } else {
    Serial.println("Telegram photo upload failed");
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return sent;
}

static void captureAndSendPhoto() {
  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == NULL) {
    Serial.println("Photo capture failed");
    return;
  }

  const bool sent = sendTelegramPhoto(frame);
  esp_camera_fb_return(frame);
  if (sent) {
    Serial.println("Photo sent to Telegram");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  pinMode(PIR_GPIO_NUM, INPUT);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // flip it back
    s->set_brightness(s, 1);   // up the brightness just a bit
    s->set_saturation(s, -2);  // lower the saturation
  }
  // drop down frame size for higher initial frame rate
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

// Setup LED FLash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  startCameraServer();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
}

void loop() {
  static bool motionWasDetected = false;
  static unsigned long lastPhotoAt = millis();
  const bool motionIsDetected = digitalRead(PIR_GPIO_NUM) == HIGH;

  if (motionIsDetected && !motionWasDetected) {
    Serial.println("Motion detected");
    captureAndSendPhoto();
    lastPhotoAt = millis();
  } else if (millis() - lastPhotoAt >= FALLBACK_PHOTO_INTERVAL_MS) {
    Serial.println("Taking 30-second fallback photo");
    captureAndSendPhoto();
    lastPhotoAt = millis();
  }
  motionWasDetected = motionIsDetected;

  delay(20);
}
