#!/usr/bin/env python3
"""
gen_gl.py — 从 MinGW 的 GL/gl.h 生成 opengl32.dll 的导出层

产物（全部签入仓库，Windows 构建不需要 python）：
  host/gl12/opengl32.def          导出表：GL 1.1 全部入口 + wgl*
  host/gl12/gen/gl_api.h          类型、常量、函数编号、impl_* 声明、直通函数表
  host/gl12/gen/gl_exports.cpp    导出包装：trace / 调用清单统计 / 直通 / 本地实现分发
  host/gl12/gen/gl_stubs.cpp      尚未实现的 impl_*（记录一次 "unimplemented" 后返回 0）

用法：
  python tools/gen_gl.py [--gl-h path/to/GL/gl.h] [--glext-h path/to/GL/glext.h]
默认在 zig 自带的 mingw 头文件里找（pip install ziglang）。

impl_* 是否已实现：扫描 host/gl12/*.cpp 里形如 `impl_glFoo(` 的函数定义。
新实现一个入口后重跑本脚本，对应桩会自动消失。
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GL12 = os.path.join(ROOT, 'host', 'gl12')
GEN = os.path.join(GL12, 'gen')

# ── 扩展入口（经 wglGetProcAddress 取得，不进 .def） ───────────────────
EXT_PROTOS = [
    'void glActiveTexture(GLenum texture)',
    'void glActiveTextureARB(GLenum texture)',
    'void glClientActiveTexture(GLenum texture)',
    'void glClientActiveTextureARB(GLenum texture)',
    'void glDrawRangeElements(GLenum mode,GLuint start,GLuint end,GLsizei count,GLenum type,const GLvoid *indices)',
    'void glDrawRangeElementsEXT(GLenum mode,GLuint start,GLuint end,GLsizei count,GLenum type,const GLvoid *indices)',
    'void glColorTable(GLenum target,GLenum internalformat,GLsizei width,GLenum format,GLenum type,const GLvoid *table)',
    'void glColorTableEXT(GLenum target,GLenum internalformat,GLsizei width,GLenum format,GLenum type,const GLvoid *table)',
    'void glColorSubTable(GLenum target,GLsizei start,GLsizei count,GLenum format,GLenum type,const GLvoid *data)',
    'void glColorSubTableEXT(GLenum target,GLsizei start,GLsizei count,GLenum format,GLenum type,const GLvoid *data)',
    'void glGetColorTable(GLenum target,GLenum format,GLenum type,GLvoid *table)',
    'void glGetColorTableEXT(GLenum target,GLenum format,GLenum type,GLvoid *table)',
    'void glGetColorTableParameteriv(GLenum target,GLenum pname,GLint *params)',
    'void glGetColorTableParameterivEXT(GLenum target,GLenum pname,GLint *params)',
    'void glGetColorTableParameterfv(GLenum target,GLenum pname,GLfloat *params)',
    'void glGetColorTableParameterfvEXT(GLenum target,GLenum pname,GLfloat *params)',
    'void glLockArraysEXT(GLint first,GLsizei count)',
    'void glUnlockArraysEXT(void)',
    'void glBlendEquation(GLenum mode)',
    'void glBlendEquationEXT(GLenum mode)',
    'void glBlendColor(GLclampf red,GLclampf green,GLclampf blue,GLclampf alpha)',
    'void glBlendColorEXT(GLclampf red,GLclampf green,GLclampf blue,GLclampf alpha)',
    'void glPointParameterf(GLenum pname,GLfloat param)',
    'void glPointParameterfARB(GLenum pname,GLfloat param)',
    'void glPointParameterfEXT(GLenum pname,GLfloat param)',
    'void glPointParameterfv(GLenum pname,const GLfloat *params)',
    'void glPointParameterfvARB(GLenum pname,const GLfloat *params)',
    'void glPointParameterfvEXT(GLenum pname,const GLfloat *params)',
    'void glLoadTransposeMatrixf(const GLfloat *m)',
    'void glLoadTransposeMatrixfARB(const GLfloat *m)',
    'void glLoadTransposeMatrixd(const GLdouble *m)',
    'void glLoadTransposeMatrixdARB(const GLdouble *m)',
    'void glMultTransposeMatrixf(const GLfloat *m)',
    'void glMultTransposeMatrixfARB(const GLfloat *m)',
    'void glMultTransposeMatrixd(const GLdouble *m)',
    'void glMultTransposeMatrixdARB(const GLdouble *m)',
    'void glBindTextureEXT(GLenum target,GLuint texture)',
    'void glGenTexturesEXT(GLsizei n,GLuint *textures)',
    'void glDeleteTexturesEXT(GLsizei n,const GLuint *textures)',
    'GLboolean glIsTextureEXT(GLuint texture)',
    'void glPrioritizeTexturesEXT(GLsizei n,const GLuint *textures,const GLclampf *priorities)',
    'GLboolean glAreTexturesResidentEXT(GLsizei n,const GLuint *textures,GLboolean *residences)',
    'void glTexSubImage2DEXT(GLenum target,GLint level,GLint xoffset,GLint yoffset,GLsizei width,GLsizei height,GLenum format,GLenum type,const GLvoid *pixels)',
    'void glCopyTexSubImage2DEXT(GLenum target,GLint level,GLint xoffset,GLint yoffset,GLint x,GLint y,GLsizei width,GLsizei height)',
]

# glMultiTexCoord{1..4}{d,f,i,s}[v][ARB]
_MT_TYPES = {'d': 'GLdouble', 'f': 'GLfloat', 'i': 'GLint', 's': 'GLshort'}
for _n in range(1, 5):
    for _t, _ct in _MT_TYPES.items():
        for _suf in ('', 'ARB'):
            comps = 'stqr'[:_n] if False else ['s', 't', 'r', 'q'][:_n]
            EXT_PROTOS.append('void glMultiTexCoord%d%s%s(GLenum target,%s)' %
                              (_n, _t, _suf, ','.join('%s %s' % (_ct, c) for c in comps)))
            EXT_PROTOS.append('void glMultiTexCoord%d%sv%s(GLenum target,const %s *v)' %
                              (_n, _t, _suf, _ct))

WGL_EXPORTS = [
    'wglChoosePixelFormat', 'wglCopyContext', 'wglCreateContext', 'wglCreateLayerContext',
    'wglDeleteContext', 'wglDescribeLayerPlane', 'wglDescribePixelFormat',
    'wglGetCurrentContext', 'wglGetCurrentDC', 'wglGetDefaultProcAddress',
    'wglGetLayerPaletteEntries', 'wglGetPixelFormat', 'wglGetProcAddress', 'wglMakeCurrent',
    'wglRealizeLayerPalette', 'wglSetLayerPaletteEntries', 'wglSetPixelFormat', 'wglShareLists',
    'wglSwapBuffers', 'wglSwapLayerBuffers', 'wglSwapMultipleBuffers', 'wglUseFontBitmapsA',
    'wglUseFontBitmapsW', 'wglUseFontOutlinesA', 'wglUseFontOutlinesW',
]

SUFFIXES = ('ARB', 'EXT')


def find_headers():
    try:
        out = subprocess.check_output([sys.executable, '-m', 'ziglang', 'env'], text=True)
        m = re.search(r'\.lib_dir = "([^"]+)"', out)
        if m:
            d = os.path.join(m.group(1), 'libc', 'include', 'any-windows-any', 'GL')
            return os.path.join(d, 'gl.h'), os.path.join(d, 'glext.h')
    except Exception:
        pass
    return None, None


class Fn:
    def __init__(self, ret, name, params, ext):
        self.ret = ret.strip()
        self.name = name
        self.params = params  # [(type, name)]
        self.ext = ext
        self.impl = name


def parse_params(s):
    s = s.strip()
    if s in ('', 'void'):
        return []
    out = []
    for i, p in enumerate(s.split(',')):
        p = p.strip()
        m = re.match(r'^(.*?)([A-Za-z_][A-Za-z0-9_]*)$', p)
        t, n = m.group(1).strip(), m.group(2)
        if not t:
            t, n = p, 'a%d' % i
        out.append((t.replace(' *', '*').replace('* ', '*').strip(), n))
    return out


def parse_gl_h(path):
    fns, consts = [], []
    rx = re.compile(r'^WINGDIAPI\s+(.+?)\s*APIENTRY\s+(gl\w+)\s*\((.*)\);')
    for line in open(path, encoding='utf-8', errors='replace'):
        m = rx.match(line.strip())
        if m:
            fns.append(Fn(m.group(1), m.group(2), parse_params(m.group(3)), False))
            continue
        m = re.match(r'^#define\s+(GL_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\s*$', line.strip())
        if m:
            consts.append((m.group(1), m.group(2)))
    return fns, consts


def parse_consts(path):
    out = []
    if not path or not os.path.exists(path):
        return out
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'^#define\s+(GL_\w+)\s+(0x[0-9A-Fa-f]+|\d+)u?\s*$', line.strip())
        if m:
            out.append((m.group(1), m.group(2)))
    return out


def parse_ext(proto):
    m = re.match(r'^(.+?)\s*\b(gl\w+)\s*\((.*)\)$', proto)
    return Fn(m.group(1), m.group(2), parse_params(m.group(3)), True)


def is_ptr(t):
    return '*' in t


def ret_default(ret):
    if ret == 'void':
        return ''
    if is_ptr(ret):
        return 'nullptr'
    return '(%s)0' % ret


FMT = {
    'GLenum': ('0x%04X', '(unsigned)%s'),
    'GLbitfield': ('0x%X', '(unsigned)%s'),
    'GLboolean': ('%u', '(unsigned)%s'),
    'GLubyte': ('%u', '(unsigned)%s'),
    'GLushort': ('%u', '(unsigned)%s'),
    'GLuint': ('%u', '(unsigned)%s'),
    'GLbyte': ('%d', '(int)%s'),
    'GLshort': ('%d', '(int)%s'),
    'GLint': ('%d', '(int)%s'),
    'GLsizei': ('%d', '(int)%s'),
    'GLfloat': ('%g', '(double)%s'),
    'GLclampf': ('%g', '(double)%s'),
    'GLdouble': ('%g', '(double)%s'),
    'GLclampd': ('%g', '(double)%s'),
}


def fmt_param(t, n):
    if is_ptr(t):
        return '%p', '(const void *)%s' % n
    f = FMT.get(t)
    if f is None:
        return '%p', '(const void *)(uintptr_t)%s' % n
    return f[0], f[1] % n


def decl_params(f):
    return ', '.join('%s %s' % (t, n) for t, n in f.params) or 'void'


def call_args(f):
    return ', '.join(n for _, n in f.params)


def scan_implemented():
    impl = set()
    for fn in os.listdir(GL12):
        if not fn.endswith('.cpp'):
            continue
        txt = open(os.path.join(GL12, fn), encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'^\s*[A-Za-z_][\w\s\*]*?\bimpl_(gl\w+)\s*\(', txt, re.M):
            impl.add(m.group(1))
        # 宏批量定义：V2(glVertex2d, GLdouble) 之类同时定义 fn 与 fn##v
        for m in re.finditer(r'^([A-Z][A-Z0-9]*)\((gl\w+),', txt, re.M):
            impl.add(m.group(2))
            impl.add(m.group(2) + 'v')
    return impl


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gl-h')
    ap.add_argument('--glext-h')
    a = ap.parse_args()
    gl_h, glext_h = a.gl_h, a.glext_h
    if not gl_h:
        gl_h, d_ext = find_headers()
        glext_h = glext_h or d_ext
    if not gl_h or not os.path.exists(gl_h):
        sys.exit('GL/gl.h not found; pass --gl-h')

    core, consts = parse_gl_h(gl_h)
    consts += parse_consts(glext_h)
    exts = [parse_ext(p) for p in EXT_PROTOS]
    allf = core + exts
    names = {f.name for f in allf}

    # 后缀别名：glFooARB → impl_glFoo（若基名存在）
    for f in allf:
        for s in SUFFIXES:
            if f.name.endswith(s) and f.name[:-len(s)] in names:
                f.impl = f.name[:-len(s)]
    impl_names = sorted({f.impl for f in allf})
    by_impl = {}
    for f in allf:
        by_impl.setdefault(f.impl, f)
        if f.impl == f.name:
            by_impl[f.impl] = f

    os.makedirs(GEN, exist_ok=True)
    implemented = scan_implemented()

    seen = set()
    cl = []
    for n, v in consts:
        if n in seen:
            continue
        seen.add(n)
        cl.append('#define %s %s' % (n, v))

    # ── gl_api.h ──
    h = []
    h.append('/* 由 tools/gen_gl.py 生成，勿手改 */')
    h.append('#pragma once')
    h.append('#include <stddef.h>\n#include <stdint.h>\n')
    h.append('#ifndef APIENTRY\n#if defined(_WIN32) && !defined(_WIN64)\n#define APIENTRY __stdcall\n#else\n#define APIENTRY\n#endif\n#endif\n')
    h.append('''typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef void GLvoid;
''')
    h.append('#ifndef ESPGL_NO_CONSTANTS')
    h.extend(cl)
    h.append('#endif\n')
    h.append('namespace espgl {\n')
    h.append('enum FnId {')
    for f in allf:
        h.append('    FN_%s,' % f.name)
    h.append('    FN_COUNT\n};\n')
    h.append('extern const char *const kFnNames[FN_COUNT];')
    h.append('extern const bool kFnIsExt[FN_COUNT];\n')
    h.append('/* 本地实现（或 gl_stubs.cpp 里的桩） */')
    for n in impl_names:
        f = by_impl[n]
        h.append('%s impl_%s(%s);' % (f.ret, n, decl_params(f)))
    h.append('\n/* 直通：真实 GL 的函数指针 */')
    h.append('struct PtTable {')
    for f in allf:
        h.append('    %s(APIENTRY *%s)(%s);' % (f.ret, f.name, decl_params(f)))
    h.append('};')
    h.append('extern PtTable g_pt;\n')
    h.append('struct EnumName {\n    uint32_t value;\n    const char *name;\n};')
    h.append('/** 按 value 升序，trace 输出用来把枚举值翻译成名字 */')
    h.append('extern const EnumName kEnumNames[];')
    h.append('extern const size_t kEnumNameCount;\n')
    h.append('struct ProcEntry {\n    const char *name;\n    void *proc;\n};')
    h.append('/** 名字 → 导出包装，供 wglGetProcAddress（含核心与扩展入口），以 nullptr 结尾 */')
    h.append('extern const ProcEntry kProcTable[];\n')
    h.append('/** 从真实 opengl32 / Mesa 里解析核心入口（扩展入口在首次调用时经其 wglGetProcAddress 懒解析） */')
    h.append('void pt_load_core(void *module);\n')
    h.append('} // namespace espgl')
    open(os.path.join(GEN, 'gl_api.h'), 'w', encoding='utf-8', newline='\n').write('\n'.join(h) + '\n')

    # ── gl_exports.cpp ──
    c = []
    c.append('/* 由 tools/gen_gl.py 生成，勿手改 */')
    c.append('#include "../gl_dispatch.h"\n')
    c.append('namespace espgl {\n')
    c.append('const char *const kFnNames[FN_COUNT] = {')
    for f in allf:
        c.append('    "%s",' % f.name)
    c.append('};\n')
    c.append('const bool kFnIsExt[FN_COUNT] = {')
    for f in allf:
        c.append('    %s,' % ('true' if f.ext else 'false'))
    c.append('};\n')
    c.append('PtTable g_pt;\n')
    c.append('void pt_load_core(void *module)\n{')
    for f in core:
        c.append('    g_pt.%s = reinterpret_cast<decltype(g_pt.%s)>(pt_sym(module, "%s"));' % (f.name, f.name, f.name))
    c.append('}\n')
    c.append('} // namespace espgl\n')
    c.append('using namespace espgl;\n')
    c.append('extern "C" {\n')
    for f in allf:
        fmts, args, enums = [], [], []
        for i, (t, n) in enumerate(f.params):
            fs, ax = fmt_param(t, n)
            fmts.append(fs)
            args.append(ax)
            if t == 'GLenum':
                enums.append((i, n))
        c.append('%s APIENTRY espgl_%s(%s)\n{' % (f.ret, f.name, decl_params(f)))
        c.append('    if (g_watch) {')
        c.append('        trace_count(FN_%s);' % f.name)
        for i, n in enums:
            c.append('        trace_enum(FN_%s, %d, %s);' % (f.name, i, n))
        c.append('        if (g_trace_log) {')
        if f.params:
            c.append('            trace_call(FN_%s, "%s(%s)", %s);' % (f.name, f.name, ', '.join(fmts), ', '.join(args)))
        else:
            c.append('            trace_call(FN_%s, "%s()");' % (f.name, f.name))
        c.append('        }')
        c.append('    }')
        c.append('    if (g_mode == MODE_PASSTHROUGH) {')
        if f.ext:
            c.append('        if (!g_pt.%s) {' % f.name)
            c.append('            g_pt.%s = reinterpret_cast<decltype(g_pt.%s)>(pt_ext("%s", "%s"));' % (f.name, f.name, f.name, f.impl))
            c.append('        }')
        c.append('        if (g_pt.%s) {' % f.name)
        if f.ret == 'void':
            c.append('            g_pt.%s(%s);\n            return;' % (f.name, call_args(f)))
        else:
            c.append('            return g_pt.%s(%s);' % (f.name, call_args(f)))
        c.append('        }')
        c.append('        return %s;' % ret_default(f.ret) if f.ret != 'void' else '        return;')
        c.append('    }')
        if f.ret == 'void':
            c.append('    impl_%s(%s);' % (f.impl, call_args(f)))
        else:
            c.append('    return impl_%s(%s);' % (f.impl, call_args(f)))
        c.append('}\n')
    c.append('} // extern "C"\n')
    c.append('namespace espgl {\n')
    ev = {}
    for n, v in consts:
        iv = int(v, 0)
        if iv < 0x100 or iv > 0xFFFFF:
            continue  # 0/1 与位掩码太多同值名，没有翻译价值
        if iv not in ev or (ev[iv].endswith(('_ARB', '_EXT', '_SGIS')) and not n.endswith(('_ARB', '_EXT', '_SGIS'))):
            ev[iv] = n
    c.append('const EnumName kEnumNames[] = {')
    for iv in sorted(ev):
        c.append('    {0x%X, "%s"},' % (iv, ev[iv]))
    c.append('};')
    c.append('const size_t kEnumNameCount = sizeof(kEnumNames) / sizeof(kEnumNames[0]);\n')
    c.append('const ProcEntry kProcTable[] = {')
    for f in allf:
        c.append('    {"%s", reinterpret_cast<void *>(&espgl_%s)},' % (f.name, f.name))
    c.append('    {nullptr, nullptr},\n};\n')
    c.append('} // namespace espgl')
    open(os.path.join(GEN, 'gl_exports.cpp'), 'w', encoding='utf-8', newline='\n').write('\n'.join(c) + '\n')

    # ── gl_stubs.cpp ──
    s = ['/* 由 tools/gen_gl.py 生成，勿手改。列出的是 espgpu 模式下尚未实现的入口。 */',
         '#include "../gl_dispatch.h"\n', 'namespace espgl {\n']
    missing = []
    for n in impl_names:
        if n in implemented:
            continue
        f = by_impl[n]
        missing.append(n)
        unused = ' '.join('(void)%s;' % pn for _, pn in f.params)
        s.append('%s impl_%s(%s)\n{\n    %s\n    gl_unimplemented(FN_%s);' % (f.ret, n, decl_params(f), unused or '', f.name))
        if f.ret != 'void':
            s.append('    return %s;' % ret_default(f.ret))
        s.append('}\n')
    s.append('} // namespace espgl')
    open(os.path.join(GEN, 'gl_stubs.cpp'), 'w', encoding='utf-8', newline='\n').write('\n'.join(s) + '\n')

    # ── opengl32.def ──
    d = ['; 由 tools/gen_gl.py 生成，勿手改', 'LIBRARY opengl32', 'EXPORTS']
    for f in core:
        d.append('    %s=espgl_%s' % (f.name, f.name))
    for w in WGL_EXPORTS:
        d.append('    %s=e%s' % (w, w))
    open(os.path.join(GL12, 'opengl32.def'), 'w', encoding='utf-8', newline='\n').write('\n'.join(d) + '\n')

    # ── 覆盖清单（供 docs/gl-coverage.md） ──
    cov = ['# 由 tools/gen_gl.py 生成：espgpu 模式下的入口实现状态', '']
    for f in allf:
        impl = f.impl in implemented
        cov.append('%s\t%s\t%s' % (f.name, 'ext' if f.ext else 'core', 'yes' if impl else 'stub'))
    open(os.path.join(GEN, 'coverage.tsv'), 'w', encoding='utf-8', newline='\n').write('\n'.join(cov) + '\n')

    print('core %d, ext %d, impl %d, stubs %d' % (len(core), len(exts), len(impl_names) - len(missing), len(missing)))


if __name__ == '__main__':
    main()
