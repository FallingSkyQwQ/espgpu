/* 由 tools/gen_gl.py 生成，勿手改。列出的是 espgpu 模式下尚未实现的入口。 */
#include "../gl_dispatch.h"

namespace espgl {

void impl_glEvalCoord1d(GLdouble u)
{
    (void)u;
    gl_unimplemented(FN_glEvalCoord1d);
}

void impl_glEvalCoord1dv(const GLdouble* u)
{
    (void)u;
    gl_unimplemented(FN_glEvalCoord1dv);
}

void impl_glEvalCoord1f(GLfloat u)
{
    (void)u;
    gl_unimplemented(FN_glEvalCoord1f);
}

void impl_glEvalCoord1fv(const GLfloat* u)
{
    (void)u;
    gl_unimplemented(FN_glEvalCoord1fv);
}

void impl_glEvalCoord2d(GLdouble u, GLdouble v)
{
    (void)u; (void)v;
    gl_unimplemented(FN_glEvalCoord2d);
}

void impl_glEvalCoord2dv(const GLdouble* u)
{
    (void)u;
    gl_unimplemented(FN_glEvalCoord2dv);
}

void impl_glEvalCoord2f(GLfloat u, GLfloat v)
{
    (void)u; (void)v;
    gl_unimplemented(FN_glEvalCoord2f);
}

void impl_glEvalCoord2fv(const GLfloat* u)
{
    (void)u;
    gl_unimplemented(FN_glEvalCoord2fv);
}

void impl_glEvalMesh1(GLenum mode, GLint i1, GLint i2)
{
    (void)mode; (void)i1; (void)i2;
    gl_unimplemented(FN_glEvalMesh1);
}

void impl_glEvalMesh2(GLenum mode, GLint i1, GLint i2, GLint j1, GLint j2)
{
    (void)mode; (void)i1; (void)i2; (void)j1; (void)j2;
    gl_unimplemented(FN_glEvalMesh2);
}

void impl_glEvalPoint1(GLint i)
{
    (void)i;
    gl_unimplemented(FN_glEvalPoint1);
}

void impl_glEvalPoint2(GLint i, GLint j)
{
    (void)i; (void)j;
    gl_unimplemented(FN_glEvalPoint2);
}

void impl_glGetMapdv(GLenum target, GLenum query, GLdouble* v)
{
    (void)target; (void)query; (void)v;
    gl_unimplemented(FN_glGetMapdv);
}

void impl_glGetMapfv(GLenum target, GLenum query, GLfloat* v)
{
    (void)target; (void)query; (void)v;
    gl_unimplemented(FN_glGetMapfv);
}

void impl_glGetMapiv(GLenum target, GLenum query, GLint* v)
{
    (void)target; (void)query; (void)v;
    gl_unimplemented(FN_glGetMapiv);
}

void impl_glGetPixelMapfv(GLenum map, GLfloat* values)
{
    (void)map; (void)values;
    gl_unimplemented(FN_glGetPixelMapfv);
}

void impl_glGetPixelMapuiv(GLenum map, GLuint* values)
{
    (void)map; (void)values;
    gl_unimplemented(FN_glGetPixelMapuiv);
}

void impl_glGetPixelMapusv(GLenum map, GLushort* values)
{
    (void)map; (void)values;
    gl_unimplemented(FN_glGetPixelMapusv);
}

void impl_glGetTexGendv(GLenum coord, GLenum pname, GLdouble* params)
{
    (void)coord; (void)pname; (void)params;
    gl_unimplemented(FN_glGetTexGendv);
}

void impl_glGetTexGenfv(GLenum coord, GLenum pname, GLfloat* params)
{
    (void)coord; (void)pname; (void)params;
    gl_unimplemented(FN_glGetTexGenfv);
}

void impl_glGetTexGeniv(GLenum coord, GLenum pname, GLint* params)
{
    (void)coord; (void)pname; (void)params;
    gl_unimplemented(FN_glGetTexGeniv);
}

void impl_glMap1d(GLenum target, GLdouble u1, GLdouble u2, GLint stride, GLint order, const GLdouble* points)
{
    (void)target; (void)u1; (void)u2; (void)stride; (void)order; (void)points;
    gl_unimplemented(FN_glMap1d);
}

void impl_glMap1f(GLenum target, GLfloat u1, GLfloat u2, GLint stride, GLint order, const GLfloat* points)
{
    (void)target; (void)u1; (void)u2; (void)stride; (void)order; (void)points;
    gl_unimplemented(FN_glMap1f);
}

void impl_glMap2d(GLenum target, GLdouble u1, GLdouble u2, GLint ustride, GLint uorder, GLdouble v1, GLdouble v2, GLint vstride, GLint vorder, const GLdouble* points)
{
    (void)target; (void)u1; (void)u2; (void)ustride; (void)uorder; (void)v1; (void)v2; (void)vstride; (void)vorder; (void)points;
    gl_unimplemented(FN_glMap2d);
}

void impl_glMap2f(GLenum target, GLfloat u1, GLfloat u2, GLint ustride, GLint uorder, GLfloat v1, GLfloat v2, GLint vstride, GLint vorder, const GLfloat* points)
{
    (void)target; (void)u1; (void)u2; (void)ustride; (void)uorder; (void)v1; (void)v2; (void)vstride; (void)vorder; (void)points;
    gl_unimplemented(FN_glMap2f);
}

void impl_glMapGrid1d(GLint un, GLdouble u1, GLdouble u2)
{
    (void)un; (void)u1; (void)u2;
    gl_unimplemented(FN_glMapGrid1d);
}

void impl_glMapGrid1f(GLint un, GLfloat u1, GLfloat u2)
{
    (void)un; (void)u1; (void)u2;
    gl_unimplemented(FN_glMapGrid1f);
}

void impl_glMapGrid2d(GLint un, GLdouble u1, GLdouble u2, GLint vn, GLdouble v1, GLdouble v2)
{
    (void)un; (void)u1; (void)u2; (void)vn; (void)v1; (void)v2;
    gl_unimplemented(FN_glMapGrid2d);
}

void impl_glMapGrid2f(GLint un, GLfloat u1, GLfloat u2, GLint vn, GLfloat v1, GLfloat v2)
{
    (void)un; (void)u1; (void)u2; (void)vn; (void)v1; (void)v2;
    gl_unimplemented(FN_glMapGrid2f);
}

void impl_glPixelMapfv(GLenum map, GLsizei mapsize, const GLfloat* values)
{
    (void)map; (void)mapsize; (void)values;
    gl_unimplemented(FN_glPixelMapfv);
}

void impl_glPixelMapuiv(GLenum map, GLsizei mapsize, const GLuint* values)
{
    (void)map; (void)mapsize; (void)values;
    gl_unimplemented(FN_glPixelMapuiv);
}

void impl_glPixelMapusv(GLenum map, GLsizei mapsize, const GLushort* values)
{
    (void)map; (void)mapsize; (void)values;
    gl_unimplemented(FN_glPixelMapusv);
}

} // namespace espgl
