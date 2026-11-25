
/*
=========================================================
              GREEN MILE - SMART IRRIGATION
              ESP32-CAM V2
=========================================================

CAMERA FEATURES:
1. Live camera streaming.
2. Automatic picture capture.
3. Automatic Supabase uploads.
4. Five-minute capture intervals.
5. Wi-Fi reconnection.
6. Camera status monitoring.

SUPABASE:
Storage Bucket: crop-images
Database Table: camera_images
Edge Function: camera-upload

HARDWARE:
AI Thinker ESP32-CAM
=========================================================
*/

#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include "board_config.h"


// =======================================================
// 1. WI-FI CONFIGURATION
// =======================================================

// Enter your NEW Wi-Fi credentials.
// Do not reuse the password that was exposed earlier.

const char* ssid = "Happy";
const char* password = "sandpaper";


// =======================================================
// 2. SUPABASE CONFIGURATION
// =======================================================

// Supabase project URL:
// https://zjebodmbvpjspuofdfbk.supabase.co

// IMPORTANT:
// This must be the Edge Function URL, not just
// the Supabase project URL.

const char* SUPABASE_FUNCTION_URL =
  "https://zjebodmbvpjspuofdfbk.supabase.co/functions/v1/camera-upload";

// Paste your Supabase anon/public key here.
// Never use your service-role key in ESP32 firmware.

const char* SUPABASE_ANON_KEY =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InpqZWJvZG1idnBqc3B1b2ZkZmJrIiwicm9sZSI6ImFub24iLCJpYXQiOjE3OTU1NDgyMjcsImV4cCI6MjExMTEyNDIyN30.4hBQaKSbQ0cHYa7QW_Dkd1ehr3vgFeygrsTvWIlbT8w";

const char* DEVICE_ID = "esp32-cam-01";


// =======================================================
// 3. AUTOMATIC CAPTURE SETTINGS
// =======================================================

// Capture and upload every five minutes.

const unsigned long UPLOAD_INTERVAL =
  5UL * 60UL * 1000UL;

unsigned long lastUploadTime = 0;


// =======================================================
// 4. CAMERA WEBSERVER FUNCTIONS
// =======================================================

// These functions are provided by the original
// ESP32 CameraWebServer project.

void startCameraServer();

void setupLedFlash();


// =======================================================
// 5. WI-FI CONNECTION
// =======================================================

void connectWiFi() {

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.println();
  Serial.println("Connecting to Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  WiFi.begin(ssid, password);

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED &&
         attempts < 30) {

    delay(500);

    Serial.print(".");

    attempts++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println("Wi-Fi connected!");

    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    Serial.print("Signal strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

  } else {

    Serial.println("Wi-Fi connection failed.");

  }
}


// =======================================================
// 6. CAMERA INITIALIZATION
// =======================================================

bool initializeCamera() {

  camera_config_t config = {};

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

  config.pixel_format = PIXFORMAT_JPEG;

  config.frame_size = FRAMESIZE_UXGA;

  config.jpeg_quality = 12;

  config.fb_count = 1;

  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  config.fb_location = CAMERA_FB_IN_PSRAM;


  // Configure available memory.

  if (psramFound()) {

    Serial.println("PSRAM detected.");

    config.jpeg_quality = 10;

    config.fb_count = 2;

    config.grab_mode = CAMERA_GRAB_LATEST;

  } else {

    Serial.println("PSRAM not detected.");

    config.frame_size = FRAMESIZE_SVGA;

    config.fb_location = CAMERA_FB_IN_DRAM;

  }


  // Initialize camera.

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {

    Serial.printf(
      "Camera initialization failed: 0x%x\n",
      err
    );

    return false;
  }


  // Configure camera sensor.

  sensor_t* sensor = esp_camera_sensor_get();

  if (sensor == nullptr) {

    Serial.println("Camera sensor unavailable.");

    return false;
  }

  if (sensor->id.PID == OV3660_PID) {

    sensor->set_vflip(sensor, 1);

    sensor->set_brightness(sensor, 1);

    sensor->set_saturation(sensor, -2);

  }


  // Initial streaming resolution.

  sensor->set_framesize(sensor, FRAMESIZE_QVGA);


  // Original camera model adjustments.

#if defined(CAMERA_MODEL_M5STACK_WIDE) || \
    defined(CAMERA_MODEL_M5STACK_ESP32CAM)

  sensor->set_vflip(sensor, 1);

  sensor->set_hmirror(sensor, 1);

#endif


#if defined(CAMERA_MODEL_ESP32S3_EYE)

  sensor->set_vflip(sensor, 1);

#endif


  // Initialize camera flash if supported.

#if defined(LED_GPIO_NUM)

  setupLedFlash();

#endif


  Serial.println("Camera initialized successfully!");

  return true;
}


// =======================================================
// 7. CAPTURE AND UPLOAD TO SUPABASE
// =======================================================

bool uploadImageToSupabase() {

  Serial.println();

  Serial.println("================================");

  Serial.println("GREEN MILE CAMERA CAPTURE");

  Serial.println("================================");


  // Check Wi-Fi.

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("Wi-Fi disconnected.");

    connectWiFi();

    if (WiFi.status() != WL_CONNECTED) {

      Serial.println("Upload cancelled.");

      return false;

    }

  }


  // Capture image.

  Serial.println("Capturing image...");

  camera_fb_t* fb = esp_camera_fb_get();

  if (!fb) {

    Serial.println("Camera capture failed!");

    return false;

  }


  Serial.println("Image captured successfully!");

  Serial.print("Image size: ");

  Serial.print(fb->len);

  Serial.println(" bytes");


  // Check image size.

  if (fb->len == 0) {

    Serial.println("Captured image is empty.");

    esp_camera_fb_return(fb);

    return false;

  }

  if (fb->len > 5 * 1024 * 1024) {

    Serial.println("Image exceeds 5 MB limit.");

    esp_camera_fb_return(fb);

    return false;

  }


  // Prepare HTTPS connection.

  WiFiClientSecure client;

  // Prototype testing only.
  // Certificate verification is disabled here.
  // Configure certificate validation before production.

  client.setInsecure();

  HTTPClient https;

  https.setTimeout(30000);


  Serial.println("Connecting to Supabase...");

  Serial.print("Function URL: ");

  Serial.println(SUPABASE_FUNCTION_URL);


  if (!https.begin(client, SUPABASE_FUNCTION_URL)) {

    Serial.println("Supabase connection setup failed.");

    esp_camera_fb_return(fb);

    return false;

  }


  // HTTP headers.

  https.addHeader("Content-Type", "image/jpeg");

  https.addHeader("apikey", SUPABASE_ANON_KEY);

  https.addHeader(
    "Authorization",
    String("Bearer ") + SUPABASE_ANON_KEY
  );

  https.addHeader("x-device-id", DEVICE_ID);


  // Upload image.

  Serial.println("Uploading image...");

  int responseCode = https.POST(fb->buf, fb->len);


  // Process server response.

  bool success = false;

  if (responseCode > 0) {

    Serial.print("HTTP Response Code: ");

    Serial.println(responseCode);

    String response = https.getString();

    Serial.println("Supabase Response:");

    Serial.println(response);

    if (responseCode == 201) {

      Serial.println("IMAGE UPLOADED SUCCESSFULLY!");

      success = true;

    } else {

      Serial.println("Image upload failed.");

      if (responseCode == 400) {
        Serial.println("Check the request or image format.");
      }

      if (responseCode == 401 ||
          responseCode == 403) {
        Serial.println("Check function authentication and permissions.");
      }

      if (responseCode == 404) {
        Serial.println("Check the Edge Function URL and deployment.");
      }

      if (responseCode >= 500) {
        Serial.println("Supabase server-side error.");
      }

    }

  } else {

    Serial.print("HTTP Error: ");

    Serial.println(https.errorToString(responseCode));

    Serial.print("Wi-Fi status: ");

    Serial.println(WiFi.status());

  }


  // Release memory and connection.

  https.end();

  esp_camera_fb_return(fb);

  Serial.println("Upload process completed.");

  return success;
}


// =======================================================
// 8. SYSTEM SETUP
// =======================================================

void setup() {

  Serial.begin(115200);

  Serial.setDebugOutput(true);

  Serial.println();


  Serial.println("====================================");

  Serial.println(" GREEN MILE SMART IRRIGATION");

  Serial.println(" ESP32-CAM SYSTEM STARTING");

  Serial.println("====================================");


  // Connect to Wi-Fi first.

  connectWiFi();

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("Wi-Fi unavailable. Restarting...");

    delay(5000);

    ESP.restart();

  }


  // Initialize camera.

  if (!initializeCamera()) {

    Serial.println("Camera startup failed!");

    delay(5000);

    ESP.restart();

  }


  // Start live camera server.

  startCameraServer();


  Serial.println();

  Serial.println("LIVE CAMERA SERVER STARTED!");

  Serial.print("Camera URL: http://");

  Serial.println(WiFi.localIP());


  Serial.print("Camera stream URL: http://");

  Serial.print(WiFi.localIP());

  Serial.println(":81/stream");


  // First automatic picture upload.

  Serial.println();

  Serial.println("Starting first picture upload...");

  uploadImageToSupabase();


  // Start automatic upload timer.

  lastUploadTime = millis();


  Serial.println();

  Serial.println("====================================");

  Serial.println("GREEN MILE CAMERA READY!");

  Serial.println("Live streaming: ACTIVE");

  Serial.println("Supabase uploads: ENABLED");

  Serial.println("Storage bucket: crop-images");

  Serial.println("Upload interval: 5 MINUTES");

  Serial.println("====================================");

}


// =======================================================
// 9. MAIN LOOP
// =======================================================

void loop() {

  unsigned long currentTime = millis();


  // Monitor Wi-Fi connection.

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("Wi-Fi connection lost.");

    connectWiFi();

  }


  // Five-minute automatic capture.

  if (currentTime - lastUploadTime >= UPLOAD_INTERVAL) {

    Serial.println();

    Serial.println("Five-minute capture interval reached.");

    uploadImageToSupabase();

    lastUploadTime = millis();

  }


  delay(1000);

}
