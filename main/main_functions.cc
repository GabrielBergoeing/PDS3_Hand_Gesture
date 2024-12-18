/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "main_functions.h"

#include "detection_responder.h"
#include "esp_err.h"
#include "image_provider.h"
#include "model_settings.h"
#include "person_detect_model_data.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_log.h>
#include "esp_main.h"
#include "esp_psram.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "nvs_flash.h"

#define BUF_SIZE (1024)

// ESP NOW =================================================================== ESP NOW
#define ESP_CHANNEL 0

static bool ghost_busters = false;

// esp32 feather mac address:   {0x30, 0xAE, 0xA4, 0x1B, 0x93, 0xF4}
// esp32 cam mac address:       {0xFC, 0xE8, 0xC0, 0xCE, 0x53, 0xD4}
static uint8_t peer_mac [ESP_NOW_ETH_ALEN] = {0x30, 0xAE, 0xA4, 0x1B, 0x93, 0xF4};

static const char* TAG = "esp_now_cam";

static esp_err_t init_wifi()
{
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_netif_init();
    esp_event_loop_create_default();
    nvs_flash_init();
    esp_wifi_init(&wifi_init_config);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    esp_wifi_start();

    ESP_LOGI(TAG, "wifi init completed");
    return ESP_OK;
}

void recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len)
{
    ESP_LOGI(TAG, "Data recieved " MACSTR " %s", MAC2STR(esp_now_info->src_addr), data);
    // ghost_busters = true;
}

void send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS)
    {
        ESP_LOGI(TAG, "ESP_NOW_SEND_SUCCESS");
    }
    else
    {
        ESP_LOGI(TAG, "ESP_NOW_SEND_FAIL");
    }
}

static esp_err_t init_esp_now(void)
{
    esp_now_init();
    esp_now_register_recv_cb(recv_cb);
    esp_now_register_send_cb(send_cb);

    ESP_LOGI(TAG, "esp now init completed");
    return ESP_OK;
}

static esp_err_t register_peer(uint8_t *peer_addr)
{
    esp_now_peer_info_t esp_now_peer_info = {};
    memcpy(esp_now_peer_info.peer_addr, peer_mac, ESP_NOW_ETH_ALEN);
    // esp_now_peer_info.ifidx = WIFI_IF_STA;
    esp_now_peer_info.channel = ESP_CHANNEL;

    esp_now_add_peer(&esp_now_peer_info);
    return ESP_OK;
}

static esp_err_t esp_now_send_data(const uint8_t *peer_addr, uint8_t *data, size_t len)
{
    esp_now_send(peer_addr, data, len);
    return ESP_OK;
}

// ESP NOW =================================================================== ESP NOW

// Globals, used for compatibility with Arduino-style sketches.
namespace {
const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;

// In order to use optimized tensorflow lite kernels, a signed int8_t quantized
// model is preferred over the legacy unsigned model format. This means that
// throughout this project, input images must be converted from unisgned to
// signed format. The easiest and quickest way to convert from unsigned to
// signed 8-bit integers is to subtract 128 from the unsigned value to get a
// signed value.

#ifdef CONFIG_IDF_TARGET_ESP32S3
constexpr int scratchBufSize = 40 * 1024;
#else
constexpr int scratchBufSize = 0;
#endif
// An area of memory to use for input, output, and intermediate arrays.
constexpr int kTensorArenaSize = 560 * 1024 + scratchBufSize;
static uint8_t *tensor_arena;//[kTensorArenaSize]; // Maybe we should move this to external
}  // namespace

// The name of this function is important for Arduino compatibility.
void setup() {
  if (esp_psram_get_size() == 0) {
    ESP_LOGE(TAG, "PSRAM not found");
    return;
  }

  // ESPNOW
  ESP_ERROR_CHECK(init_wifi());
  ESP_ERROR_CHECK(init_esp_now());
  ESP_ERROR_CHECK(register_peer(peer_mac));

  // Map the model into a usable data structure. This doesn't involve any
  // copying or parsing, it's a very lightweight operation.
  model = tflite::GetModel(g_person_detect_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    MicroPrintf("Model provided is schema version %d not equal to supported "
                "version %d.", model->version(), TFLITE_SCHEMA_VERSION);
    return;
  }

  if (tensor_arena == NULL) {
    tensor_arena = (uint8_t *) heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (tensor_arena == NULL) {
    printf("Couldn't allocate memory of %d bytes\n", kTensorArenaSize);
    return;
  }

  // Pull in only the operation implementations we need.
  // This relies on a complete list of all the ops needed by this graph.
  // An easier approach is to just use the AllOpsResolver, but this will
  // incur some penalty in code space for op implementations that are not
  // needed by this graph.
  //
  // tflite::AllOpsResolver resolver;
  // NOLINTNEXTLINE(runtime-global-variables)
  static tflite::MicroMutableOpResolver<11> micro_op_resolver;
  micro_op_resolver.AddQuantize();
  micro_op_resolver.AddReshape();
  micro_op_resolver.AddFullyConnected(); // Add ops used in MNIST model
  micro_op_resolver.AddSoftmax();
  micro_op_resolver.AddDequantize();
  micro_op_resolver.AddMean();
  micro_op_resolver.AddConv2D();
  micro_op_resolver.AddDepthwiseConv2D();
  micro_op_resolver.AddMaxPool2D();
  micro_op_resolver.AddMul();
  micro_op_resolver.AddAdd();

  // Build an interpreter to run the model with.
  // NOLINTNEXTLINE(runtime-global-variables)
  static tflite::MicroInterpreter static_interpreter(
      model, micro_op_resolver, tensor_arena, kTensorArenaSize);
  interpreter = &static_interpreter;

  // Allocate memory from the tensor_arena for the model's tensors.
  TfLiteStatus allocate_status = interpreter->AllocateTensors();
  if (allocate_status != kTfLiteOk) {
    MicroPrintf("AllocateTensors() failed");
    return;
  }

  // Get information about the memory area to use for the model's input.
  input = interpreter->input(0);

#ifndef CLI_ONLY_INFERENCE
  // Initialize Camera
  TfLiteStatus init_status = InitCamera();
  if (init_status != kTfLiteOk) {
    MicroPrintf("InitCamera failed\n");
    return;
  }
#endif
}

#ifndef CLI_ONLY_INFERENCE
// The name of this function is important for Arduino compatibility.
void loop() {
  if (ghost_busters) {
    vTaskDelay(7000 / portTICK_PERIOD_MS);
  uint8_t buffer[5];
  
  // Run inference for 5 hand gesture detections
    int gesture_count = 0;
    while (gesture_count < 5) {

        // Get image from provider
        if (kTfLiteOk != GetImage(kNumCols, kNumRows, kNumChannels, input->data.int8)) {
            MicroPrintf("Image capture failed.");
            continue; // Skip to next iteration if capture fails
        }

        // Run inference
        if (kTfLiteOk != interpreter->Invoke()) {
            MicroPrintf("Invoke failed.");
            continue; // Skip to next iteration if inference fails
        }

        // Process the inference results
        TfLiteTensor* output = interpreter->output(0);
        float digit_scores[kCategoryCount];
        for (int i = 0; i < kCategoryCount; ++i) {
            digit_scores[i] = output->data.f[i];
        }

        // Respond to detection (e.g., log detected gesture)
        buffer[gesture_count] = RespondToDetection(digit_scores, kCategoryLabels);

        // Increment the gesture count after each detection
        gesture_count++;
        vTaskDelay(1); // Short delay to avoid watchdog trigger

    }
    uint8_t tmp[5] = {1, 1, 1, 1, 1};
    esp_now_send_data(peer_mac, tmp, 32);
    ghost_busters = false;
  }
}
#endif

#if defined(COLLECT_CPU_STATS)
  long long total_time = 0;
  long long start_time = 0;
  extern long long softmax_total_time;
  extern long long dc_total_time;
  extern long long conv_total_time;
  extern long long fc_total_time;
  extern long long pooling_total_time;
  extern long long add_total_time;
  extern long long mul_total_time;
#endif

void run_inference(void *ptr) {
  /* Convert from uint8 picture data to int8 */
  uint8_t* uint8_ptr = (uint8_t*) ptr;
  for (int i = 0; i < kNumCols * kNumRows; i++) {
    input->data.f[i] = (float(uint8_ptr[i]) / 127.5) - 1;
  }
  printf("\n");

#if defined(COLLECT_CPU_STATS)
  long long start_time = esp_timer_get_time();
#endif
  // Run the model on this input and make sure it succeeds.
  if (kTfLiteOk != interpreter->Invoke()) {
    MicroPrintf("Invoke failed.");
  }

#if defined(COLLECT_CPU_STATS)
  long long total_time = (esp_timer_get_time() - start_time);
  printf("Total time = %lld\n", total_time / 1000);
  printf("FC time = %lld\n", fc_total_time / 1000);
  printf("DC time = %lld\n", dc_total_time / 1000);
  printf("conv time = %lld\n", conv_total_time / 1000);
  printf("Pooling time = %lld\n", pooling_total_time / 1000);
  printf("add time = %lld\n", add_total_time / 1000);
  printf("mul time = %lld\n", mul_total_time / 1000);

  /* Reset times */
  total_time = 0;
  dc_total_time = 0;
  conv_total_time = 0;
  fc_total_time = 0;
  pooling_total_time = 0;
  add_total_time = 0;
  mul_total_time = 0;
#endif

  TfLiteTensor* output = interpreter->output(0);
  
  // Process the inference results.
  float digit_scores[kCategoryCount];
  for (int i = 0; i < kCategoryCount; ++i) {
    MicroPrintf("Seña \"%s\": %0.2f%%", kCategoryLabels[i], output->data.f[i]*100);
    digit_scores[i] = output->data.f[i];
  }
  
  RespondToDetection(digit_scores, kCategoryLabels);
}
