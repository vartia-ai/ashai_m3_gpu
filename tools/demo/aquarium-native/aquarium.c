/* aquarium-native: the WebGL Aquarium (WebGLSamples, BSD-3-Clause) as a
 * native OpenGL 2.1 / GLSL 1.20 program, so macOS and Linux can run the
 * same scene with no browser. The scene, camera path, fish motion, shaders
 * and draw order follow aquarium.js; convert.py builds the asset pack.
 * raylib provides the window, context, timing and overlay text. Bubbles and
 * lasers are left out. The aquarium is Copyright 2009 Google Inc. under the
 * BSD 3-Clause license; see NOTICE.
 *
 *   aquarium PACKDIR [--fish N | --tour N,N,...] [--secs S] [--size WxH]
 *            [--view N] [--window X,Y,W,H] [--label TEXT] [--quit-after S]
 *
 * V cycles the page's camera presets.
 *
 * The scene renders into a fixed-size offscreen target (default 1600x1000)
 * with vsync off; the window shows it scaled. A CSV line per second goes to
 * stdout: t,fish,fps,frame_ms,draws,triangles.
 */
#include <math.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#include "raylib.h"

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <dlfcn.h>
#include <sys/sysctl.h>
static void *gl_proc(const char *n) { return dlsym(RTLD_DEFAULT, n); }
#else
#include <GL/gl.h>
#include <SDL.h>
static void *gl_proc(const char *n) { return SDL_GL_GetProcAddress(n); }
#endif

/* ---- GL 2.0+ entry points ---------------------------------------------- */

#define GL_ARRAY_BUFFER_ 0x8892
#define GL_ELEMENT_ARRAY_BUFFER_ 0x8893
#define GL_STATIC_DRAW_ 0x88E4
#define GL_FRAGMENT_SHADER_ 0x8B30
#define GL_VERTEX_SHADER_ 0x8B31
#define GL_COMPILE_STATUS_ 0x8B81
#define GL_LINK_STATUS_ 0x8B82
#define GL_TEXTURE0_ 0x84C0
#define GL_TEXTURE_CUBE_MAP_ 0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X_ 0x8515
#define GL_CLAMP_TO_EDGE_ 0x812F
#define GL_TEXTURE_MAX_LEVEL_ 0x813D
#define GL_FRAMEBUFFER_ 0x8D40
#define GL_RENDERBUFFER_ 0x8D41
#define GL_COLOR_ATTACHMENT0_ 0x8CE0
#define GL_DEPTH_ATTACHMENT_ 0x8D00
#define GL_FRAMEBUFFER_COMPLETE_ 0x8CD5
#define GL_DEPTH_COMPONENT24_ 0x81A6
#define GL_FUNC_ADD_ 0x8006

#define GLFUNCS(X)                                                                 \
   X(GLuint, CreateShader, (GLenum))                                              \
   X(void, ShaderSource, (GLuint, GLsizei, const char *const *, const GLint *))   \
   X(void, CompileShader, (GLuint))                                               \
   X(void, GetShaderiv, (GLuint, GLenum, GLint *))                                \
   X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, char *))                \
   X(GLuint, CreateProgram, (void))                                               \
   X(void, AttachShader, (GLuint, GLuint))                                        \
   X(void, BindAttribLocation, (GLuint, GLuint, const char *))                    \
   X(void, LinkProgram, (GLuint))                                                 \
   X(void, GetProgramiv, (GLuint, GLenum, GLint *))                               \
   X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *))               \
   X(void, UseProgram, (GLuint))                                                  \
   X(GLint, GetUniformLocation, (GLuint, const char *))                           \
   X(void, Uniform1i, (GLint, GLint))                                             \
   X(void, Uniform1f, (GLint, GLfloat))                                           \
   X(void, Uniform3fv, (GLint, GLsizei, const GLfloat *))                         \
   X(void, Uniform4fv, (GLint, GLsizei, const GLfloat *))                         \
   X(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *))        \
   X(void, GenBuffers, (GLsizei, GLuint *))                                       \
   X(void, BindBuffer, (GLenum, GLuint))                                          \
   X(void, BufferData, (GLenum, ptrdiff_t, const void *, GLenum))                 \
   X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
   X(void, EnableVertexAttribArray, (GLuint))                                     \
   X(void, DisableVertexAttribArray, (GLuint))                                    \
   X(void, ActiveTexture, (GLenum))                                               \
   X(void, BlendEquation, (GLenum))                                               \
   X(void, GenFramebuffers, (GLsizei, GLuint *))                                  \
   X(void, BindFramebuffer, (GLenum, GLuint))                                     \
   X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))         \
   X(void, GenRenderbuffers, (GLsizei, GLuint *))                                 \
   X(void, BindRenderbuffer, (GLenum, GLuint))                                    \
   X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))               \
   X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))             \
   X(GLenum, CheckFramebufferStatus, (GLenum))

#define DECL(r, n, a) static r(*p##n) a;
GLFUNCS(DECL)
static void (*pBindVertexArray)(GLuint);

static void die(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fprintf(stderr, "aquarium: ");
   vfprintf(stderr, fmt, ap);
   fprintf(stderr, "\n");
   va_end(ap);
   exit(1);
}

static void *load_proc(const char *name)
{
   char ext[96];
   void *p = gl_proc(name);
   if (!p) {
      snprintf(ext, sizeof ext, "%sEXT", name);
      p = gl_proc(ext);
   }
   return p;
}

static void load_gl(void)
{
#define LOAD(r, n, a)                                    \
   if (!(p##n = (r(*) a)load_proc("gl" #n)))             \
      die("missing GL entry point gl%s", #n);
   GLFUNCS(LOAD)
   pBindVertexArray = (void (*)(GLuint))load_proc("glBindVertexArray");
   if (!pBindVertexArray)
      pBindVertexArray = (void (*)(GLuint))load_proc("glBindVertexArrayAPPLE");
}

/* ---- math (column-major, as GL takes it) ------------------------------- */

typedef struct { float m[16]; } M4;

static M4 m4_mul(M4 a, M4 b)
{
   M4 r;
   for (int c = 0; c < 4; c++)
      for (int i = 0; i < 4; i++) {
         float s = 0;
         for (int k = 0; k < 4; k++)
            s += a.m[k * 4 + i] * b.m[c * 4 + k];
         r.m[c * 4 + i] = s;
      }
   return r;
}

static M4 m4_identity(void)
{
   M4 r = {{0}};
   r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
   return r;
}

static M4 m4_transpose(M4 a)
{
   M4 r;
   for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++)
         r.m[i * 4 + j] = a.m[j * 4 + i];
   return r;
}

static M4 m4_inverse(M4 a)
{
   const float *m = a.m;
   float inv[16];
   inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
   inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
   inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
   inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
   inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
   inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
   inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
   inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
   inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
   inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
   inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
   inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
   inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
   inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
   inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
   inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
   float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
   M4 r;
   for (int i = 0; i < 16; i++)
      r.m[i] = det != 0 ? inv[i] / det : 0;
   return r;
}

static void v3_norm(float *v)
{
   float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
   if (l > 0)
      v[0] /= l, v[1] /= l, v[2] /= l;
}

static void v3_cross(const float *a, const float *b, float *r)
{
   r[0] = a[1] * b[2] - a[2] * b[1];
   r[1] = a[2] * b[0] - a[0] * b[2];
   r[2] = a[0] * b[1] - a[1] * b[0];
}

/* tdl's cameraLookAt: the camera's world matrix (view inverse). */
static M4 camera_look_at(const float *eye, const float *target)
{
   float up[3] = {0, 1, 0}, z[3] = {eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};
   float x[3], y[3];
   v3_norm(z);
   v3_cross(up, z, x);
   v3_norm(x);
   v3_cross(z, x, y);
   M4 r = m4_identity();
   memcpy(&r.m[0], x, 12);
   memcpy(&r.m[4], y, 12);
   memcpy(&r.m[8], z, 12);
   memcpy(&r.m[12], eye, 12);
   return r;
}

static M4 frustum(float l, float r, float b, float t, float n, float f)
{
   M4 m = {{0}};
   m.m[0] = 2 * n / (r - l);
   m.m[5] = 2 * n / (t - b);
   m.m[8] = (r + l) / (r - l);
   m.m[9] = (t + b) / (t - b);
   m.m[10] = -(f + n) / (f - n);
   m.m[11] = -1;
   m.m[14] = -2 * f * n / (f - n);
   return m;
}

static M4 translation(float x, float y, float z)
{
   M4 r = m4_identity();
   r.m[12] = x, r.m[13] = y, r.m[14] = z;
   return r;
}

static M4 scaling(float x, float y, float z)
{
   M4 r = m4_identity();
   r.m[0] = x, r.m[5] = y, r.m[10] = z;
   return r;
}

static M4 rotation_z(float a)
{
   M4 r = m4_identity();
   r.m[0] = cosf(a), r.m[1] = sinf(a), r.m[4] = -sinf(a), r.m[5] = cosf(a);
   return r;
}

/* tdl.math.pseudoRandom, in doubles as JavaScript computes it. */
static double rand_seed;
static double pseudo_random(void)
{
   rand_seed = fmod(134775813.0 * rand_seed + 1.0, 4294967296.0);
   return rand_seed / 4294967296.0;
}

/* ---- the pack ----------------------------------------------------------- */

enum {
   U_world, U_worldInverseTranspose, U_viewProjection, U_viewInverse,
   U_lightWorldPos, U_lightColor, U_specular, U_shininess, U_specularFactor,
   U_ambient, U_fogPower, U_fogMult, U_fogOffset, U_fogColor, U_time,
   U_worldPosition, U_nextPosition, U_scale, U_fishLength, U_fishWaveLength,
   U_fishBendAmount, U_refractionFudge, U_eta, U_tankColorFudge, U_colorMult,
   NUM_U
};
static const char *const uniform_names[NUM_U] = {
   "world", "worldInverseTranspose", "viewProjection", "viewInverse",
   "lightWorldPos", "lightColor", "specular", "shininess", "specularFactor",
   "ambient", "fogPower", "fogMult", "fogOffset", "fogColor", "time",
   "worldPosition", "nextPosition", "scale", "fishLength", "fishWaveLength",
   "fishBendAmount", "refractionFudge", "eta", "tankColorFudge", "colorMult",
};

typedef struct { char name[16]; GLuint id; GLint u[NUM_U]; } Prog;
typedef struct { char name[64]; GLuint id; } Tex;
typedef struct {
   char scene[32], group[16];
   int prog, fog, blend;
   GLuint vbo, ibo;
   int nidx;
   GLuint tex[3]; /* diffuse, normalMap, reflectionMap */
} AqModel;
typedef struct { char scene[32]; M4 world; } Place;
typedef struct {
   char name[32];
   float speed, speedRange, radius, radiusRange, tailSpeed, heightOffset, heightRange;
   float fishLength, fishWaveLength, fishBendAmount;
   int model;
} Fish;

static Prog progs[16];
static int nprogs;
static Tex texs[96];
static int ntexs;
static AqModel models[64];
static int nmodels;
static Place places[128];
static int nplaces;
static Fish fishes[8];
static int nfish;
typedef struct {
   float eyeHeight, eyeRadius, eyeSpeed, targetHeight, targetRadius, fov;
   float ambient[3], fogPower, fogMult, fogOffset, fog[3];
} View;
static View views[8];
static int nviews;
static GLuint skybox;
static int lightray_prog = -1;
static GLuint lightray_tex, lightray_vbo, lightray_ibo;

static char *read_file(const char *dir, const char *rel, size_t *len)
{
   char path[1024];
   snprintf(path, sizeof path, "%s/%s", dir, rel);
   FILE *f = fopen(path, "rb");
   if (!f)
      die("cannot open %s", path);
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *buf = malloc(n + 1);
   if (fread(buf, 1, n, f) != (size_t)n)
      die("short read %s", path);
   buf[n] = 0;
   fclose(f);
   if (len)
      *len = n;
   return buf;
}

static GLuint compile(GLenum type, const char *src, const char *what)
{
   GLuint s = pCreateShader(type);
   GLint ok;
   pShaderSource(s, 1, &src, NULL);
   pCompileShader(s);
   pGetShaderiv(s, GL_COMPILE_STATUS_, &ok);
   if (!ok) {
      char log[4096];
      pGetShaderInfoLog(s, sizeof log, NULL, log);
      die("%s: %s", what, log);
   }
   return s;
}

static void load_prog(const char *dir, const char *name, const char *vs, const char *fs)
{
   Prog *p = &progs[nprogs++];
   char *vsrc = read_file(dir, vs, NULL), *fsrc = read_file(dir, fs, NULL);
   snprintf(p->name, sizeof p->name, "%s", name);
   p->id = pCreateProgram();
   pAttachShader(p->id, compile(GL_VERTEX_SHADER_, vsrc, vs));
   pAttachShader(p->id, compile(GL_FRAGMENT_SHADER_, fsrc, fs));
   const char *attrs[] = {"position", "normal", "texCoord", "tangent", "binormal"};
   for (int i = 0; i < 5; i++)
      pBindAttribLocation(p->id, i, attrs[i]);
   pLinkProgram(p->id);
   GLint ok;
   pGetProgramiv(p->id, GL_LINK_STATUS_, &ok);
   if (!ok) {
      char log[4096];
      pGetProgramInfoLog(p->id, sizeof log, NULL, log);
      die("link %s: %s", name, log);
   }
   pUseProgram(p->id);
   for (int i = 0; i < NUM_U; i++)
      p->u[i] = pGetUniformLocation(p->id, uniform_names[i]);
   const char *samplers[] = {"diffuse", "normalMap", "reflectionMap", "skybox", "colorMap"};
   const int units[] = {0, 1, 2, 3, 0};
   for (int i = 0; i < 5; i++) {
      GLint loc = pGetUniformLocation(p->id, samplers[i]);
      if (loc >= 0)
         pUniform1i(loc, units[i]);
   }
   free(vsrc);
   free(fsrc);
}

static int find_prog(const char *name)
{
   for (int i = 0; i < nprogs; i++)
      if (!strcmp(progs[i].name, name))
         return i;
   die("unknown program %s", name);
   return -1;
}

static GLuint find_tex(const char *name)
{
   if (!strcmp(name, "-"))
      return 0;
   for (int i = 0; i < ntexs; i++)
      if (!strcmp(texs[i].name, name))
         return texs[i].id;
   die("unknown texture %s", name);
   return 0;
}

static void upload_levels(GLenum target, const unsigned char *data, int w, int h, int levels)
{
   for (int l = 0; l < levels; l++) {
      glTexImage2D(target, l, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
      data += (size_t)w * h * 4;
      w = w > 1 ? w / 2 : 1;
      h = h > 1 ? h / 2 : 1;
   }
}

static void load_tex(const char *dir, const char *name, const char *rel, int w, int h,
                     int levels, int clamp)
{
   Tex *t = &texs[ntexs++];
   unsigned char *data = (unsigned char *)read_file(dir, rel, NULL);
   snprintf(t->name, sizeof t->name, "%s", name);
   glGenTextures(1, &t->id);
   glBindTexture(GL_TEXTURE_2D, t->id);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   /* Sampling state before the upload, so the driver allocates the whole
    * mip tree with level 0 instead of copying levels in at first use. */
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                   levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL_, levels - 1);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp ? GL_CLAMP_TO_EDGE_ : GL_REPEAT);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp ? GL_CLAMP_TO_EDGE_ : GL_REPEAT);
   upload_levels(GL_TEXTURE_2D, data, w, h, levels);
   free(data);
}

static void load_cube(const char *dir, int size, int levels, char faces[6][128])
{
   glGenTextures(1, &skybox);
   glBindTexture(GL_TEXTURE_CUBE_MAP_, skybox);
   glTexParameteri(GL_TEXTURE_CUBE_MAP_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
   glTexParameteri(GL_TEXTURE_CUBE_MAP_, GL_TEXTURE_MAX_LEVEL_, levels - 1);
   for (int i = 0; i < 6; i++) {
      unsigned char *data = (unsigned char *)read_file(dir, faces[i], NULL);
      upload_levels(GL_TEXTURE_CUBE_MAP_POSITIVE_X_ + i, data, size, size, levels);
      free(data);
   }
   glTexParameteri(GL_TEXTURE_CUBE_MAP_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_CUBE_MAP_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
   glTexParameteri(GL_TEXTURE_CUBE_MAP_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
   glTexParameteri(GL_TEXTURE_CUBE_MAP_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
}

static void upload_mesh(const float *verts, int nverts, const uint16_t *idx, int nidx,
                        GLuint *vbo, GLuint *ibo)
{
   pGenBuffers(1, vbo);
   pBindBuffer(GL_ARRAY_BUFFER_, *vbo);
   pBufferData(GL_ARRAY_BUFFER_, (ptrdiff_t)nverts * 14 * 4, verts, GL_STATIC_DRAW_);
   pGenBuffers(1, ibo);
   pBindBuffer(GL_ELEMENT_ARRAY_BUFFER_, *ibo);
   pBufferData(GL_ELEMENT_ARRAY_BUFFER_, (ptrdiff_t)nidx * 2, idx, GL_STATIC_DRAW_);
}

static void load_model(const char *dir, AqModel *m, const char *rel)
{
   size_t len;
   char *buf = read_file(dir, rel, &len);
   uint32_t nv, ni;
   memcpy(&nv, buf, 4);
   memcpy(&ni, buf + 4, 4);
   if (len < 8 + (size_t)nv * 56 + (size_t)ni * 2)
      die("bad mesh %s", rel);
   upload_mesh((const float *)(buf + 8), nv, (const uint16_t *)(buf + 8 + (size_t)nv * 56), ni,
               &m->vbo, &m->ibo);
   m->nidx = ni;
   free(buf);
}

static void load_pack(const char *dir)
{
   char *man = read_file(dir, "manifest.txt", NULL);
   for (char *line = strtok(man, "\n"); line; line = strtok(NULL, "\n")) {
      char kind[16];
      if (sscanf(line, "%15s", kind) != 1)
         continue;
      if (!strcmp(kind, "tex")) {
         char name[64], rel[128];
         int w, h, levels, clamp;
         sscanf(line, "tex %63s %127s %d %d %d %d", name, rel, &w, &h, &levels, &clamp);
         load_tex(dir, name, rel, w, h, levels, clamp);
      } else if (!strcmp(kind, "cube")) {
         char name[32], faces[6][128];
         int size, levels;
         sscanf(line, "cube %31s %d %d %127s %127s %127s %127s %127s %127s", name, &size,
                &levels, faces[0], faces[1], faces[2], faces[3], faces[4], faces[5]);
         load_cube(dir, size, levels, faces);
      } else if (!strcmp(kind, "prog")) {
         char name[16], vs[128], fs[128];
         sscanf(line, "prog %15s %127s %127s", name, vs, fs);
         load_prog(dir, name, vs, fs);
      } else if (!strcmp(kind, "model")) {
         AqModel *m = &models[nmodels++];
         char prog[16], rel[128], t[3][64];
         sscanf(line, "model %31s %15s %d %d %15s %127s %63s %63s %63s", m->scene, prog,
                &m->fog, &m->blend, m->group, rel, t[0], t[1], t[2]);
         m->prog = find_prog(prog);
         for (int i = 0; i < 3; i++)
            m->tex[i] = find_tex(t[i]);
         load_model(dir, m, rel);
      } else if (!strcmp(kind, "lightray")) {
         char prog[16], tex[64];
         sscanf(line, "lightray %15s %63s", prog, tex);
         lightray_prog = find_prog(prog);
         lightray_tex = find_tex(tex);
      } else if (!strcmp(kind, "fish")) {
         Fish *f = &fishes[nfish++];
         sscanf(line, "fish %31s %f %f %f %f %f %f %f %f %f %f", f->name, &f->speed,
                &f->speedRange, &f->radius, &f->radiusRange, &f->tailSpeed, &f->heightOffset,
                &f->heightRange, &f->fishLength, &f->fishWaveLength, &f->fishBendAmount);
      } else if (!strcmp(kind, "view") && nviews < 8) {
         View *v = &views[nviews++];
         sscanf(line, "view %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f", &v->eyeHeight,
                &v->eyeRadius, &v->eyeSpeed, &v->targetHeight, &v->targetRadius, &v->fov,
                &v->ambient[0], &v->ambient[1], &v->ambient[2], &v->fogPower, &v->fogMult,
                &v->fogOffset, &v->fog[0], &v->fog[1], &v->fog[2]);
      } else if (!strcmp(kind, "place")) {
         Place *p = &places[nplaces++];
         float *w = p->world.m;
         sscanf(line, "place %31s %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f", p->scene,
                &w[0], &w[1], &w[2], &w[3], &w[4], &w[5], &w[6], &w[7], &w[8], &w[9], &w[10],
                &w[11], &w[12], &w[13], &w[14], &w[15]);
      }
   }
   free(man);
   if (!nviews)
      die("pack has no views; rebuild it with convert.py");
   for (int i = 0; i < nfish; i++) {
      fishes[i].model = -1;
      for (int j = 0; j < nmodels; j++)
         if (!strcmp(models[j].scene, fishes[i].name)) {
            fishes[i].model = j;
            break;
         }
      if (fishes[i].model < 0)
         die("no model for fish %s", fishes[i].name);
   }
   /* The light ray: tdl's 1x1 plane, reoriented to stand in XY from y=0..1. */
   float v[4 * 14] = {0};
   const float xy[4][2] = {{-0.5f, 0}, {0.5f, 0}, {-0.5f, 1}, {0.5f, 1}};
   for (int i = 0; i < 4; i++) {
      v[i * 14 + 0] = xy[i][0];
      v[i * 14 + 1] = xy[i][1];
      v[i * 14 + 6] = xy[i][0] + 0.5f;
      v[i * 14 + 7] = 1 - xy[i][1];
   }
   const uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
   upload_mesh(v, 4, idx, 6, &lightray_vbo, &lightray_ibo);
}

/* ---- drawing ------------------------------------------------------------ */

typedef struct {
   M4 viewProjection, viewInverse, viewProjectionNoRotation;
   float lightWorldPos[3], ambient[4], fogColor[4];
   float fogPower, fogMult, fogOffset, rayY;
   float clock;
} Frame;

typedef struct {
   float shininess, specularFactor;
   int inner;
} Consts;

static long draws, tris;
static const float one4[4] = {1, 1, 1, 1};

static void bind_mesh(GLuint vbo, GLuint ibo)
{
   pBindBuffer(GL_ARRAY_BUFFER_, vbo);
   pBindBuffer(GL_ELEMENT_ARRAY_BUFFER_, ibo);
   const int comps[5] = {3, 3, 2, 3, 3}, offs[5] = {0, 3, 6, 8, 11};
   for (int i = 0; i < 5; i++) {
      pEnableVertexAttribArray(i);
      pVertexAttribPointer(i, comps[i], GL_FLOAT, GL_FALSE, 56, (const void *)(intptr_t)(offs[i] * 4));
   }
}

/* tdl's drawPrep: program, shared uniforms, buffers and textures. */
static void draw_prep(const AqModel *m, const Frame *f, const Consts *c)
{
   const Prog *p = &progs[m->prog];
   pUseProgram(p->id);
   pUniformMatrix4fv(p->u[U_viewProjection], 1, GL_FALSE, f->viewProjection.m);
   pUniformMatrix4fv(p->u[U_viewInverse], 1, GL_FALSE, f->viewInverse.m);
   pUniform3fv(p->u[U_lightWorldPos], 1, f->lightWorldPos);
   pUniform4fv(p->u[U_lightColor], 1, one4);
   pUniform4fv(p->u[U_specular], 1, one4);
   pUniform1f(p->u[U_shininess], c->shininess);
   pUniform1f(p->u[U_specularFactor], c->specularFactor);
   pUniform4fv(p->u[U_ambient], 1, f->ambient);
   pUniform1f(p->u[U_fogPower], f->fogPower);
   pUniform1f(p->u[U_fogMult], f->fogMult);
   pUniform1f(p->u[U_fogOffset], f->fogOffset);
   pUniform4fv(p->u[U_fogColor], 1, f->fogColor);
   if (c->inner) {
      pUniform1f(p->u[U_refractionFudge], 3);
      pUniform1f(p->u[U_eta], 1);
      pUniform1f(p->u[U_tankColorFudge], 0.8f);
   }
   bind_mesh(m->vbo, m->ibo);
   for (int i = 0; i < 3; i++) {
      pActiveTexture(GL_TEXTURE0_ + i);
      glBindTexture(GL_TEXTURE_2D, m->tex[i]);
   }
   pActiveTexture(GL_TEXTURE0_ + 3);
   glBindTexture(GL_TEXTURE_CUBE_MAP_, skybox);
}

static void draw_elements(int nidx)
{
   glDrawElements(GL_TRIANGLES, nidx, GL_UNSIGNED_SHORT, 0);
   draws++;
   tris += nidx / 3;
}

static void draw_group(const char *group, const Frame *f, const Consts *c)
{
   const AqModel *current = NULL;
   int ii = 0;
   for (int pi = 0; pi < nplaces; pi++) {
      const Place *pl = &places[pi];
      int in_group = 0;
      for (int mi = 0; mi < nmodels; mi++) {
         const AqModel *m = &models[mi];
         if (strcmp(m->scene, pl->scene) || strcmp(m->group, group))
            continue;
         in_group = 1;
         if (m->blend)
            glEnable(GL_BLEND);
         else
            glDisable(GL_BLEND);
         if (m != current) {
            current = m;
            draw_prep(m, f, c);
         }
         const Prog *p = &progs[m->prog];
         M4 wit = m4_transpose(m4_inverse(pl->world));
         pUniformMatrix4fv(p->u[U_world], 1, GL_FALSE, pl->world.m);
         pUniformMatrix4fv(p->u[U_worldInverseTranspose], 1, GL_FALSE, wit.m);
         pUniform1f(p->u[U_time], f->clock + ii);
         draw_elements(m->nidx);
      }
      ii += in_group;
   }
}

/* aquarium.js's split of a fish count over the types. */
static void fish_counts(int total, int *num)
{
   int left = total;
   const char *types[] = {"Big", "Medium", "Small"};
   for (int t = 0; t < 3; t++)
      for (int i = 0; i < nfish; i++) {
         if (strncmp(fishes[i].name, types[t], strlen(types[t])))
            continue;
         int n = left;
         if (t == 0)
            n = left < (total < 100 ? 1 : 2) ? left : (total < 100 ? 1 : 2);
         else if (t == 1) {
            int cap = total < 1000 ? total / 10 : total < 10000 ? 80 : 160;
            n = left < cap ? left : cap;
         }
         left -= n;
         num[i] = n;
      }
}

typedef struct { float duration, timer, rot, x; } LightRay;
static LightRay rays[5];

/* The page uses Math.random for the rays; a fixed-seed generator keeps
 * them identical on both systems. */
static uint32_t ray_seed = 1;
static float ray_random(void)
{
   ray_seed = ray_seed * 1664525u + 1013904223u;
   return (ray_seed >> 8) / 16777216.0f;
}

static void init_ray(LightRay *r)
{
   r->duration = 1 + ray_random();
   r->timer = r->duration;
   float v = ray_random();
   r->rot = v;
   r->x = (v - 0.5f) * 20;
}

static void render_scene(const Frame *f, int total_fish)
{
   const Consts generic = {50, 1, 0}, outside = {50, 0, 0}, inner = {50, 1, 1},
                seaweed = {50, 1, 0}, fishc = {5, 0.3f, 0};
   glClearColor(0, 0.8f, 1, 0);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   glEnable(GL_DEPTH_TEST);
   glDisable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   pBlendEquation(GL_FUNC_ADD_);
   glEnable(GL_CULL_FACE);
   glDepthMask(GL_TRUE);
   rand_seed = 0;

   draw_group("base", f, &generic);

   glEnable(GL_BLEND);
   int num[8] = {0};
   fish_counts(total_fish, num);
   for (int ff = 0; ff < nfish; ff++) {
      const Fish *fi = &fishes[ff];
      const AqModel *m = &models[fi->model];
      const Prog *p = &progs[m->prog];
      if (!num[ff])
         continue;
      draw_prep(m, f, &fishc);
      pUniform1f(p->u[U_fishLength], fi->fishLength);
      pUniform1f(p->u[U_fishWaveLength], fi->fishWaveLength);
      pUniform1f(p->u[U_fishBendAmount], fi->fishBendAmount);
      double base_clock = f->clock * 0.124;
      double height = 25 + fi->heightOffset, height_range = 1 * fi->heightRange;
      for (int ii = 0; ii < num[ff]; ii++) {
         double fish_clock = base_clock + ii * 0.52;
         double speed = fi->speed + pseudo_random() * fi->speedRange;
         double scale = 1.0 + pseudo_random() * 1;
         double xr = fi->radius + pseudo_random() * fi->radiusRange;
         double yr = 2.0 + pseudo_random() * height_range;
         double zr = fi->radius + pseudo_random() * fi->radiusRange;
         double sc = fish_clock * speed;
         double xc = sc * 1, yc = sc * 0.556, zc = sc * 1;
         float pos[3] = {(float)(sin(xc) * xr), (float)(sin(yc) * yr + height), (float)(cos(zc) * zr)};
         float next[3] = {(float)(sin(xc - 0.04) * xr), (float)(sin(yc - 0.01) * yr + height),
                          (float)(cos(zc - 0.04) * zr)};
         pUniform3fv(p->u[U_worldPosition], 1, pos);
         pUniform3fv(p->u[U_nextPosition], 1, next);
         pUniform1f(p->u[U_scale], (float)scale);
         pUniform1f(p->u[U_time], (float)fmod((f->clock + ii) * fi->tailSpeed * speed, M_PI * 2));
         draw_elements(m->nidx);
      }
   }

   draw_group("inner", f, &inner);
   draw_group("seaweed", f, &seaweed);
   draw_group("outside", f, &outside);

   /* Light rays. */
   glEnable(GL_BLEND);
   glDisable(GL_CULL_FACE);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE);
   glDepthMask(GL_FALSE);
   if (lightray_prog >= 0) {
      const Prog *p = &progs[lightray_prog];
      pUseProgram(p->id);
      pUniformMatrix4fv(p->u[U_viewProjection], 1, GL_FALSE, f->viewProjectionNoRotation.m);
      bind_mesh(lightray_vbo, lightray_ibo);
      pActiveTexture(GL_TEXTURE0_);
      glBindTexture(GL_TEXTURE_2D, lightray_tex);
      for (int i = 0; i < 5; i++) {
         float lerp = rays[i].timer / rays[i].duration;
         M4 w = m4_mul(m4_mul(translation(rays[i].x, f->rayY, 0), rotation_z(rays[i].rot + lerp * 0.2f)),
                       scaling(10, -100, 10));
         float cm[4] = {1, 1, 1, sinf(lerp * (float)M_PI)};
         pUniformMatrix4fv(p->u[U_world], 1, GL_FALSE, w.m);
         pUniform4fv(p->u[U_colorMult], 1, cm);
         draw_elements(6);
      }
   }
   glDepthMask(GL_TRUE);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

   draw_group("outer", f, &inner);
}

/* ---- main --------------------------------------------------------------- */

static void os_label(char *buf, size_t n)
{
#if defined(__APPLE__)
   char ver[64] = "?";
   size_t len = sizeof ver;
   sysctlbyname("kern.osproductversion", ver, &len, NULL, 0);
   snprintf(buf, n, "macOS %s", ver);
#else
   struct utsname u;
   uname(&u);
   snprintf(buf, n, "Linux %s", u.release);
#endif
}

static void fmt_int(char *buf, size_t n, long v)
{
   char tmp[32];
   int len = snprintf(tmp, sizeof tmp, "%ld", v), o = 0;
   for (int i = 0; i < len && o < (int)n - 1; i++) {
      if (i && (len - i) % 3 == 0 && o < (int)n - 2)
         buf[o++] = ',';
      buf[o++] = tmp[i];
   }
   buf[o] = 0;
}

int main(int argc, char **argv)
{
   const char *pack = NULL, *label = NULL;
   int tour[16] = {1000}, ntour = 1, fbw = 1600, fbh = 1000, view = 0, win[4] = {0};
   float secs = 25, quit_after = 0;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--fish") && i + 1 < argc)
         tour[0] = atoi(argv[++i]), ntour = 1;
      else if (!strcmp(argv[i], "--tour") && i + 1 < argc) {
         ntour = 0;
         for (char *s = strtok(argv[++i], ","); s && ntour < 16; s = strtok(NULL, ","))
            tour[ntour++] = atoi(s);
      } else if (!strcmp(argv[i], "--secs") && i + 1 < argc)
         secs = atof(argv[++i]);
      else if (!strcmp(argv[i], "--size") && i + 1 < argc)
         sscanf(argv[++i], "%dx%d", &fbw, &fbh);
      else if (!strcmp(argv[i], "--window") && i + 1 < argc)
         sscanf(argv[++i], "%d,%d,%d,%d", &win[0], &win[1], &win[2], &win[3]);
      else if (!strcmp(argv[i], "--view") && i + 1 < argc)
         view = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--label") && i + 1 < argc)
         label = argv[++i];
      else if (!strcmp(argv[i], "--quit-after") && i + 1 < argc)
         quit_after = atof(argv[++i]);
      else if (argv[i][0] != '-' && !pack)
         pack = argv[i];
      else
         die("usage: aquarium PACKDIR [--fish N | --tour N,N,...] [--secs S] [--size WxH] "
             "[--view N] [--window X,Y,W,H] [--label TEXT] [--quit-after S]");
   }
   if (!pack)
      die("no pack directory given");

   SetTraceLogLevel(LOG_WARNING);
   SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);
   InitWindow(win[2] > 0 ? win[2] : 1280, win[3] > 0 ? win[3] : 800, "Aquarium (native GL 2.1)");
   if (win[2] > 0)
      SetWindowPosition(win[0], win[1]);
   SetTargetFPS(0);
#if defined(__APPLE__)
   GLint zero = 0;
   CGLSetParameter(CGLGetCurrentContext(), kCGLCPSwapInterval, &zero);
#else
   SDL_GL_SetSwapInterval(0);
#endif
   load_gl();
   load_pack(pack);
   view = ((view % nviews) + nviews) % nviews;

   GLuint fbo, color, depth;
   glGenTextures(1, &color);
   glBindTexture(GL_TEXTURE_2D, color);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbw, fbh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
   pGenRenderbuffers(1, &depth);
   pBindRenderbuffer(GL_RENDERBUFFER_, depth);
   pRenderbufferStorage(GL_RENDERBUFFER_, GL_DEPTH_COMPONENT24_, fbw, fbh);
   pGenFramebuffers(1, &fbo);
   pBindFramebuffer(GL_FRAMEBUFFER_, fbo);
   pFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D, color, 0);
   pFramebufferRenderbuffer(GL_FRAMEBUFFER_, GL_DEPTH_ATTACHMENT_, GL_RENDERBUFFER_, depth);
   if (pCheckFramebufferStatus(GL_FRAMEBUFFER_) != GL_FRAMEBUFFER_COMPLETE_)
      die("offscreen framebuffer incomplete");
   pBindFramebuffer(GL_FRAMEBUFFER_, 0);
   Texture2D scene_tex = {color, fbw, fbh, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};

   char os[64], gpu[160], line[256];
   os_label(os, sizeof os);
   snprintf(gpu, sizeof gpu, "%s  |  OpenGL %s", (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VERSION));
   fprintf(stderr, "aquarium: %s; %s; scene %dx%d\n", os, gpu, fbw, fbh);
   printf("t,fish,fps,frame_ms,draws,triangles\n");
   fflush(stdout);

   for (int i = 0; i < 5; i++)
      init_ray(&rays[i]);
   double clock = 0, eye_clock = 0, then = GetTime(), start = then, step_start = then;
   double acc_t = 0, shown_fps = 0, shown_ms = 0;
   long acc_frames = 0, shown_draws = 0, shown_tris = 0;
   int step = 0;

   while (!WindowShouldClose()) {
      double now = GetTime(), dt = now - then;
      then = now;
      if (IsKeyPressed(KEY_V))
         view = (view + 1) % nviews;
      const View *vw = &views[view];
      clock += dt;
      eye_clock += dt * vw->eyeSpeed;
      for (int i = 0; i < 5; i++) {
         rays[i].timer -= dt;
         if (rays[i].timer < 0)
            init_ray(&rays[i]);
      }
      if (ntour > 1 && now - step_start >= secs) {
         step = (step + 1) % ntour;
         step_start = now;
         acc_t = 0;
         acc_frames = 0;
      }
      if (quit_after > 0 && now - start >= quit_after)
         break;

      Frame f;
      float eye[3] = {sinf(eye_clock) * vw->eyeRadius, vw->eyeHeight, cosf(eye_clock) * vw->eyeRadius};
      float target[3] = {sinf(eye_clock + M_PI) * vw->targetRadius, vw->targetHeight,
                         cosf(eye_clock + M_PI) * vw->targetRadius};
      f.viewInverse = camera_look_at(eye, target);
      M4 view_m = m4_inverse(f.viewInverse);
      float top = tanf(vw->fov * (float)M_PI / 180 * 0.5f), right = top * fbw / fbh;
      M4 proj = frustum(-right, right, -top, top, 1, 25000);
      f.viewProjection = m4_mul(proj, view_m);
      f.viewProjectionNoRotation = m4_mul(proj, translation(view_m.m[12], view_m.m[13], view_m.m[14]));
      for (int i = 0; i < 3; i++)
         f.lightWorldPos[i] = eye[i] + f.viewInverse.m[i] * 20 + f.viewInverse.m[4 + i] * 30;
      for (int i = 0; i < 3; i++) {
         f.ambient[i] = vw->ambient[i];
         f.fogColor[i] = vw->fog[i];
      }
      f.ambient[3] = 0;
      f.fogColor[3] = 1;
      f.fogPower = vw->fogPower;
      f.fogMult = vw->fogMult;
      f.fogOffset = vw->fogOffset;
      f.rayY = fmaxf(70, fminf(120, 50 + vw->eyeHeight));
      f.clock = (float)clock;

      if (pBindVertexArray)
         pBindVertexArray(0);
      pBindFramebuffer(GL_FRAMEBUFFER_, fbo);
      glViewport(0, 0, fbw, fbh);
      draws = tris = 0;
      render_scene(&f, tour[step]);
      long frame_draws = draws, frame_tris = tris;

      /* Hand the context back to raylib in the state it expects. */
      for (int i = 0; i < 5; i++)
         pDisableVertexAttribArray(i);
      pBindBuffer(GL_ARRAY_BUFFER_, 0);
      pBindBuffer(GL_ELEMENT_ARRAY_BUFFER_, 0);
      pUseProgram(0);
      for (int i = 3; i >= 0; i--) {
         pActiveTexture(GL_TEXTURE0_ + i);
         glBindTexture(GL_TEXTURE_2D, 0);
         glBindTexture(GL_TEXTURE_CUBE_MAP_, 0);
      }
      pBindFramebuffer(GL_FRAMEBUFFER_, 0);
      glViewport(0, 0, GetRenderWidth(), GetRenderHeight());
      glDisable(GL_DEPTH_TEST);
      glEnable(GL_BLEND);
      glEnable(GL_CULL_FACE);
      glCullFace(GL_BACK);

      acc_t += dt;
      acc_frames++;
      if (acc_t >= 1.0) {
         shown_fps = acc_frames / acc_t;
         shown_ms = 1000.0 * acc_t / acc_frames;
         shown_draws = frame_draws;
         shown_tris = frame_tris;
         printf("%.1f,%d,%.1f,%.2f,%ld,%ld\n", now - start, tour[step], shown_fps, shown_ms,
                shown_draws, shown_tris);
         fflush(stdout);
         acc_t = 0;
         acc_frames = 0;
      }

      BeginDrawing();
      ClearBackground(BLACK);
      float sw = GetScreenWidth(), sh = GetScreenHeight();
      float s = fminf(sw / fbw, sh / fbh), dw = fbw * s, dh = fbh * s;
      DrawTexturePro(scene_tex, (Rectangle){0, 0, (float)fbw, (float)-fbh},
                     (Rectangle){(sw - dw) / 2, (sh - dh) / 2, dw, dh}, (Vector2){0, 0}, 0, WHITE);
      int fs = sh >= 700 ? 20 : 16, lh = fs + 6, x = 16, y = 16;
      DrawRectangle(8, 8, (int)fminf(sw - 16, 640), lh * 6 + 12, (Color){0, 0, 0, 150});
      snprintf(line, sizeof line, "%s", label ? label : os);
      DrawText(line, x, y, fs, WHITE), y += lh;
      DrawText(gpu, x, y, fs, (Color){200, 220, 235, 255}), y += lh;
      snprintf(line, sizeof line, "fps %.1f   frame %.2f ms", shown_fps, shown_ms);
      DrawText(line, x, y, fs + 6, (Color){120, 220, 255, 255}), y += lh + 6;
      char nf[32], nd[32], nt[32];
      fmt_int(nf, sizeof nf, tour[step]);
      fmt_int(nd, sizeof nd, shown_draws);
      fmt_int(nt, sizeof nt, shown_tris);
      snprintf(line, sizeof line, "fish %s   draws/frame %s   triangles/frame %s", nf, nd, nt);
      DrawText(line, x, y, fs, WHITE), y += lh;
      if (ntour > 1)
         snprintf(line, sizeof line, "scene %dx%d, vsync off, view %d   step %d/%d, next in %.0f s",
                  fbw, fbh, view, step + 1, ntour, secs - (now - step_start));
      else
         snprintf(line, sizeof line, "scene %dx%d, vsync off, view %d", fbw, fbh, view);
      DrawText(line, x, y, fs, (Color){200, 220, 235, 255});
      EndDrawing();
   }
   CloseWindow();
   return 0;
}
