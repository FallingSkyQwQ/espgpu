/*
 * app_main.c — 启动流程
 *
 * 顺序有讲究：
 *   1. NVS（OTA 状态、设备配置要存这里）
 *   2. GPU 状态与帧缓冲（PSRAM 分配，越早越好，避免碎片）
 *   3. USB 描述符（序列号依赖 eFuse MAC）
 *   4. TinyUSB 安装
 *   5. 起任务：命令处理固定在 core0，呈现 / 发送 / TinyUSB 在 core1
 *
 * OTA 回滚确认不在这里做：要等主机通过 USB 发来第一个控制请求（gpu.c
 * confirm_running_app_once），USB 起不来的新固件下次复位会自动回滚。
 */

#include <stdlib.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
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

#if CONFIG_ESPGPU_USB_ENABLE
/* USB 的中断分配在调用 tinyusb_driver_install() 的那个核上。app_main 在 core0，
 * 而 core0 跑命令执行（T&L 是单核的）：每个 64 字节包一次中断，整帧发送期间会把
 * T&L 的墙钟时间拉长（issue #2 里 T&L 随分辨率变慢的一个可疑原因）。
 * 所以在 core1 上安装，让中断和 TinyUSB 任务、发送任务待在一起。 */
static esp_err_t s_usb_err;

static void usb_init_task(void *arg)
{
    s_usb_err = espgpu_usb_init();
    xTaskNotifyGive((TaskHandle_t)arg);
    vTaskDelete(NULL);
}
#endif

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
    espgpu_log_forward_init();
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
    s_usb_err = ESP_FAIL;
    if (xTaskCreatePinnedToCore(usb_init_task, "usb_init", 4096, xTaskGetCurrentTaskHandle(), 5,
                                NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "usb_init task create failed");
        abort();
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_ERROR_CHECK(s_usb_err);
#else
    ESP_LOGW(TAG, "USB OTG disabled by config, Serial-JTAG stays active");
#endif

    /* ── 任务 ──
     * core0：接收 + 控制请求（8）/ 命令执行：T&L / 光栅（worker 0）/ 编码（7）
     * core1：TinyUSB（9）/ 第二个光栅 worker（8）/ 帧发送（7）/ 测试图案呈现（6）
     *
     * 命令任务的栈：gpu_core_exec → gp_draw3d → draw_triangle（gp_cvtx_t va[]+vb[] 和
     * gp_wvtx_t w[] 共约 4.8 KB 的局部数组）→ emit_tri → 分箱，主机上实测最深 6.8 KB
     * （fuzz），再加 PRESENT 回调里的编码、链路上任何一处 ESP_LOG（经 vprintf 钩子再
     * 格式化一次）和 Xtensa 窗口寄存器溢出区。原来的 6144 在 3D + 裁剪时必然溢出。
     *
     * 注意 CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE 会让 configASSERT 失效，
     * 所以创建失败要显式检查（栈加大之后内部 SRAM 不够是现实的失败方式）。 */
    static const struct {
        TaskFunction_t fn;
        const char *name;
        uint32_t stack;
        UBaseType_t prio;
        BaseType_t core;
    } tasks[] = {
        {espgpu_gpu_command_task, "gpu_cmd", 16384, 7, 0},
        /* 接收 + 控制请求：优先级高于命令任务，渲染一帧期间也能及时回 GET_STATS 等
         * （issue #3）。大部分时间阻塞在 USB 读上，几乎不占 CPU。 */
        {espgpu_gpu_rx_task, "gpu_rx", 6144, 8, 0},
        {espgpu_gpu_present_task, "gpu_present", 6144, 6, 1},
        /* 命令模式的帧发送：与下一帧的命令执行重叠。放 core1，
         * 命令模式下那里的测试图案任务是空闲的。 */
        /* 优先级高于 core1 上的光栅 / 编码 worker（8）：发送任务大部分时间阻塞在 USB 上，
         * 但 TX FIFO 只有 8 KB（~11 ms），被 worker 压住时上行会断流 */
        {espgpu_gpu_sender_task, "gpu_send", 4096, 9, 1},
    };
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); i++) {
        if (xTaskCreatePinnedToCore(tasks[i].fn, tasks[i].name, tasks[i].stack, NULL,
                                    tasks[i].prio, NULL, tasks[i].core) != pdPASS) {
            ESP_LOGE(TAG, "task %s create failed (stack %u, free internal %u)", tasks[i].name,
                     (unsigned)tasks[i].stack,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            abort();
        }
    }

    ESP_LOGI(TAG, "boot complete, waiting for host enumeration");
}
