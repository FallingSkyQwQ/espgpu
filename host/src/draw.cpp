/*
 * draw.cpp — M2 绘制命令的批量打包
 *
 * 命令先追加进主机侧缓冲，攒够一个 Chunk 再一次性发出去：
 * 一帧通常只占一次 bulk 传输，比每条命令一个包省得多（64B 包的协议开销很可观）。
 *
 * 线程模型：本组接口不做加锁，调用方需要保证同一时刻只有一个线程在提交命令。
 * 接收线程不会碰 cmd_buf。
 */

#include "internal.h"

#include <cstring>

using namespace espgpu;

namespace espgpu {

int cmd_flush(espgpu_device *dev, int timeout_ms)
{
    if (dev->cmd_buf.empty()) {
        return 0;
    }
    if (!dev->connected.load()) {
        dev->cmd_buf.clear();
        return -1;
    }
    /* 差分参考帧失步时在这里顺带要关键帧（GL 前端不调 espgpu_poll） */
    link_service_keyframe(dev);
    const int rc = link_send_chunk(dev, ESPGPU_CHUNK_CMD, 0, dev->cmd_buf.data(),
                                   static_cast<uint32_t>(dev->cmd_buf.size()),
                                   static_cast<DWORD>(timeout_ms));
    dev->cmd_buf.clear();
    return rc;
}

void cmd_push(espgpu_device *dev, uint8_t op, uint8_t flags, const void *body, uint32_t size)
{
    if (size > kCmdMaxBody) {
        log_msg(LOG_ERROR, "command 0x%02X body too large (%u), dropped", op, size);
        return;
    }
    if (dev->cmd_buf.size() + sizeof(espgpu_cmd_t) + size > kCmdFlushThreshold) {
        cmd_flush(dev, 500);
    }

    espgpu_cmd_t cmd{};
    cmd.op = op;
    cmd.flags = flags;
    cmd.size = static_cast<uint16_t>(size);

    const auto *p = reinterpret_cast<const uint8_t *>(&cmd);
    dev->cmd_buf.insert(dev->cmd_buf.end(), p, p + sizeof(cmd));

    if (size && body) {
        const auto *b = reinterpret_cast<const uint8_t *>(body);
        dev->cmd_buf.insert(dev->cmd_buf.end(), b, b + size);
    }
}

} // namespace espgpu

namespace {

inline void push(espgpu_device *dev, uint8_t op, const void *body, uint16_t size)
{
    cmd_push(dev, op, 0, body, size);
}

inline int flush(espgpu_device *dev, int timeout_ms)
{
    return cmd_flush(dev, timeout_ms);
}

constexpr size_t kFlushThreshold = kCmdFlushThreshold;

} // namespace

extern "C" {

int espgpu_cmd_clear(espgpu_device_t *dev, uint16_t color)
{
    if (dev == nullptr) {
        return -1;
    }
    espgpu_clear_t c{};
    c.flags = 1; /* 清颜色 */
    c.color = color;
    push(dev, ESPGPU_OP_CLEAR, &c, static_cast<uint16_t>(sizeof(c)));
    return 0;
}

int espgpu_cmd_set_pipeline(espgpu_device_t *dev, uint8_t blend, uint8_t filter, uint8_t wrap,
                            uint8_t alpha_test, uint8_t alpha_ref)
{
    if (dev == nullptr) {
        return -1;
    }
    espgpu_pipeline2d_t p{};
    p.blend = blend;
    p.filter = filter;
    p.wrap = wrap;
    p.alpha_test = alpha_test;
    p.alpha_ref = alpha_ref;
    push(dev, ESPGPU_OP_PIPELINE2D, &p, static_cast<uint16_t>(sizeof(p)));
    return 0;
}

int espgpu_cmd_bind_texture(espgpu_device_t *dev, uint32_t id, uint8_t filter, uint8_t wrap)
{
    if (dev == nullptr) {
        return -1;
    }
    espgpu_bind_texture_t b{};
    b.unit = 0;
    b.texture = static_cast<uint16_t>(id);
    b.filter = filter;
    b.wrap = wrap;
    push(dev, ESPGPU_OP_BIND_TEXTURE, &b, static_cast<uint16_t>(sizeof(b)));
    return 0;
}

int espgpu_cmd_upload_texture(espgpu_device_t *dev, uint32_t id, uint16_t width,
                              uint16_t height, uint8_t format, const void *pixels)
{
    if (dev == nullptr || pixels == nullptr || width == 0 || height == 0) {
        return -1;
    }

    if (uint64_t(width) * height * 2 > 0xFFFFFFFFu) {
        return -1; /* total_len 是 u32 */
    }
    const uint32_t total = static_cast<uint32_t>(width) * height * 2;
    /* 每个分片留出上传头 + 命令头的空间 */
    const uint32_t max_frag =
        static_cast<uint32_t>(kCmdMaxBody - sizeof(espgpu_upload_texture_t));
    const auto *src = static_cast<const uint8_t *>(pixels);

    uint32_t off = 0;
    while (off < total) {
        uint32_t n = total - off;
        if (n > max_frag) {
            n = max_frag;
        }
        n &= ~1u; /* 像素 16 位，偏移必须偶数对齐 */
        if (n == 0) {
            n = 2;
        }

        espgpu_upload_texture_t u{};
        u.id = id;
        u.width = width;
        u.height = height;
        u.offset = off;
        u.total_len = total;
        u.format = format;

        std::vector<uint8_t> body(sizeof(u) + n);
        memcpy(body.data(), &u, sizeof(u));
        memcpy(body.data() + sizeof(u), src + off, n);

        push(dev, ESPGPU_OP_UPLOAD_TEXTURE, body.data(),
             static_cast<uint16_t>(body.size()));
        off += n;
    }
    return 0;
}

int espgpu_cmd_draw_quads(espgpu_device_t *dev, const espgpu_vertex2d_t *verts,
                          uint32_t quad_count)
{
    if (dev == nullptr || verts == nullptr || quad_count == 0 || quad_count > 0x3FFFFFFFu) {
        return -1;
    }

    const uint32_t total = quad_count * 4;
    const uint32_t room =
        (static_cast<uint32_t>(kFlushThreshold) - 64 - sizeof(espgpu_draw_t)) /
        static_cast<uint32_t>(sizeof(espgpu_vertex2d_t));
    const uint32_t per_batch = room & ~3u;
    if (per_batch == 0) {
        return -1;
    }

    uint32_t done = 0;
    while (done < total) {
        uint32_t n = total - done;
        if (n > per_batch) {
            n = per_batch;
        }

        espgpu_draw_t d{};
        d.prim = ESPGPU_PRIM_QUADS;
        d.source = ESPGPU_VSRC_INLINE;
        d.vertex_count = static_cast<uint16_t>(n);
        d.index_count = 0;

        const size_t bytes = n * sizeof(espgpu_vertex2d_t);
        std::vector<uint8_t> body(sizeof(d) + bytes);
        memcpy(body.data(), &d, sizeof(d));
        memcpy(body.data() + sizeof(d), verts + done, bytes);

        push(dev, ESPGPU_OP_DRAW, body.data(), static_cast<uint16_t>(body.size()));
        done += n;
    }
    return 0;
}

int espgpu_cmd_scissor(espgpu_device_t *dev, int16_t x, int16_t y, int16_t w, int16_t h)
{
    if (dev == nullptr) {
        return -1;
    }
    espgpu_rect_t r{};
    r.x = static_cast<uint16_t>(x);
    r.y = static_cast<uint16_t>(y);
    r.w = static_cast<uint16_t>(w);
    r.h = static_cast<uint16_t>(h);
    push(dev, ESPGPU_OP_SCISSOR, &r, static_cast<uint16_t>(sizeof(r)));
    return 0;
}

int espgpu_cmd_present(espgpu_device_t *dev, int timeout_ms)
{
    if (dev == nullptr) {
        return -1;
    }
    push(dev, ESPGPU_OP_PRESENT, nullptr, 0);
    return flush(dev, timeout_ms);
}

void espgpu_cmd_discard(espgpu_device_t *dev)
{
    if (dev != nullptr) {
        dev->cmd_buf.clear();
    }
}

} // extern "C"
