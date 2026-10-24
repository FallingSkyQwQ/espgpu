/*
 * espgpu-bench — 带宽与压缩率实测
 *
 * M1 的验收工具：把固件的每种测试图案 × 每种编码 × 每种像素格式跑一遍，
 * 输出实测帧率、上行带宽和压缩比，用来确认"8 倍压缩"这条预算线是否成立。
 *
 * 用法：
 *   espgpu-bench                 # 默认矩阵，每种组合 3 秒
 *   espgpu-bench -s 5            # 每种组合 5 秒
 *   espgpu-bench -i              # 只打设备信息
 */

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "espgpu/espgpu.h"

namespace {

constexpr int kUsbBudgetKBps = 850; /* Full-Speed bulk 实测保守值 */

const char *pattern_name(int p)
{
    switch (p) {
    case ESPGPU_PATTERN_BARS: return "bars   ";
    case ESPGPU_PATTERN_UI: return "ui     ";
    case ESPGPU_PATTERN_DOOMISH: return "doomish";
    case ESPGPU_PATTERN_NOISE: return "noise  ";
    case ESPGPU_PATTERN_MIX: return "mix    ";
    default: return "?      ";
    }
}

const char *codec_name(int c)
{
    switch (c) {
    case ESPGPU_CODEC_RAW: return "raw   ";
    case ESPGPU_CODEC_RLE: return "rle   ";
    case ESPGPU_CODEC_DELTA_RLE: return "drle  ";
    default: return "?     ";
    }
}

const char *format_name(int f)
{
    return (f == ESPGPU_FMT_INDEX8) ? "index8" : "rgb565";
}

struct Row {
    std::string label;
    double fps;
    double in_kbps;
    double ratio;
    double raw_mbps;
    bool ok;
};

int run_case(espgpu_device_t *dev, uint16_t w, uint16_t h, uint8_t format, uint8_t codec,
             uint8_t pattern, double seconds, Row &out)
{
    espgpu_mode_t mode{};
    mode.width = w;
    mode.height = h;
    mode.format = format;
    mode.codec = codec;
    mode.max_fps = 60; /* 不设上限，让链路自己跑满 */
    mode.vsync = 0;

    if (espgpu_set_mode(dev, &mode, pattern, 800) != 0) {
        out.ok = false;
        return -1;
    }

    /* 丢掉切换模式的过渡帧 */
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    const auto t0 = std::chrono::steady_clock::now();
    const espgpu_link_stats_t *ls = espgpu_link_stats(dev);
    const uint32_t frames0 = ls->frames_received;
    const uint32_t bytes0 = ls->bytes_in;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - t0).count() >= seconds) {
            break;
        }
        espgpu_poll(dev, 20);
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(t1 - t0).count();
    const uint32_t frames = ls->frames_received - frames0;
    const uint32_t bytes = ls->bytes_in - bytes0;

    espgpu_stats_t ds{};
    const bool have_stats = espgpu_get_stats(dev, &ds, 500) == 0;

    const double raw_frame_bytes = static_cast<double>(w) * h * (format == ESPGPU_FMT_INDEX8 ? 1 : 2);
    out.fps = frames / elapsed;
    out.in_kbps = (bytes / 1024.0) / elapsed;
    out.ratio = have_stats && ds.frame_enc_bytes
                    ? static_cast<double>(ds.frame_raw_bytes) / ds.frame_enc_bytes
                    : 0.0;
    out.raw_mbps = raw_frame_bytes * out.fps / (1024.0 * 1024.0);
    out.ok = frames > 0;
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);

    double seconds = 3.0;
    bool info_only = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0) {
            info_only = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: espgpu-bench [-s seconds] [-i]\n");
            return 0;
        }
    }

    espgpu_set_log_level(2);

    printf("============================================================\n");
    printf(" ESP32-S3 GPU benchmark\n");
    printf("============================================================\n\n");

    const int count = espgpu_device_count();
    printf("devices found: %d\n", count);
    if (count == 0) {
        printf("\n未找到设备。检查：\n");
        printf("  1. USB 线是否插在 ESP32-S3 的原生 USB 口（GPIO19/20），不是 UART 口\n");
        printf("  2. 设备管理器里是否出现 winusb 设备（VID_303A PID_8201）\n");
        return 1;
    }

    for (int i = 0; i < count; i++) {
        printf("  [%d] %s\n", i, espgpu_device_path_at(i));
    }

    char err[256] = {0};
    espgpu_device_t *dev = espgpu_open(0, err, sizeof(err));
    if (dev == nullptr) {
        printf("open failed: %s\n", err);
        return 1;
    }

    espgpu_device_info_t info{};
    if (espgpu_get_info(dev, &info, 1500) == 0) {
        printf("\n--- device ---\n");
        printf("chip            : %s\n", info.chip_model);
        printf("firmware        : %u.%u.%u (proto %u)\n", info.fw_major, info.fw_minor,
               info.fw_patch, info.proto_version);
        printf("max resolution  : %ux%u\n", info.max_width, info.max_height);
        printf("psram           : %u MB total, %u KB free\n", info.psram_mb,
               info.psram_free / 1024);
        printf("sram            : %u KB total, %u KB free\n", info.sram_total / 1024,
               info.sram_free / 1024);
        printf("capabilities    : 0x%04X\n", info.capabilities);
        printf("codecs          : 0x%02X\n", info.codecs);
        printf("\n");
    } else {
        printf("get_info failed\n");
        espgpu_close(dev);
        return 1;
    }

    if (info_only) {
        espgpu_close(dev);
        return 0;
    }

    struct Case {
        uint16_t w, h;
        uint8_t format;
    };
    static const Case resolutions[] = {
        {320, 200, ESPGPU_FMT_RGB565},
        {320, 200, ESPGPU_FMT_INDEX8},
        {160, 120, ESPGPU_FMT_RGB565},
    };
    static const uint8_t codecs[] = {ESPGPU_CODEC_RAW, ESPGPU_CODEC_RLE,
                                     ESPGPU_CODEC_DELTA_RLE};
    static const uint8_t patterns[] = {ESPGPU_PATTERN_BARS, ESPGPU_PATTERN_UI,
                                       ESPGPU_PATTERN_DOOMISH, ESPGPU_PATTERN_NOISE};

    std::vector<Row> rows;

    for (const auto &res : resolutions) {
        for (uint8_t codec : codecs) {
            for (uint8_t pattern : patterns) {
                Row row{};
                char label[96];
                snprintf(label, sizeof(label), "%ux%u %s %s %s", res.w, res.h,
                         format_name(res.format), codec_name(codec), pattern_name(pattern));
                row.label = label;

                printf("%-46s ", label);
                fflush(stdout);

                if (run_case(dev, res.w, res.h, res.format, codec, pattern, seconds, row) == 0 &&
                    row.ok) {
                    printf("%6.1f fps  %7.1f KB/s  x%5.1f  (裸传 %5.2f MB/s)\n", row.fps,
                           row.in_kbps, row.ratio, row.raw_mbps);
                } else {
                    printf("FAILED\n");
                }
                rows.push_back(row);
            }
        }
    }

    printf("\n--- 结论 ---\n");
    printf("USB 预算 %.0f KB/s\n", static_cast<double>(kUsbBudgetKBps));
    int pass = 0;
    for (const auto &r : rows) {
        if (r.ok && r.fps >= 24.0 && r.in_kbps <= kUsbBudgetKBps) {
            pass++;
        }
    }
    printf("%zu 组用例中 %d 组同时满足 >=24fps 且 <=%.0f KB/s\n", rows.size(), pass,
           static_cast<double>(kUsbBudgetKBps));

    espgpu_close(dev);
    return 0;
}
