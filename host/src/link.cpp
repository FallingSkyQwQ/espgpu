/*
 * link.cpp — Chunk 分帧、接收线程、控制通道
 *
 * 接收线程从 bulk IN 拿到的是纯字节流，需要：
 *   1. 用魔数滑动窗口重新对齐（设备重启或半截包后必须能自愈）
 *   2. 按 header.length 切出完整 Chunk
 *   3. 按类型分发：FRAME 走分片重组，CTRL_RESP 唤醒等待者
 */

#include "internal.h"

#include "espgpu_lz.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace espgpu {

namespace {

uint64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t now_us()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

/* ── 帧分片重组 ──────────────────────────────────────────────────── */

void on_frame_chunk(espgpu_device *dev, const espgpu_chunk_hdr_t &hdr, const uint8_t *payload)
{
    if (hdr.length < sizeof(espgpu_frame_hdr_t)) {
        return;
    }

    espgpu_frame_hdr_t fh;
    memcpy(&fh, payload, sizeof(fh));

    const uint8_t *data = payload + sizeof(fh);
    const uint32_t data_len = hdr.length - static_cast<uint32_t>(sizeof(fh));

    if (fh.flags & ESPGPU_FF_FIRST) {
        dev->cur_frame_id = fh.frame_id;
        dev->cur_raw_len = fh.raw_len;
        dev->cur_frag_count = fh.frag_count;
        dev->cur_frag_expect = 0;
        dev->cur_broken = false;
        dev->enc.clear();
        dev->enc.reserve(fh.raw_len);
    }

    if (fh.frame_id != dev->cur_frame_id || fh.frag_index != dev->cur_frag_expect) {
        /* 分片顺序错了或者中间丢了：这一帧作废，等下一个 FIRST */
        dev->cur_broken = true;
        dev->stats.fragments_lost++;
    }

    dev->enc.insert(dev->enc.end(), data, data + data_len);
    dev->cur_frag_expect = static_cast<uint8_t>(fh.frag_index + 1);

    if (fh.flags & ESPGPU_FF_LAST) {
        const bool complete = !dev->cur_broken && dev->cur_frag_expect == dev->cur_frag_count &&
                             dev->enc.size() == fh.total_enc;
        if (complete) {
            if (frame_decode(dev, dev->enc.data(), dev->enc.size(), fh.raw_len, fh.width,
                             fh.height, fh.format, fh.codec) != 0) {
                /* 解码失败 = 参考帧已经和设备的 shadow 错位，必须重新对齐，
                 * 否则后续所有差分帧都会带残影 */
                dev->need_keyframe = true;
            }
        } else {
            std::lock_guard<std::mutex> lk(dev->stats_mu);
            dev->stats.frames_dropped++;
            dev->stats.fragments_lost++;
            dev->need_keyframe = true;
        }
        dev->enc.clear();
        dev->cur_broken = false;
    }
}

void on_ctrl_resp(espgpu_device *dev, const espgpu_chunk_hdr_t &hdr, const uint8_t *payload)
{
    if (hdr.length < sizeof(espgpu_ctrl_resp_t)) {
        return;
    }
    espgpu_ctrl_resp_t r;
    memcpy(&r, payload, sizeof(r));

    std::lock_guard<std::mutex> lk(dev->ctrl_mu);
    auto it = dev->pending.find(r.seq);
    if (it == dev->pending.end()) {
        log_msg(LOG_DEBUG, "ctrl resp for unknown seq=%u op=0x%04X", r.seq, r.op);
        return;
    }
    it->second.done = true;
    it->second.status = r.status;
    if (r.len && hdr.length >= sizeof(r) + r.len) {
        it->second.data.assign(payload + sizeof(r), payload + sizeof(r) + r.len);
    }
    dev->ctrl_cv.notify_all();
}

void on_clut(espgpu_device *dev, const espgpu_chunk_hdr_t &hdr, const uint8_t *payload)
{
    if (hdr.length < sizeof(dev->clut)) {
        return;
    }
    std::lock_guard<std::mutex> lk(dev->frame_mu);
    memcpy(dev->clut, payload, sizeof(dev->clut));
}

void dispatch(espgpu_device *dev, const espgpu_chunk_hdr_t &hdr, const uint8_t *payload)
{
    switch (hdr.type) {
    case ESPGPU_CHUNK_FRAME:
        on_frame_chunk(dev, hdr, payload);
        break;
    case ESPGPU_CHUNK_CTRL_RESP:
        on_ctrl_resp(dev, hdr, payload);
        break;
    case ESPGPU_CHUNK_CLUT:
        on_clut(dev, hdr, payload);
        break;
    case ESPGPU_CHUNK_EVENT:
        if (hdr.length >= sizeof(espgpu_event_t)) {
            espgpu_event_t ev;
            memcpy(&ev, payload, sizeof(ev));
            log_msg(LOG_DEBUG, "event kind=%u code=%u value=%u arg=%u", ev.kind, ev.code,
                    ev.value, ev.arg);
            std::lock_guard<std::mutex> lk(dev->evt_mu);
            if (ev.kind == ESPGPU_EVT_FENCE) {
                dev->last_fence = ev.arg;
                dev->have_fence = true;
            }
            dev->events.push_back(ev);
            while (dev->events.size() > 1024) {
                dev->events.pop_front();
            }
            dev->evt_cv.notify_all();
        }
        break;
    case ESPGPU_CHUNK_LOG:
        log_msg(LOG_INFO, "device: %.*s", static_cast<int>(hdr.length), payload);
        break;
    default:
        log_msg(LOG_DEBUG, "unknown chunk type 0x%02X len=%u", hdr.type, hdr.length);
        break;
    }
}

void parse_rx(espgpu_device *dev)
{
    /* 重同步计数先在局部累加，最后一次性写回。
     * 之前每跳一个字节就 lock 一次 stats_mu，80 万次加锁直接把接收线程拖垮，
     * 表现为「丢数据 -> 更严重失步 -> 加锁更多」的死循环。 */
    uint32_t resyncs = 0;

    for (;;) {
        const size_t avail = dev->rx.size() - dev->rx_off;
        if (avail < 4) {
            break;
        }

        uint32_t magic;
        memcpy(&magic, dev->rx.data() + dev->rx_off, 4);
        if (magic != ESPGPU_MAGIC_D2H) {
            dev->rx_off++;
            resyncs++;
            continue;
        }

        if (avail < ESPGPU_CHUNK_HDR_SIZE) {
            break;
        }

        espgpu_chunk_hdr_t hdr;
        memcpy(&hdr, dev->rx.data() + dev->rx_off, sizeof(hdr));

        if (hdr.length > ESPGPU_CHUNK_MAX_PAYLOAD || hdr.version != ESPGPU_PROTO_VERSION) {
            /* 头部不可信，滑动一字节重新找 */
            dev->rx_off++;
            resyncs++;
            continue;
        }

        const size_t need = ESPGPU_CHUNK_HDR_SIZE + hdr.length +
                            ((hdr.flags & ESPGPU_F_CRC) ? ESPGPU_CRC_SIZE : 0);
        if (avail < need) {
            break; /* 等更多数据 */
        }

        const uint8_t *payload = dev->rx.data() + dev->rx_off + ESPGPU_CHUNK_HDR_SIZE;

        if (hdr.flags & ESPGPU_F_CRC) {
            uint16_t rx_crc = 0;
            memcpy(&rx_crc, payload + hdr.length, sizeof(rx_crc));
            uint16_t calc = espgpu_chunk_crc(&hdr, payload);
            if (rx_crc != calc) {
                std::lock_guard<std::mutex> lk(dev->stats_mu);
                dev->stats.crc_errors++;
                dev->rx_off += need;
                continue;
            }
        }

        dispatch(dev, hdr, payload);
        dev->rx_off += need;
    }

    if (resyncs) {
        std::lock_guard<std::mutex> lk(dev->stats_mu);
        dev->stats.resyncs += resyncs;
    }

    if (dev->rx_off > 0) {
        dev->rx.erase(dev->rx.begin(), dev->rx.begin() + static_cast<ptrdiff_t>(dev->rx_off));
        dev->rx_off = 0;
    }
}

void reader_main(espgpu_device *dev)
{
    std::vector<uint8_t> buf(64 * 1024);

    while (dev->running.load()) {
        DWORD got = 0;
        int rc = usb_read(dev, buf.data(), static_cast<DWORD>(buf.size()), 200, &got);
        if (rc == 0) {
            continue; /* 超时，回去检查 running */
        }
        if (rc < 0) {
            if (dev->running.load()) {
                log_msg(LOG_ERROR, "bulk IN failed, marking device offline");
                dev->connected = false;
                { std::lock_guard<std::mutex> lk(dev->frame_mu); dev->frame_cv.notify_all(); }
                { std::lock_guard<std::mutex> lk(dev->evt_mu); dev->evt_cv.notify_all(); }
            }
            break;
        }
        if (got == 0) {
            continue;
        }


        dev->rx.insert(dev->rx.end(), buf.data(), buf.data() + got);
        parse_rx(dev);

        /* 防御：单次解析后仍积压超过 1MB，说明流已经乱了 */
        if (dev->rx.size() > (1u << 20)) {
            log_msg(LOG_WARN, "rx backlog too large, dropping %zu bytes", dev->rx.size());
            dev->rx.clear();
            dev->rx_off = 0;
        }
    }
}

} // namespace

/* ── 发送 ─────────────────────────────────────────────────────────── */

int link_send_chunk_iov(espgpu_device *dev, uint8_t type, uint8_t flags, const void *p0,
                        uint32_t n0, const void *p1, uint32_t n1, DWORD timeout_ms)
{
    const uint32_t total = n0 + n1;
    if (total > ESPGPU_CHUNK_MAX_PAYLOAD) {
        return -1;
    }

    static std::atomic<uint16_t> seq{0};

    espgpu_chunk_hdr_t hdr{};
    hdr.magic = ESPGPU_MAGIC_H2D;
    hdr.version = ESPGPU_PROTO_VERSION;
    hdr.seq = seq++;
    hdr.length = static_cast<uint16_t>(total);
    hdr.type = type;
    hdr.flags = static_cast<uint8_t>(flags & ~(ESPGPU_F_CRC | ESPGPU_F_LZ));

    /* 头和载荷拼成一次写，避免设备端看到半截 Chunk */
    std::vector<uint8_t> frame;
    frame.resize(sizeof(hdr) + total);
    if (n0) {
        memcpy(frame.data() + sizeof(hdr), p0, n0);
    }
    if (n1) {
        memcpy(frame.data() + sizeof(hdr) + n0, p1, n1);
    }

    /* 下行压缩（PLAN §2 账本：命令流是 24fps 的主要带宽开销）。
     * 只压 CMD / DATA；压不动就发原文。载荷 = u32 原长 + LZ4 block。 */
    if (dev->compress.load() && total >= 256 &&
        (type == ESPGPU_CHUNK_CMD || type == ESPGPU_CHUNK_DATA)) {
        std::lock_guard<std::mutex> lk(dev->write_mu);
        dev->lz_table.resize(size_t(1) << ESPGPU_LZ_HASH_BITS);
        dev->lz_buf.resize(4 + espgpu_lz_bound(total));
        const size_t clen = espgpu_lz_encode(frame.data() + sizeof(hdr), total,
                                             dev->lz_buf.data() + 4, dev->lz_buf.size() - 4,
                                             dev->lz_table.data());
        if (clen > 0 && clen + 4 < total) {
            memcpy(dev->lz_buf.data(), &total, 4);
            hdr.flags |= ESPGPU_F_LZ;
            hdr.length = static_cast<uint16_t>(clen + 4);
            frame.resize(sizeof(hdr) + clen + 4);
            memcpy(frame.data() + sizeof(hdr), dev->lz_buf.data(), clen + 4);
        }
    }
    memcpy(frame.data(), &hdr, sizeof(hdr));

    if (!usb_write(dev, frame.data(), frame.size(), timeout_ms)) {
        return -1;
    }
    return 0;
}

int link_send_chunk(espgpu_device *dev, uint8_t type, uint8_t flags, const void *payload,
                    uint32_t len, DWORD timeout_ms)
{
    return link_send_chunk_iov(dev, type, flags, payload, len, nullptr, 0, timeout_ms);
}

/* ── 控制请求 ─────────────────────────────────────────────────────── */

int link_ctrl(espgpu_device *dev, uint16_t op, uint32_t a0, uint32_t a1, const void *data,
              uint16_t len, std::vector<uint8_t> &resp, DWORD timeout_ms)
{
    const uint16_t seq = dev->next_seq++;

    std::vector<uint8_t> payload(sizeof(espgpu_ctrl_req_t) + len);
    auto *req = reinterpret_cast<espgpu_ctrl_req_t *>(payload.data());
    req->op = op;
    req->seq = seq;
    req->arg0 = a0;
    req->arg1 = a1;
    if (len && data) {
        memcpy(payload.data() + sizeof(*req), data, len);
    }

    {
        std::lock_guard<std::mutex> lk(dev->ctrl_mu);
        dev->pending[seq] = PendingCtrl{};
    }

    const uint64_t t0 = now_us();

    if (link_send_chunk(dev, ESPGPU_CHUNK_CTRL, 0, payload.data(),
                        static_cast<uint32_t>(payload.size()), timeout_ms) != 0) {
        std::lock_guard<std::mutex> lk(dev->ctrl_mu);
        dev->pending.erase(seq);
        return -1;
    }

    std::unique_lock<std::mutex> lk(dev->ctrl_mu);
    const bool ok = dev->ctrl_cv.wait_for(
        lk, std::chrono::milliseconds(timeout_ms), [&] {
            auto it = dev->pending.find(seq);
            return it != dev->pending.end() && it->second.done;
        });

    if (!ok) {
        dev->pending.erase(seq);
        log_msg(LOG_WARN, "control op 0x%04X timed out after %d ms", op, timeout_ms);
        return -2;
    }

    PendingCtrl pc = std::move(dev->pending[seq]);
    dev->pending.erase(seq);
    lk.unlock();

    resp = std::move(pc.data);

    const uint64_t dt_us = now_us() - t0;
    {
        std::lock_guard<std::mutex> slk(dev->stats_mu);
        dev->stats.rtt_ms = static_cast<float>(dt_us) / 1000.0f;
    }

    return pc.status;
}

/* ── 生命周期 ─────────────────────────────────────────────────────── */

void link_start(espgpu_device *dev)
{
    dev->running = true;
    dev->rx.clear();
    dev->rx_off = 0;
    dev->reader = std::thread(reader_main, dev);
}

void link_stop(espgpu_device *dev)
{
    dev->running = false;
    if (dev->reader.joinable()) {
        dev->reader.join();
    }
    /* 唤醒所有还在等待响应的控制请求、帧和 fence */
    {
        std::lock_guard<std::mutex> lk(dev->ctrl_mu);
        dev->ctrl_cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(dev->frame_mu);
        dev->frame_cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(dev->evt_mu);
        dev->evt_cv.notify_all();
    }
}

} // namespace espgpu
