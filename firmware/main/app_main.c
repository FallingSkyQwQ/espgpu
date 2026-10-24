/*
 * app_main.c — 启动流程
 *
 * 顺序有讲究：
 *   1. NVS（OTA 状态、设备配置要存这里）
 *   2. GPU 状态与帧缓冲（PSRAM 分配，越早越好，避免碎片）
 *   3. USB 描述符（序列号依赖 eFuse MAC）
 *   4. TinyUSB 安装
 *   5. 起任务：命令处理固定在 core0，呈现固定在 core1
 */

#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "gpu.h"
#include "usb_descriptors.h"
#include "usb_device.h"
#include "version.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " ESP32-S3 GPU  fw %d.%d.%d", ESPGPU_FW_VERSION_MAJOR,
             ESPGPU_FW_VERSION_MINOR, ESPGPU_FW_VERSION_PATCH);
    ESP_LOGI(TAG, "========================================");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: %d core(s), rev %d.%d", chip.cores, chip.revision / 100,
             chip.revision % 100);

    /* ── NVS ── */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, reformatting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* ── PSRAM ── */
    size_t psram = esp_psram_get_size();
    if (psram == 0) {
        ESP_LOGE(TAG, "PSRAM not detected! N16R8 requires 8MB Octal PSRAM.");
        ESP_LOGE(TAG, "check CONFIG_SPIRAM_MODE_OCT / board revision.");
    } else {
        ESP_LOGI(TAG, "PSRAM: %u KB free after init", (unsigned)(psram / 1024));
    }

    /* ── GPU ── */
    ESP_ERROR_CHECK(espgpu_gpu_init());

    /* ── USB ── */
    espgpu_descriptors_init();
    ESP_ERROR_CHECK(espgpu_usb_init());

    /* ── 任务 ──
     * core0：命令收发 + 统计采样（与 TinyUSB 任务同核，避免跨核同步开销）
     * core1：内容生成 + 编码 + 呈现（M2 起换成光栅器）
     */
    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(espgpu_gpu_command_task, "gpu_cmd", 6144, NULL, 4, NULL, 0);
    configASSERT(ok == pdPASS);

    ok = xTaskCreatePinnedToCore(espgpu_gpu_present_task, "gpu_present", 6144, NULL, 6, NULL, 1);
    configASSERT(ok == pdPASS);

    ESP_LOGI(TAG, "boot complete, waiting for host enumeration");
}
