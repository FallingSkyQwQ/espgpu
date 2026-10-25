/*
 * raster2d.c — M2 的 2D 顶点路径
 *
 * espgpu_vertex2d_t（1/16 像素屏幕坐标 + 1/16 texel 纹理坐标 + 每四边形一个顶点色）
 * 直接变成窗口空间三角形，走和 3D 相同的分箱 / tile 光栅，状态取 PIPELINE2D。
 * 行为与 M2 的立即模式光栅器保持一致：像素中心采样、v[0].color 调制（0xFFFF = 不调制）。
 */

#include "core_internal.h"

void gp_draw2d(gpu_core_t *c, const espgpu_draw_t *d, const uint8_t *verts, uint32_t len)
{
    (void)d;
    if (d->prim != ESPGPU_PRIM_QUADS && d->prim != ESPGPU_PRIM_TRIANGLES) {
        return;
    }
    const uint32_t stride = (uint32_t)sizeof(espgpu_vertex2d_t);
    const uint32_t count = len / stride;
    const uint32_t per = d->prim == ESPGPU_PRIM_QUADS ? 4u : 3u;

    const gp_tex_t *tex = c->bind_id[0] ? gp_tex_find(&c->tc, c->bind_id[0]) : NULL;
    const bool textured = tex && (tex->px || tex->idx);

    gp_olayout_t ol;
    memset(&ol, 0, sizeof(ol));
    ol.off_color = ol.off_t0 = ol.off_t1 = ol.off_fog = -1;
    ol.flags = GP_TF_2D | GP_TF_CONST_COLOR;
    if (textured) {
        ol.flags |= GP_TF_TEX0;
        ol.off_t0 = 0;
        ol.n = 2;
    }

    uint8_t saved[4];
    memcpy(saved, c->const_color, 4);

    for (uint32_t i = 0; i + per <= count; i += per) {
        espgpu_vertex2d_t v[4];
        memcpy(v, verts + (size_t)i * stride, per * stride);

        const uint32_t rgb = gp_expand565(v[0].color);
        c->const_color[0] = (uint8_t)(rgb >> 16);
        c->const_color[1] = (uint8_t)(rgb >> 8);
        c->const_color[2] = (uint8_t)rgb;
        c->const_color[3] = 255;

        gp_wvtx_t w[4];
        for (uint32_t k = 0; k < per; k++) {
            /* gp_setup_tri 只读 x/y/z/iw 和前 ol.n（≤ 2）个属性：不必清整个 128 字节 */
            w[k].x = (float)v[k].x * (1.0f / 16.0f); /* 2 的幂：与 / 16 逐位相同 */
            w[k].y = (float)v[k].y * (1.0f / 16.0f);
            w[k].z = 0.0f;
            w[k].iw = 1.0f;
            w[k].a[0] = (float)v[k].u * (1.0f / 16.0f);
            w[k].a[1] = (float)v[k].v * (1.0f / 16.0f);
        }
        for (int attempt = 0; attempt < 2; attempt++) {
            if (gp_setup_tri(c, &w[0], &w[1], &w[2], &ol, NULL, false)) {
                break;
            }
            gp_flush(c);
        }
        if (per == 4) {
            for (int attempt = 0; attempt < 2; attempt++) {
                if (gp_setup_tri(c, &w[0], &w[2], &w[3], &ol, NULL, false)) {
                    break;
                }
                gp_flush(c);
            }
        }
        c->st_prims++;
        c->st_verts += per;
    }
    memcpy(c->const_color, saved, 4);
}
