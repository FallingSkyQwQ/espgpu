/*
 * panel_api.cpp — M5 管理功能（时钟 / 调试视图 / 编码 / 日志 / OTA / 复位）
 *               + 前端共用的呈现辅助（BGRA 快照、画到 HDC）
 */

#include "internal.h"

#include <algorithm>
#include <cstring>

using namespace espgpu;

namespace {

int simple_ctrl(espgpu_device *dev, uint16_t op, uint32_t a0, int timeout_ms)
{
    if (dev == nullptr) {
        return -1;
    }
    std::vector<uint8_t> resp;
    return link_ctrl(dev, op, a0, 0, nullptr, 0, resp, static_cast<DWORD>(timeout_ms));
}

/** 标准 CRC-32（zlib / esp_rom_crc32_le 兼容） */
uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    while (n--) {
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

} // namespace

extern "C" {

int espgpu_set_clock(espgpu_device_t *dev, int mhz, int timeout_ms)
{
    return simple_ctrl(dev, ESPGPU_CTRL_SET_CLOCK, static_cast<uint32_t>(mhz), timeout_ms);
}

int espgpu_set_debug_view(espgpu_device_t *dev, int view, int timeout_ms)
{
    return simple_ctrl(dev, ESPGPU_CTRL_SET_DEBUG_VIEW, static_cast<uint32_t>(view), timeout_ms);
}

int espgpu_set_codec(espgpu_device_t *dev, int codec, int timeout_ms)
{
    return simple_ctrl(dev, ESPGPU_CTRL_SET_CODEC, static_cast<uint32_t>(codec), timeout_ms);
}

int espgpu_log_subscribe(espgpu_device_t *dev, int level, int timeout_ms)
{
    return simple_ctrl(dev, ESPGPU_CTRL_LOG_SUBSCRIBE, static_cast<uint32_t>(level), timeout_ms);
}

int espgpu_soft_reset(espgpu_device_t *dev, int timeout_ms)
{
    return simple_ctrl(dev, ESPGPU_CTRL_SOFT_RESET, 0, timeout_ms);
}

int espgpu_request_keyframe(espgpu_device_t *dev, int timeout_ms)
{
    return simple_ctrl(dev, ESPGPU_CTRL_REQUEST_KEYFRAME, 0, timeout_ms);
}

int espgpu_ota_flash(espgpu_device_t *dev, const void *image, size_t len, const char *version,
                     espgpu_progress_fn cb, void *user)
{
    if (dev == nullptr || image == nullptr || len == 0 || len > 0x300000) {
        return -1;
    }
    const auto *p = static_cast<const uint8_t *>(image);
    /* ESP-IDF 应用镜像以 0xE9 开头，挡掉选错文件（比如 bootloader 或分区表）的情况 */
    if (p[0] != 0xE9) {
        log_msg(LOG_ERROR, "ota: not an ESP application image (magic 0x%02X)", p[0]);
        return ESPGPU_ERR_INVALID_ARG;
    }

    std::vector<uint8_t> resp;
    const char *ver = version ? version : "";
    int st = link_ctrl(dev, ESPGPU_CTRL_OTA_BEGIN, static_cast<uint32_t>(len), 0, ver,
                       static_cast<uint16_t>(strlen(ver)), resp, 5000);
    if (st != ESPGPU_OK) {
        log_msg(LOG_ERROR, "ota begin failed: %d", st);
        return st;
    }

    constexpr uint32_t kBlock = 8 * 1024;
    uint32_t crc = 0;
    uint32_t off = 0;
    while (off < len) {
        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(kBlock, len - off));
        /* 擦除扇区时单次写可能要几百毫秒，超时给足 */
        st = link_ctrl(dev, ESPGPU_CTRL_OTA_DATA, off, 0, p + off, static_cast<uint16_t>(n), resp,
                       5000);
        if (st != ESPGPU_OK) {
            log_msg(LOG_ERROR, "ota data @%u failed: %d", off, st);
            link_ctrl(dev, ESPGPU_CTRL_OTA_ABORT, 0, 0, nullptr, 0, resp, 1000);
            return st;
        }
        crc = crc32_update(crc, p + off, n);
        off += n;
        if (cb && !cb(off, static_cast<uint32_t>(len), user)) {
            link_ctrl(dev, ESPGPU_CTRL_OTA_ABORT, 0, 0, nullptr, 0, resp, 1000);
            log_msg(LOG_WARN, "ota cancelled at %u/%zu", off, len);
            return -3;
        }
    }

    st = link_ctrl(dev, ESPGPU_CTRL_OTA_END, crc, 0, nullptr, 0, resp, 10000);
    if (st != ESPGPU_OK) {
        log_msg(LOG_ERROR, "ota end failed: %d", st);
        return st;
    }
    log_msg(LOG_INFO, "ota: %zu bytes written, crc %08X; reset to boot the new image", len, crc);
    return 0;
}

const espgpu_device_info_t *espgpu_cached_info(espgpu_device_t *dev)
{
    if (dev == nullptr) {
        return nullptr;
    }
    return &dev->info;
}

bool espgpu_is_emulator(espgpu_device_t *dev)
{
    return dev != nullptr && dev->emu != nullptr;
}

int espgpu_frame_snapshot_bgra(espgpu_device_t *dev, uint8_t *dst, size_t cap, size_t pitch,
                               int *out_w, int *out_h, uint32_t *out_frame_id)
{
    if (dev == nullptr || dst == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lk(dev->frame_mu);
    const int w = dev->width;
    const int h = dev->height;
    if (w <= 0 || h <= 0 || dev->rgba.size() < static_cast<size_t>(w) * h * 4) {
        return -1;
    }
    if (pitch == 0) {
        pitch = static_cast<size_t>(w) * 4;
    }
    if (pitch < static_cast<size_t>(w) * 4 || cap < pitch * (h - 1) + static_cast<size_t>(w) * 4) {
        return -1;
    }
    const uint8_t *src = dev->rgba.data();
    for (int y = 0; y < h; y++) {
        uint8_t *d = dst + pitch * y;
        const uint8_t *s = src + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; x++) {
            d[x * 4 + 0] = s[x * 4 + 2];
            d[x * 4 + 1] = s[x * 4 + 1];
            d[x * 4 + 2] = s[x * 4 + 0];
            d[x * 4 + 3] = 255;
        }
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_frame_id) *out_frame_id = dev->frame_id;
    return 0;
}

int espgpu_blit_to_hdc(espgpu_device_t *dev, void *hdc_ptr, int dst_w, int dst_h)
{
    if (dev == nullptr || hdc_ptr == nullptr || dst_w <= 0 || dst_h <= 0) {
        return -1;
    }
    HDC hdc = static_cast<HDC>(hdc_ptr);

    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lk(dev->frame_mu);
        w = dev->width;
        h = dev->height;
    }
    if (w <= 0 || h <= 0) {
        PatBlt(hdc, 0, 0, dst_w, dst_h, BLACKNESS);
        return -1;
    }
    dev->blit_buf.resize(static_cast<size_t>(w) * h * 4);
    if (espgpu_frame_snapshot_bgra(dev, dev->blit_buf.data(), dev->blit_buf.size(), 0, &w, &h,
                                   nullptr) != 0) {
        return -1;
    }

    /* 等比缩放、黑边居中 */
    int out_w = dst_w;
    int out_h = static_cast<int>(static_cast<int64_t>(dst_w) * h / w);
    if (out_h > dst_h) {
        out_h = dst_h;
        out_w = static_cast<int>(static_cast<int64_t>(dst_h) * w / h);
    }
    const int ox = (dst_w - out_w) / 2;
    const int oy = (dst_h - out_h) / 2;
    if (oy > 0) {
        PatBlt(hdc, 0, 0, dst_w, oy, BLACKNESS);
        PatBlt(hdc, 0, oy + out_h, dst_w, dst_h - oy - out_h, BLACKNESS);
    }
    if (ox > 0) {
        PatBlt(hdc, 0, oy, ox, out_h, BLACKNESS);
        PatBlt(hdc, ox + out_w, oy, dst_w - ox - out_w, out_h, BLACKNESS);
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; /* 自上而下 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    /* 像素风内容：最近邻放大，不要 HALFTONE 的模糊 */
    SetStretchBltMode(hdc, COLORONCOLOR);
    const int rc = StretchDIBits(hdc, ox, oy, out_w, out_h, 0, 0, w, h, dev->blit_buf.data(), &bi,
                                 DIB_RGB_COLORS, SRCCOPY);
    return rc == 0 ? -1 : 0;
}

} // extern "C"
