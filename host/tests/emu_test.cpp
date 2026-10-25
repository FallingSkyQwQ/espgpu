/*
 * espgpu-emu-test — espgpu.dll 的端到端测试（软件模拟器，不需要硬件）
 *
 * 覆盖：draw3d 拆批（索引重编号 / 越界索引 / strip 展开 / 空索引）与快路径逐像素一致，
 * fence / 帧等待，以及 espgpu_close 唤醒并等待其他线程里阻塞的调用（之前是 use-after-free）。
 * ctest 会设 ESPGPU_EMULATOR=1；手动运行时这里也会设。ESPGPU_TEST_LZ=1 打开下行压缩。
 */
#include "espgpu/espgpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

struct V {
    float x, y, z;
    uint16_t c;
    uint16_t pad;
};

static int fails = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);       \
            fails++;                                                  \
        }                                                             \
    } while (0)

static void setup(espgpu_device_t *dev)
{
    espgpu_vertex_layout_t l{};
    l.stride = sizeof(V);
    l.attrib_count = 2;
    l.index_type = 1;
    l.attribs[0] = {ESPGPU_ATTR_POSITION, ESPGPU_TYPE_F32, 3, 0, 0, 0, 0};
    l.attribs[1] = {ESPGPU_ATTR_COLOR, ESPGPU_TYPE_RGB565, 1, 0, 12, 0, 0};
    espgpu_cmd_viewport(dev, 0, 0, 320, 200);
    espgpu_cmd_scissor(dev, 0, 0, 320, 200);
    espgpu_cmd_clear_ex(dev, 3, 0, 0xFFFF);
    espgpu_cmd_matrix(dev, ESPGPU_MATRIX_MVP, nullptr);
    espgpu_cmd_vertex_layout(dev, &l);
}

static std::vector<uint8_t> grab(espgpu_device_t *dev)
{
    const uint32_t before = espgpu_frame_id(dev);
    EXPECT(espgpu_cmd_present(dev, 1000) == 0);
    EXPECT(espgpu_wait_frame(dev, before, 2000) == 1);
    std::vector<uint8_t> px(320 * 200 * 4);
    int w = 0, h = 0;
    EXPECT(espgpu_frame_snapshot(dev, px.data(), px.size(), &w, &h, nullptr) == 0);
    EXPECT(w == 320 && h == 200);
    return px;
}

static uint32_t bytes_out(espgpu_device_t *dev)
{
    return espgpu_link_stats(dev)->bytes_out;
}

static size_t nonblack(const std::vector<uint8_t> &p)
{
    size_t n = 0;
    for (size_t i = 0; i < p.size(); i += 4) n += (p[i] | p[i + 1] | p[i + 2]) != 0;
    return n;
}

int main()
{
#if defined(_WIN32)
    _putenv_s("ESPGPU_EMULATOR", "1");
#else
    setenv("ESPGPU_EMULATOR", "1", 1);
#endif
    espgpu_set_log_level(1);
    char err[256];
    espgpu_device_t *dev = espgpu_open(0, err, sizeof(err));
    if (!dev) {
        printf("open failed: %s\n", err);
        return 1;
    }
    espgpu_set_compression(dev, getenv("ESPGPU_TEST_LZ") != nullptr);

    /* 网格：N×N 顶点，索引三角形 */
    const int N = 64;
    std::vector<V> verts;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            V v{};
            v.x = -0.95f + 1.9f * x / (N - 1) + 0.01f * std::sin(y * 0.7f);
            v.y = -0.95f + 1.9f * y / (N - 1);
            v.z = 0.0f;
            v.c = uint16_t((x * 2654435761u + y * 40503u) >> 7);
            verts.push_back(v);
        }
    std::vector<uint16_t> idx;
    for (int y = 0; y + 1 < N; y++)
        for (int x = 0; x + 1 < N; x++) {
            const uint16_t a = uint16_t(y * N + x), b = uint16_t(a + 1), c = uint16_t(a + N), d = uint16_t(c + 1);
            idx.insert(idx.end(), {a, b, c, b, d, c});
        }
    std::vector<V> flat;
    for (uint16_t i : idx) flat.push_back(verts[i]);

    /* 参考：非索引列表（简单按顶点切） */
    setup(dev);
    uint32_t b0 = bytes_out(dev);
    EXPECT(espgpu_cmd_draw(dev, ESPGPU_PRIM_TRIANGLES, flat.data(), uint32_t(flat.size()), sizeof(V), nullptr, 0, 1) == 0);
    const auto ref = grab(dev);
    const uint32_t b_flat = bytes_out(dev) - b0;
    EXPECT(nonblack(ref) > 320 * 200 / 2);

    /* 索引绘制，走拆批路径 */
    setup(dev);
    b0 = bytes_out(dev);
    EXPECT(espgpu_cmd_draw(dev, ESPGPU_PRIM_TRIANGLES, verts.data(), uint32_t(verts.size()), sizeof(V), idx.data(), uint32_t(idx.size()), 1) == 0);
    const auto got = grab(dev);
    const uint32_t b_idx = bytes_out(dev) - b0;
    EXPECT(got == ref);

    /* 同样的索引用 u32 传（与布局 u16 不一致） */
    std::vector<uint32_t> idx32(idx.begin(), idx.end());
    setup(dev);
    EXPECT(espgpu_cmd_draw(dev, ESPGPU_PRIM_TRIANGLES, verts.data(), uint32_t(verts.size()), sizeof(V), idx32.data(), uint32_t(idx32.size()), 2) == 0);
    EXPECT(grab(dev) == ref);

    /* 越界索引：设备语义是丢掉整个三角形；拆批路径必须一致 */
    {
        std::vector<uint16_t> bad = idx;
        bad[4] = 60000; /* 第二个三角形作废 */
        std::vector<V> flat2;
        for (size_t t = 0; t < bad.size(); t += 3) {
            if (t == 3) continue;
            for (int k = 0; k < 3; k++) flat2.push_back(verts[bad[t + k]]);
        }
        setup(dev);
        espgpu_cmd_draw(dev, ESPGPU_PRIM_TRIANGLES, flat2.data(), uint32_t(flat2.size()), sizeof(V), nullptr, 0, 1);
        const auto r2 = grab(dev);
        setup(dev);
        espgpu_cmd_draw(dev, ESPGPU_PRIM_TRIANGLES, verts.data(), uint32_t(verts.size()), sizeof(V), bad.data(), uint32_t(bad.size()), 1);
        EXPECT(grab(dev) == r2);
        EXPECT(r2 != ref);
    }

    /* 非索引三角形带：拆批（重新编号）vs 小批量直接发（快路径，设备自己展开） */
    {
        std::vector<V> strip;
        for (int i = 0; i < 4000; i++) {
            V v{};
            v.x = -0.9f + 1.8f * (i / 2) / 1999.0f;
            v.y = (i & 1) ? 0.5f : -0.5f + 0.3f * std::sin(i * 0.01f);
            v.c = uint16_t(i * 37);
            strip.push_back(v);
        }
        setup(dev);
        /* 参考：每段 1000 个顶点（16KB，快路径），段间重叠 2 个顶点并保持奇偶 */
        for (int s = 0; s + 2 < 4000; s += 998) {
            const int n = std::min(1000, 4000 - s);
            EXPECT(espgpu_cmd_draw(dev, ESPGPU_PRIM_TRIANGLE_STRIP, strip.data() + s, uint32_t(n), sizeof(V), nullptr, 0, 1) == 0);
        }
        const auto rs = grab(dev);
        setup(dev);
        b0 = bytes_out(dev);
        EXPECT(espgpu_cmd_draw(dev, ESPGPU_PRIM_TRIANGLE_STRIP, strip.data(), 4000, sizeof(V), nullptr, 0, 1) == 0);
        const auto gs = grab(dev);
        printf("strip 4000 verts: %u bytes\n", bytes_out(dev) - b0);
        EXPECT(gs == rs);
        EXPECT(nonblack(rs) > 1000);
    }

    /* 空索引绘制：什么都不画 */
    setup(dev);
    EXPECT(espgpu_cmd_draw(dev, ESPGPU_PRIM_TRIANGLES, verts.data(), 3, sizeof(V), idx.data(), 0, 1) == 0);
    EXPECT(nonblack(grab(dev)) == 0);

    printf("mesh %dx%d: flat list %u bytes, indexed draw %u bytes (%.1fx less)\n", N, N, b_flat, b_idx,
           double(b_flat) / b_idx);

    /* 有损块编码（ESPGPU_CODEC_BLOCK）：模拟器发 BLOCK 关键帧，主机按块解码。
     * 与无损参考相比：有误差（不是逐字节相同），但平均误差很小；切回无损后又逐字节一致。 */
    {
        EXPECT(espgpu_set_codec(dev, ESPGPU_CODEC_BLOCK, 1000) == 0);
        setup(dev);
        EXPECT(espgpu_cmd_draw(dev, ESPGPU_PRIM_TRIANGLES, flat.data(), uint32_t(flat.size()), sizeof(V), nullptr, 0, 1) == 0);
        const auto blk = grab(dev);
        EXPECT(blk.size() == ref.size());
        double err = 0;
        for (size_t i = 0; i < blk.size() && i < ref.size(); i++) {
            if ((i & 3) != 3) err += std::abs(int(blk[i]) - int(ref[i]));
        }
        err /= double(ref.size()) * 0.75;
        printf("block codec: mean abs error %.2f / 255 per channel\n", err);
        /* 这张网格是逐顶点随机色（BC1 的最坏情况），真实画面误差小得多（~32 dB PSNR） */
        EXPECT(err > 0.0 && err < 24.0);
        EXPECT(espgpu_set_codec(dev, ESPGPU_CODEC_RLE, 1000) == 0);
        setup(dev);
        EXPECT(espgpu_cmd_draw(dev, ESPGPU_PRIM_TRIANGLES, flat.data(), uint32_t(flat.size()), sizeof(V), nullptr, 0, 1) == 0);
        EXPECT(grab(dev) == ref);
    }

    /* fence 往返 */
    espgpu_cmd_fence(dev, 7);
    EXPECT(espgpu_wait_fence(dev, 7, 1000) == 0);

    /* 关闭时唤醒阻塞者：另一个线程在等永远不会来的 fence 和帧 */
    std::atomic<int> rc_f{99}, rc_w{99};
    std::thread t1([&] { rc_f = espgpu_wait_fence(dev, 1000000, 20000); });
    std::thread t2([&] { rc_w = espgpu_wait_frame(dev, espgpu_frame_id(dev) + 1000, 20000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto t0 = std::chrono::steady_clock::now();
    std::thread closer([&] { espgpu_close(dev); });
    t1.join();
    t2.join();
    closer.join();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("close with blocked waiters: %.1f ms (fence rc=%d frame rc=%d)\n", ms, rc_f.load(), rc_w.load());
    EXPECT(rc_f == -1 && rc_w == -1);
    EXPECT(ms < 1000);

    printf(fails ? "e2e FAILED (%d)\n" : "e2e OK\n", fails);
    return fails != 0;
}
