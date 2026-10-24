#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "espgpu_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 纹理。
 *
 * M2 支持 RGB565 与 RGBA4444。统一展开成 RGB565 像素 + 可选 alpha 平面：
 * 采样时颜色只需一次 16 位取，alpha 另取一次 8 位，避免每像素解包。
 */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t *pixels;  /* PSRAM，RGB565 */
    uint8_t *alpha;    /* PSRAM，可空；RGBA4444 时非空 */
} espgpu_texture_t;

/** 工作表面：RGB565，分配在内部 SRAM（光栅器绝不该直写 PSRAM）。 */
typedef struct {
    uint16_t *pixels;
    uint16_t width;
    uint16_t height;
    int16_t scissor_x;
    int16_t scissor_y;
    int16_t scissor_w;
    int16_t scissor_h;
} espgpu_surface_t;

/** 每次绘制用到的状态。 */
typedef struct {
    uint8_t blend;      /* ESPGPU_BLEND_* */
    uint8_t filter;     /* ESPGPU_FILTER_* */
    uint8_t wrap;       /* ESPGPU_WRAP_* */
    uint8_t alpha_test;
    uint8_t alpha_ref;
} espgpu_draw_state_t;

bool raster_surface_init(espgpu_surface_t *s, uint16_t w, uint16_t h);
void raster_surface_deinit(espgpu_surface_t *s);
void raster_set_scissor(espgpu_surface_t *s, int16_t x, int16_t y, int16_t w, int16_t h);
void raster_clear(espgpu_surface_t *s, uint16_t color);

/**
 * 画一个四边形。顶点顺序任意，内部会按两个三角形拆分并自动处理绕序。
 * tex 为 NULL 时按纯色填充（用 v[0].color）。
 */
void raster_draw_quad(espgpu_surface_t *s, const espgpu_texture_t *tex,
                      const espgpu_vertex2d_t v[4], const espgpu_draw_state_t *st);

/** RGB565 两个颜色按 alpha 混合，供内部与测试使用。 */
uint16_t raster_blend_rgb565(uint16_t src, uint16_t dst, uint8_t alpha, uint8_t mode);

#ifdef __cplusplus
}
#endif
