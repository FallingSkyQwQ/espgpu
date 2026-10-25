/*
 * emulator.cpp — 软件模拟器（ESPGPU_EMULATOR=1）
 *
 * 在进程内跑和固件同一份 firmware/core，对外表现成一条 USB 字节流：
 * usb_write 把 H2D Chunk 喂进来，模拟器线程解析、执行，把 D2H Chunk（帧、事件、
 * 控制响应）写进输出队列，usb_read 再取走。link / decode 层完全不知道区别，
 * 所以 GL 前端、面板、demo 都能在没有硬件时端到端跑起来，渲染结果与设备逐像素一致
 * （同一份光栅代码），只是没有 USB 带宽限制。
 */

#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "core.h"
#include "espgpu_lz.h"
#include "espgpu_block.h"
void gp_host_set_log_hook(void (*fn)(int level, const char *msg));
}

struct EmuState {
    std::thread th;
    std::mutex mu;
    std::condition_variable cv_in;
    std::condition_variable cv_out;
    std::condition_variable cv_space; /**< in 有空位了（模拟 USB 背压） */
    std::vector<uint8_t> in;
    std::vector<uint8_t> out;
    size_t out_off = 0;
    bool stop = false;

    gpu_core_t *core = nullptr;
    espgpu_mode_t mode{};
    espgpu_stats_t stats{};
    uint32_t frame_id = 1; /* 0 = 还没有帧 */
    uint16_t seq = 0;
    uint8_t codec = ESPGPU_CODEC_RLE;
    uint8_t debug_view = 0;
    uint16_t cpu_mhz = 240;
    std::vector<uint8_t> lz;
    std::chrono::steady_clock::time_point boot = std::chrono::steady_clock::now();

    /* OTA：模拟器只校验流程，不落盘 */
    uint32_t ota_total = 0;
    uint32_t ota_written = 0;
    uint8_t ota_state = 0;
};

namespace espgpu {

namespace {

constexpr uint32_t kTexBudget = 4u * 1024u * 1024u;

/* 真设备有 USB 背压；模拟器没有的话，应用提交得比核心渲染快时 in 会无限增长、
 * 延迟无限增大。in 超过上限时 emu_write 阻塞（最多 timeout_ms）。 */
constexpr size_t kInCap = 1u << 20;
/* 接收线程跟不上时（比如调试器停住），out 超过上限就丢帧而不是无限攒内存 */
constexpr size_t kOutCap = 16u << 20;

void core_log(int level, const char *msg)
{
    log_msg(level <= 1 ? LOG_ERROR : level == 2 ? LOG_WARN : LOG_DEBUG, "emu core: %s", msg);
}

/** 在 out 里追加一个 D2H Chunk。调用者持有 e->mu。 */
void emit_locked(EmuState *e, uint8_t type, const void *p0, uint32_t n0, const void *p1,
                 uint32_t n1)
{
    espgpu_chunk_hdr_t h{};
    h.magic = ESPGPU_MAGIC_D2H;
    h.version = ESPGPU_PROTO_VERSION;
    h.seq = e->seq++;
    h.length = static_cast<uint16_t>(n0 + n1);
    h.type = type;
    h.flags = 0;
    const auto *ph = reinterpret_cast<const uint8_t *>(&h);
    e->out.insert(e->out.end(), ph, ph + sizeof(h));
    if (n0) {
        const auto *a = static_cast<const uint8_t *>(p0);
        e->out.insert(e->out.end(), a, a + n0);
    }
    if (n1) {
        const auto *b = static_cast<const uint8_t *>(p1);
        e->out.insert(e->out.end(), b, b + n1);
    }
    e->cv_out.notify_all();
}

void emit(EmuState *e, uint8_t type, const void *p0, uint32_t n0, const void *p1 = nullptr,
          uint32_t n1 = 0)
{
    std::lock_guard<std::mutex> lk(e->mu);
    emit_locked(e, type, p0, n0, p1, n1);
}

void ctrl_resp(EmuState *e, const espgpu_ctrl_req_t &req, uint16_t status, const void *data,
               uint16_t len)
{
    espgpu_ctrl_resp_t r{};
    r.op = req.op;
    r.seq = req.seq;
    r.status = status;
    r.len = len;
    emit(e, ESPGPU_CHUNK_CTRL_RESP, &r, sizeof(r), data, len);
}

void on_event(void *user, const espgpu_event_t *ev)
{
    auto *e = static_cast<EmuState *>(user);
    espgpu_event_t out = *ev;
    if (out.kind == ESPGPU_EVT_FENCE && e->frame_id > 1) {
        /* 与固件相同：带上最后产出的帧号（on_present 在同一线程里递增 frame_id） */
        out.code |= ESPGPU_EVTF_FENCE_FRAME;
        out.value = static_cast<uint16_t>(e->frame_id - 1);
    }
    emit(e, ESPGPU_CHUNK_EVENT, &out, sizeof(out));
}

/** PRESENT：按 RLE 编码后分片发出（RLE 比 RAW 少拷很多字节，解码也走正常路径） */
void on_present(void *user, const uint16_t *px, uint16_t w, uint16_t h)
{
    auto *e = static_cast<EmuState *>(user);
    const size_t pixels = static_cast<size_t>(w) * h;
    const uint32_t raw = static_cast<uint32_t>(pixels * 2);

    /* BLOCK 模式发有损块关键帧（让没有硬件时也能看到有损画质）；
     * 其余模式（含 AUTO：模拟器没有带宽限制，永远走无损分支）发像素级 RLE 关键帧，
     * 格式与 framebuffer.c 相同 */
    std::vector<uint8_t> enc;
    const bool block = e->codec == ESPGPU_CODEC_BLOCK;
    size_t i = block ? pixels : 0;
    if (block) {
        enc.resize(espgpu_block_max_encoded(w, h));
        enc.resize(espgpu_block_encode(px, nullptr, w, h, 1, enc.data(), enc.size()));
    } else {
        enc.reserve(raw / 4);
    }
    while (i < pixels) {
        size_t run = 1;
        while (i + run < pixels && run < 129 && px[i + run] == px[i]) {
            run++;
        }
        if (run >= 2) {
            enc.push_back(static_cast<uint8_t>(0x80 | (run - 2)));
            enc.push_back(static_cast<uint8_t>(px[i] & 0xFF));
            enc.push_back(static_cast<uint8_t>(px[i] >> 8));
            i += run;
            continue;
        }
        const size_t lit = i;
        i++;
        while (i < pixels && i - lit < 128) {
            if (i + 2 < pixels && px[i] == px[i + 1] && px[i + 1] == px[i + 2]) {
                break;
            }
            i++;
        }
        const size_t n = i - lit;
        enc.push_back(static_cast<uint8_t>(n - 1));
        for (size_t k = 0; k < n; k++) {
            enc.push_back(static_cast<uint8_t>(px[lit + k] & 0xFF));
            enc.push_back(static_cast<uint8_t>(px[lit + k] >> 8));
        }
    }

    const uint32_t frag_cap = ESPGPU_CHUNK_MAX_PAYLOAD - static_cast<uint32_t>(sizeof(espgpu_frame_hdr_t));
    const uint32_t total = static_cast<uint32_t>(enc.size());
    uint32_t count = (total + frag_cap - 1) / frag_cap;
    if (count == 0) {
        count = 1;
    }
    std::lock_guard<std::mutex> lk(e->mu);
    if (e->out.size() - e->out_off > kOutCap || count > 255) {
        /* 模拟器只发 RLE 关键帧，丢一帧不会破坏后续帧 */
        e->frame_id++;
        e->stats.frames_dropped++;
        return;
    }
    uint32_t off = 0;
    for (uint32_t f = 0; f < count; f++) {
        const uint32_t n = std::min(frag_cap, total - off);
        espgpu_frame_hdr_t fh{};
        fh.frame_id = e->frame_id;
        fh.width = w;
        fh.height = h;
        fh.origin_w = w;
        fh.origin_h = h;
        fh.data_len = n;
        fh.raw_len = raw;
        fh.total_enc = total;
        fh.format = ESPGPU_FMT_RGB565;
        fh.codec = block ? ESPGPU_CODEC_BLOCK : ESPGPU_CODEC_RLE;
        fh.frag_index = static_cast<uint8_t>(f);
        fh.frag_count = static_cast<uint8_t>(count);
        fh.flags = static_cast<uint8_t>((f == 0 ? ESPGPU_FF_FIRST : 0) |
                                        (f + 1 == count ? ESPGPU_FF_LAST : 0));
        emit_locked(e, ESPGPU_CHUNK_FRAME, &fh, sizeof(fh), enc.data() + off, n);
        off += n;
    }
    e->frame_id++;
    e->stats.frames_presented++;
    e->stats.frame_raw_bytes = raw;
    e->stats.frame_enc_bytes = total;
}

void fill_info(EmuState *e, espgpu_device_info_t &di)
{
    memset(&di, 0, sizeof(di));
    di.proto_version = ESPGPU_PROTO_VERSION;
    di.fw_major = 0;
    di.fw_minor = 2;
    di.fw_patch = 0;
    snprintf(di.chip_model, sizeof(di.chip_model), "Software emulator");
    di.max_width = 800;
    di.max_height = 600;
    di.min_width = 32;
    di.min_height = 32;
    di.psram_mb = 8;
    di.flash_mb = 16;
    di.psram_total = 8u << 20;
    di.psram_free = 6u << 20;
    di.sram_total = 512u << 10;
    di.sram_free = 200u << 10;
    di.flash_total = 16u << 20;
    di.capabilities = ESPGPU_CAP_INDEX8 | ESPGPU_CAP_RGB565 | ESPGPU_CAP_DEPTH | ESPGPU_CAP_FBO |
                      ESPGPU_CAP_FRAGMENT_VM | ESPGPU_CAP_QUADS | ESPGPU_CAP_LIGHTING |
                      ESPGPU_CAP_OTA | ESPGPU_CAP_TNL | ESPGPU_CAP_TEX_LRU |
                      ESPGPU_CAP_PALETTED_TEX | ESPGPU_CAP_BUFFERS | ESPGPU_CAP_CLOCK |
                      ESPGPU_CAP_DEBUG_VIEW | ESPGPU_CAP_LZ;
    di.codecs = ESPGPU_CAP_CODEC_RAW | ESPGPU_CAP_CODEC_RLE | ESPGPU_CAP_CODEC_DELTA_RLE |
                ESPGPU_CAP_CODEC_BLOCK | ESPGPU_CAP_CODEC_AUTO;
    di.texture_cache_size = kTexBudget;
    di.max_texture_dim = 512;
    di.max_texture_units = ESPGPU_MAX_TEXTURE_UNITS;
    di.max_vertex_attribs = ESPGPU_MAX_VERTEX_ATTRIBS;
    di.cpu_freq_mhz = static_cast<uint8_t>(e->cpu_mhz > 255 ? 240 : e->cpu_mhz);
    di.psram_freq_mhz = 80;
}

void handle_ctrl(EmuState *e, const espgpu_ctrl_req_t &req, const uint8_t *extra, uint32_t n)
{
    uint16_t st = ESPGPU_OK;
    switch (req.op) {
    case ESPGPU_CTRL_PING:
        ctrl_resp(e, req, ESPGPU_OK, &req, sizeof(req));
        return;
    case ESPGPU_CTRL_GET_DEVICE_INFO: {
        espgpu_device_info_t di;
        fill_info(e, di);
        ctrl_resp(e, req, ESPGPU_OK, &di, sizeof(di));
        return;
    }
    case ESPGPU_CTRL_GET_STATS: {
        espgpu_stats_t s = e->stats;
        gpu_core_fill_stats(e->core, &s);
        s.uptime_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - e->boot)
                                                .count());
        s.internal_temp_c10 = 420;
        s.psram_temp_c10 = INT16_MIN;
        s.cpu_freq_mhz = e->cpu_mhz;
        s.ota_state = e->ota_state;
        s.ota_written = e->ota_written;
        s.free_psram = 6u << 20;
        s.free_heap = 200u << 10;
        s.min_free_heap = 180u << 10;
        ctrl_resp(e, req, ESPGPU_OK, &s, sizeof(s));
        return;
    }
    case ESPGPU_CTRL_SET_MODE:
        if (n >= sizeof(espgpu_mode_t)) {
            memcpy(&e->mode, extra, sizeof(e->mode));
            uint16_t w = e->mode.width < 32 ? 32 : (e->mode.width > 800 ? 800 : e->mode.width);
            uint16_t h = e->mode.height < 32 ? 32 : (e->mode.height > 600 ? 600 : e->mode.height);
            if (!gpu_core_set_size(e->core, w, h)) {
                st = ESPGPU_ERR_NO_MEM;
            }
            e->codec = e->mode.codec;
        }
        break;
    case ESPGPU_CTRL_SET_CLOCK:
        if (req.arg0 == 80 || req.arg0 == 160 || req.arg0 == 240) {
            e->cpu_mhz = static_cast<uint16_t>(req.arg0);
        } else {
            st = ESPGPU_ERR_INVALID_ARG;
        }
        break;
    case ESPGPU_CTRL_SET_DEBUG_VIEW:
        gpu_core_set_debug_view(e->core, static_cast<uint8_t>(req.arg0));
        break;
    case ESPGPU_CTRL_RESET_STATS:
        e->stats = espgpu_stats_t{};
        break;
    case ESPGPU_CTRL_OTA_BEGIN:
        e->ota_total = req.arg0;
        e->ota_written = 0;
        e->ota_state = 1;
        break;
    case ESPGPU_CTRL_OTA_DATA:
        if (e->ota_state != 1 || req.arg0 != e->ota_written) {
            st = ESPGPU_ERR_INVALID_ARG;
        } else {
            e->ota_written += n;
        }
        break;
    case ESPGPU_CTRL_OTA_END:
        e->ota_state = (e->ota_written == e->ota_total) ? 2 : 3;
        st = e->ota_state == 2 ? ESPGPU_OK : ESPGPU_ERR_INVALID_ARG;
        break;
    case ESPGPU_CTRL_OTA_ABORT:
        e->ota_state = 0;
        break;
    case ESPGPU_CTRL_SET_CLUT:
    case ESPGPU_CTRL_SET_CODEC:
        if (req.arg0 <= ESPGPU_CODEC_AUTO && req.arg0 != ESPGPU_CODEC_TILE_DELTA) {
            e->codec = static_cast<uint8_t>(req.arg0);
        } else {
            st = ESPGPU_ERR_UNSUPPORTED;
        }
        break;
    case ESPGPU_CTRL_REQUEST_KEYFRAME:
    case ESPGPU_CTRL_LOG_SUBSCRIBE:
    case ESPGPU_CTRL_SOFT_RESET:
        break;
    default:
        st = ESPGPU_ERR_UNSUPPORTED;
        break;
    }
    ctrl_resp(e, req, st, nullptr, 0);
}

const uint8_t *unpack(EmuState *e, const espgpu_chunk_hdr_t &h, const uint8_t *p, uint32_t *len)
{
    if (!(h.flags & ESPGPU_F_LZ)) {
        *len = h.length;
        return p;
    }
    if (h.length < 4) {
        return nullptr;
    }
    uint32_t raw;
    memcpy(&raw, p, 4);
    if (raw > (1u << 20)) {
        return nullptr;
    }
    e->lz.resize(raw);
    if (espgpu_lz_decode(p + 4, h.length - 4, e->lz.data(), raw) != raw) {
        log_msg(LOG_WARN, "emulator: corrupt LZ chunk");
        return nullptr;
    }
    e->stats.lz_in_bytes += h.length;
    e->stats.lz_out_bytes += raw;
    *len = raw;
    return e->lz.data();
}

void emu_main(EmuState *e)
{
    std::vector<uint8_t> buf;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(e->mu);
            e->cv_in.wait(lk, [&] { return e->stop || !e->in.empty(); });
            if (e->stop) {
                return;
            }
            buf.insert(buf.end(), e->in.begin(), e->in.end());
            e->in.clear();
        }
        e->cv_space.notify_all();

        size_t off = 0;
        for (;;) {
            if (buf.size() - off < ESPGPU_CHUNK_HDR_SIZE) {
                break;
            }
            uint32_t magic;
            memcpy(&magic, buf.data() + off, 4);
            espgpu_chunk_hdr_t h;
            memcpy(&h, buf.data() + off, sizeof(h));
            if (magic != ESPGPU_MAGIC_H2D || h.version != ESPGPU_PROTO_VERSION ||
                h.length > ESPGPU_CHUNK_MAX_PAYLOAD) {
                off++;
                continue;
            }
            const size_t need = ESPGPU_CHUNK_HDR_SIZE + h.length +
                                ((h.flags & ESPGPU_F_CRC) ? ESPGPU_CRC_SIZE : 0);
            if (buf.size() - off < need) {
                break;
            }
            const uint8_t *payload = buf.data() + off + ESPGPU_CHUNK_HDR_SIZE;
            e->stats.bytes_in += static_cast<uint32_t>(need);

            switch (h.type) {
            case ESPGPU_CHUNK_CTRL:
                if (h.length >= sizeof(espgpu_ctrl_req_t)) {
                    espgpu_ctrl_req_t req;
                    memcpy(&req, payload, sizeof(req));
                    handle_ctrl(e, req, payload + sizeof(req),
                                h.length - static_cast<uint32_t>(sizeof(req)));
                }
                break;
            case ESPGPU_CHUNK_CMD: {
                uint32_t n = 0;
                const uint8_t *p = unpack(e, h, payload, &n);
                if (p) {
                    gpu_core_exec(e->core, p, n);
                    e->stats.cmd_count++;
                }
                break;
            }
            case ESPGPU_CHUNK_DATA: {
                uint32_t n = 0;
                const uint8_t *p = unpack(e, h, payload, &n);
                if (p) {
                    gpu_core_data(e->core, p, n);
                }
                break;
            }
            default:
                break;
            }
            off += need;
        }
        buf.erase(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(off));
    }
}

} // namespace

bool emulator_enabled()
{
    const char *v = getenv("ESPGPU_EMULATOR");
    return v && v[0] && strcmp(v, "0") != 0;
}

bool emu_open(espgpu_device *dev, std::string &err)
{
    auto *e = new (std::nothrow) EmuState();
    if (e == nullptr) {
        err = "out of memory";
        return false;
    }
    gp_host_set_log_hook(core_log);
    gpu_core_config_t cfg{};
    cfg.arena_bytes = 4u << 20;
    cfg.texture_budget = kTexBudget;
    cfg.buffer_budget = 4u << 20;
    cfg.max_texture_dim = 512;
    gpu_core_cb_t cb{};
    cb.send_event = on_event;
    cb.present = on_present;
    cb.user = e;
    e->core = gpu_core_create(&cfg, &cb);
    if (e->core == nullptr) {
        delete e;
        err = "emulator core init failed";
        return false;
    }
    e->mode.width = 320;
    e->mode.height = 200;
    e->mode.format = ESPGPU_FMT_RGB565;
    e->mode.codec = ESPGPU_CODEC_RLE;
    e->mode.max_fps = 24;
    dev->emu = e;
    dev->path = kEmulatorPath;
    dev->connected = true;
    e->th = std::thread(emu_main, e);
    log_msg(LOG_INFO, "opened software emulator (firmware/core on the host CPU)");
    return true;
}

void emu_close(espgpu_device *dev)
{
    EmuState *e = dev->emu;
    if (e == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(e->mu);
        e->stop = true;
    }
    e->cv_in.notify_all();
    e->cv_out.notify_all();
    e->cv_space.notify_all();
    if (e->th.joinable()) {
        e->th.join();
    }
    gpu_core_destroy(e->core);
    delete e;
    dev->emu = nullptr;
}

bool emu_write(espgpu_device *dev, const void *data, size_t len, DWORD timeout_ms)
{
    EmuState *e = dev->emu;
    {
        std::unique_lock<std::mutex> lk(e->mu);
        const bool room = e->cv_space.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
            return e->stop || !dev->running.load() || e->in.size() < kInCap;
        });
        if (e->stop || !dev->running.load() || !room) {
            return false;
        }
        const auto *p = static_cast<const uint8_t *>(data);
        e->in.insert(e->in.end(), p, p + len);
    }
    e->cv_in.notify_all();
    return true;
}

int emu_read(espgpu_device *dev, void *buf, DWORD cap, DWORD timeout_ms, DWORD *got)
{
    EmuState *e = dev->emu;
    std::unique_lock<std::mutex> lk(e->mu);
    e->cv_out.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
        return e->stop || !dev->running.load() || e->out_off < e->out.size();
    });
    if (e->stop) {
        return -1;
    }
    const size_t avail = e->out.size() - e->out_off;
    if (avail == 0) {
        return 0;
    }
    const size_t n = std::min<size_t>(avail, cap);
    memcpy(buf, e->out.data() + e->out_off, n);
    e->out_off += n;
    if (e->out_off == e->out.size()) {
        e->out.clear();
        e->out_off = 0;
    } else if (e->out_off > (1u << 20)) {
        e->out.erase(e->out.begin(), e->out.begin() + static_cast<ptrdiff_t>(e->out_off));
        e->out_off = 0;
    }
    *got = static_cast<DWORD>(n);
    return 1;
}

void emu_abort(espgpu_device *dev)
{
    EmuState *e = dev->emu;
    if (e == nullptr) {
        return;
    }
    /* 等待者的谓词里都看 dev->running；加锁再通知，避免丢唤醒 */
    std::lock_guard<std::mutex> lk(e->mu);
    e->cv_out.notify_all();
    e->cv_space.notify_all();
}

} // namespace espgpu
