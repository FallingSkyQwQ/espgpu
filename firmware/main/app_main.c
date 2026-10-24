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

    /* 调试期可用：把这两个 tag 放开到 DEBUG 可以看到每条控制请求 */
    esp_log_level_set("gpu", ESP_LOG_INFO);
    esp_log_level_set("usb", ESP_LOG_INFO);

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

#if CONFIG_ESPGPU_FLASH_WINDOW_MS > 0
    /* 关键：ESP32-S3 的 USB-Serial-JTAG 和 USB-OTG 共用 GPIO19/20。
     * 一旦下面 tinyusb_driver_install() 接管 OTG，esptool 用的 CDC 口就消失了，
     * 再烧录只能按住 BOOT 点 RST。
     *
     * 这段窗口让 ROM 的 Serial-JTAG 继续活着，idf.py flash 可以自行复位芯片。
     * 量产版本把它设成 0。 */
    ESP_LOGI(TAG, "flash window: %d ms before claiming USB pins",
             CONFIG_ESPGPU_FLASH_WINDOW_MS);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_ESPGPU_FLASH_WINDOW_MS));
#endif

    /* ── USB ── */
    espgpu_descriptors_init();
#if CONFIG_ESPGPU_USB_ENABLE
    ESP_ERROR_CHECK(espgpu_usb_init());
#else
    ESP_LOGW(TAG, "USB OTG disabled by config, Serial-JTAG stays active");
#endif

    /* ── 任务 ──
     * core0：命令收发 + 统计采样（与 TinyUSB 任务同核）
     * core1：内容生成 + 编码 + 呈现（M2 起换成光栅器）
     *
     * 命令任务的优先级必须高于 TinyUSB 任务（5）：esp_tinyusb 的任务是
     * `while(1) tud_task();`，持续出帧时它几乎一直就绪，同核上优先级更低的
     * 任务会被饿死，表现为 SET_MODE / GET_STATS 全部超时。
     */
    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(espgpu_gpu_command_task, "gpu_cmd", 6144, NULL, 7, NULL, 0);
    configASSERT(ok == pdPASS);
    (void)ok;

    ok = xTaskCreatePinnedToCore(espgpu_gpu_present_task, "gpu_present", 6144, NULL, 6, NULL, 1);
    configASSERT(ok == pdPASS);
    (void)ok;

    ESP_LOGI(TAG, "boot complete, waiting for host enumeration");
}
