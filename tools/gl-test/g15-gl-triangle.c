// An ordinary GLES 3.0 program: clear a 128x128 RGBA8 framebuffer object to
// opaque blue, draw one red triangle, read it back and compare it with the
// CPU oracle in render-ref.h (the same image the kernel's render tests use).
// Nothing in it is G15-specific, so it doubles as an llvmpipe reference.
//
//   g15-gl-triangle [--driver NAME|software] [--mode const|external|externalnv12|...] [--size WxH] [--out FILE]
//
// --size renders to a W x H framebuffer (each 1..4096) with the same 128x128
// viewport at the origin, so the triangle keeps its pixel positions; the
// expected image is the same edge test over W x H (#10 R4: the native 160x160,
// 256x256 and 200x120 captures have exactly these images).
//
// Modes draw the same image three ways: a constant-colour fragment shader,
// the colour through a varying that is equal at all three vertices (so
// interpolation is exact), and positions from a vertex buffer.
//
// Picks the first EGL device whose DRM driver is NAME (default asahi), or with
// "software" the device without a DRM node (llvmpipe), so a reference run
// never touches a GPU. Prints one result line:
//   G15_GL_RESULT PASS|FAIL mismatches=N red=N canary=N fnv1a64=H expected=H
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>

#include "render-ref.h"

#define MAX2(a, b) ((a) > (b) ? (a) : (b))

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

// Native image hashes (#10 R4) of the triangle at the sizes captured on macOS.
static const struct {
    int w, h;
    uint64_t fnv;
} native_triangles[] = {
    {128, 128, M3R_TRIANGLE_FNV},
    {160, 160, UINT64_C(0x50034a3431ca56a5)},
    {256, 256, UINT64_C(0x14ce4aff7b78e0a5)},
    {200, 120, UINT64_C(0x7321c46ab72ae325)},
};

// m3r_expected over a W x H image.
static void expected_image(int w, int h, int triangle, uint8_t *out) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *p = out + ((size_t)y * w + x) * 4;
            int red = triangle && m3r_inside(x, y);
            p[0] = red ? 255 : 0;
            p[1] = 0;
            p[2] = red ? 0 : 255;
            p[3] = 255;
        }
}

// m3r_check_pixels over `bytes` bytes.
static struct m3r_check check_image(const uint8_t *actual, const uint8_t *expected, size_t bytes) {
    struct m3r_check r = {0};
    for (size_t i = 0; i < bytes; i += 4) {
        r.mismatched_pixels += memcmp(actual + i, expected + i, 4) != 0;
        r.red_pixels += actual[i] == 255 && actual[i + 1] == 0 && actual[i + 2] == 0 && actual[i + 3] == 255;
        r.canary_pixels += actual[i + 3] == M3R_CANARY_ALPHA;
    }
    r.actual_hash = m3r_hash(actual, bytes);
    r.expected_hash = m3r_hash(expected, bytes);
    return r;
}

// Window coordinates equal image rows here: GL FBO row 0 is window y = 0 and
// glReadPixels returns rows bottom-up, so (16,16) is memory row 16, column 16.
static const char *vs_src =
    "#version 300 es\n"
    "void main() {\n"
    "    vec2 p = gl_VertexID == 0 ? vec2(-0.75, -0.75)\n"
    "           : gl_VertexID == 1 ? vec2(0.765625, -0.75)\n"
    "                              : vec2(-0.75, 0.75);\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char *vs_varying_src =
    "#version 300 es\n"
    "out vec4 v_color;\n"
    "void main() {\n"
    "    vec2 p = gl_VertexID == 0 ? vec2(-0.75, -0.75)\n"
    "           : gl_VertexID == 1 ? vec2(0.765625, -0.75)\n"
    "                              : vec2(-0.75, 0.75);\n"
    "    float one = float(gl_VertexID + 1) / float(gl_VertexID + 1);\n"
    "    v_color = vec4(one, 0.0, 0.0, one);\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char *fs_varying_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec4 v_color;\n"
    "out vec4 color;\n"
    "void main() { color = v_color; }\n";

// Red and green follow window x and y: (x_ndc + 1) / 2 at each vertex, so the
// interpolated value at a pixel centre is (px + 0.5) / 128.
static const char *vs_gradient_src =
    "#version 300 es\n"
    "out vec2 v_xy;\n"
    "void main() {\n"
    "    vec2 p = gl_VertexID == 0 ? vec2(-0.75, -0.75)\n"
    "           : gl_VertexID == 1 ? vec2(0.765625, -0.75)\n"
    "                              : vec2(-0.75, 0.75);\n"
    "    v_xy = p * 0.5 + 0.5;\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char *fs_gradient_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_xy;\n"
    "out vec4 color;\n"
    "void main() { color = vec4(v_xy, 0.0, 1.0); }\n";

// Preserve the same screen-space triangle while assigning different clip W
// values. A non-perspective scalar still follows window X exactly; a smooth
// scalar does not, so this detects both the shader iterator and PPP shading
// mode instead of merely checking that the stage was admitted.
static const char *vs_linear_src =
    "#version 300 es\n"
    "#extension GL_NV_shader_noperspective_interpolation : require\n"
    "noperspective out float v_value;\n"
    "void main() {\n"
    "    vec2 p = gl_VertexID == 0 ? vec2(-0.75, -0.75)\n"
    "           : gl_VertexID == 1 ? vec2(0.765625, -0.75)\n"
    "                              : vec2(-0.75, 0.75);\n"
    "    float w = gl_VertexID == 0 ? 1.0 : (gl_VertexID == 1 ? 2.0 : 4.0);\n"
    "    v_value = p.x * 0.5 + 0.5;\n"
    "    gl_Position = vec4(p * w, 0.0, w);\n"
    "}\n";

static const char *fs_linear_src =
    "#version 300 es\n"
    "#extension GL_NV_shader_noperspective_interpolation : require\n"
    "precision highp float;\n"
    "noperspective in float v_value;\n"
    "out vec4 color;\n"
    "void main() { color = vec4(v_value, 0.0, 0.0, 1.0); }\n";

// Match the dominant rejected benchmark shape: a texture and API uniform in
// a fragment shader with real demotion. A nearest 2x1 alpha mask discards the
// left half of the ordinary triangle and keeps the right half red.
static const char *fs_discard_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_color;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    float a = texture(u_tex, vec2(gl_FragCoord.x / 128.0, 0.5)).a;\n"
    "    if (a < 0.5) discard;\n"
    "    color = u_color;\n"
    "}\n";

// Match the remaining glmark refusal shape: four distinct vertex buffers, an
// API uniform, two vertex-stage textures and fourteen scalar varyings. Every
// varying component reaches a different image band, so an exact-image pass
// proves the vertex shader sampled both texture/sampler tables successfully.
static const char *vs_vertextex_src =
    "#version 300 es\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in float a_zero0;\n"
    "layout(location = 2) in float a_zero1;\n"
    "layout(location = 3) in float a_zero2;\n"
    "uniform highp sampler2D u_tex0;\n"
    "uniform highp sampler2D u_tex1;\n"
    "uniform highp vec4 u_bias;\n"
    "out highp vec4 v0;\n"
    "out highp vec4 v1;\n"
    "out highp vec4 v2;\n"
    "out highp vec2 v3;\n"
    "void main() {\n"
    "    float z = a_zero0 + a_zero1 + a_zero2 + u_bias.x;\n"
    "    highp vec4 t0 = textureLod(u_tex0, vec2(0.5 + z), 0.0);\n"
    "    highp vec4 t1 = textureLod(u_tex1, vec2(0.5 + z), 0.0);\n"
    "    v0 = t0;\n"
    "    v1 = t1;\n"
    "    v2 = vec4(t0.rg, t1.rg);\n"
    "    v3 = vec2(t0.b, t1.b);\n"
    "    gl_Position = vec4(a_pos + vec2(z), 0.0, 1.0);\n"
    "}\n";

static const char *fs_vertextex_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec4 v0;\n"
    "in vec4 v1;\n"
    "in vec4 v2;\n"
    "in vec2 v3;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    if (gl_FragCoord.x < 48.0) color = v0;\n"
    "    else if (gl_FragCoord.x < 80.0) color = v1;\n"
    "    else if (gl_FragCoord.x < 96.0) color = v2;\n"
    "    else color = vec4(v3, v0.a * v1.a, v2.a);\n"
    "}\n";

static const char *vs_vbo_src =
    "#version 300 es\n"
    "in vec2 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const float vbo_positions[] = {-0.75f, -0.75f, 0.765625f, -0.75f, -0.75f, 0.75f};

// The same triangle as two draws that share the edge from (113,16) to
// (16,64): the fill rule covers each pixel once, so the image is unchanged.
// The split above as one fan around (113,16): (113,16), (16,16), (16,64), (16,112).
static const float fan_positions[] = {0.765625f, -0.75f, -0.75f, -0.75f, -0.75f, 0.0f, -0.75f, 0.75f};

static const float split_positions[] = {-0.75f, -0.75f,    0.765625f, -0.75f, -0.75f, 0.0f,
                                        -0.75f, 0.0f,      0.765625f, -0.75f, -0.75f, 0.75f};

static const char *fs_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "out vec4 color;\n"
    "void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }\n";

// Depth modes (#10, Codex's native D32F reference): a W x H RGBA8 colour
// buffer and a DEPTH_COMPONENT32F depth buffer, cleared to blue and 1.0,
// depth test LESS. The near red triangle (16,16),(113,16),(16,112) at depth
// 0.25, then the far green one (32,32),(113,32),(32,112) at 0.75, in the
// same 128x128 viewport. Colour is read with glReadPixels, depth with
// NV_read_depth (GL_DEPTH_COMPONENT, GL_FLOAT) and compared bit for bit.
//   depth        the native raster scene
//   depthload    the scene, a readback (new batch), then the far triangle
//                again: every fragment fails LESS, both images unchanged
//   depthexport  near from z 0.5 with gl_FragDepth = 0.25 (late test)
//   depthmask    far first with depth writes off, then near
static const char *vs_depth_src =
    "#version 300 es\n"
    "in vec3 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 1.0); }\n";
static const char *fs_depth_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform vec4 u_color;\n"
    "out vec4 color;\n"
    "void main() { color = u_color; }\n";
static const char *fs_depth_export_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform vec4 u_color;\n"
    "out vec4 color;\n"
    "void main() { color = u_color; gl_FragDepth = 0.25; }\n";

// x_ndc = px / 64 - 1 in the 128x128 viewport; z_ndc = 2 * depth - 1.
static const float near_tri[] = {-0.75f, -0.75f, -0.5f, 0.765625f, -0.75f, -0.5f, -0.75f, 0.75f, -0.5f};
static const float near_export_tri[] = {-0.75f, -0.75f, 0.0f, 0.765625f, -0.75f, 0.0f, -0.75f, 0.75f, 0.0f};
static const float far_tri[] = {-0.5f, -0.5f, 0.5f, 0.765625f, -0.5f, 0.5f, -0.5f, 0.75f, 0.5f};

static int inside_tri(const int v[6], int x, int y) {
    const int px = 2 * x + 1, py = 2 * y + 1;
    for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        if ((2 * v[2 * j] - 2 * v[2 * i]) * (py - 2 * v[2 * i + 1]) -
                (2 * v[2 * j + 1] - 2 * v[2 * i + 1]) * (px - 2 * v[2 * i]) <=
            0)
            return 0;
    }
    return 1;
}

static int fail(const char *what);
static GLuint compile(GLenum type, const char *src);

static GLuint link_program(const char *vs_src, const char *fs_src) {
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    return linked ? prog : 0;
}

static void draw_depth_tri(GLuint prog, const float *pos, float r, float g) {
    glUseProgram(prog);
    glUniform4f(glGetUniformLocation(prog, "u_color"), r, g, 0.0f, 1.0f);
    glBufferData(GL_ARRAY_BUFFER, 9 * sizeof(float), pos, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// --format bgra: a GL_BGRA8_EXT colour renderbuffer (read back as RGBA).
static GLenum colour_format = GL_RGBA8;

// --depth16: the depth modes use DEPTH_COMPONENT16; depth is then compared
// with llvmpipe through depth_fnv (the float oracle is not 16-bit exact), and
// colour with the oracle as usual.
static int depth16;
static int run_depth(const char *mode, int W, int H, const char *out) {
    int load = !strcmp(mode, "depthload"), export = !strcmp(mode, "depthexport");
    int mask = !strcmp(mode, "depthmask");
    if (!load && !export && !mask && strcmp(mode, "depth"))
        return fail("unknown-mode");
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    if (!ext || !strstr(ext, "GL_NV_read_depth"))
        return fail("no-NV_read_depth");
    GLuint rb[2], fbo;
    glGenRenderbuffers(2, rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorage(GL_RENDERBUFFER, colour_format, W, H);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorage(GL_RENDERBUFFER, depth16 ? GL_DEPTH_COMPONENT16 : GL_DEPTH_COMPONENT32F, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("fbo-incomplete");
    GLuint prog = link_program(vs_depth_src, fs_depth_src);
    GLuint prog_export = link_program(vs_depth_src, fs_depth_export_src);
    if (!prog || !prog_export)
        return fail("compile");
    GLuint buf;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);

    glViewport(0, 0, M3R_WIDTH, M3R_HEIGHT);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClearDepthf(1.0f);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    if (mask) {
        glDepthMask(GL_FALSE);
        draw_depth_tri(prog, far_tri, 0.0f, 1.0f);
        glDepthMask(GL_TRUE);
        draw_depth_tri(prog, near_tri, 1.0f, 0.0f);
    } else {
        draw_depth_tri(export ? prog_export : prog, export ? near_export_tri : near_tri, 1.0f, 0.0f);
        draw_depth_tri(prog, far_tri, 0.0f, 1.0f);
    }
    const size_t n = (size_t)W * H;
    uint8_t *colour = malloc(n * 4), *expected = malloc(n * 4);
    float *depth = malloc(n * 4), *expected_depth = malloc(n * 4);
    if (!colour || !expected || !depth || !expected_depth)
        return fail("malloc");
    if (load) {
        // A readback ends the batch; the far triangle then draws in a new
        // one that loads both buffers.
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, colour);
        draw_depth_tri(prog, far_tri, 0.0f, 1.0f);
    }
    memset(colour, M3R_CANARY_ALPHA, n * 4);
    memset(depth, 0x5a, n * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, colour);
    // DEPTH_COMPONENT16 reads as GL_UNSIGNED_SHORT (the first n*2 bytes).
    glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, depth16 ? GL_UNSIGNED_SHORT : GL_FLOAT, depth);
    if (glGetError() != GL_NO_ERROR)
        return fail("readpixels");

    static const int near_v[6] = {16, 16, 113, 16, 16, 112}, far_v[6] = {32, 32, 113, 32, 32, 112};
    unsigned near_px = 0, far_px = 0, overlap = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = (size_t)y * W + x;
            int nr = inside_tri(near_v, x, y), fr = inside_tri(far_v, x, y);
            near_px += nr;
            far_px += fr && !nr;
            overlap += nr && fr;
            uint8_t *e = expected + 4 * i;
            e[0] = nr ? 255 : 0;
            e[1] = !nr && fr ? 255 : 0;
            e[2] = nr || fr ? 0 : 255;
            e[3] = 255;
            expected_depth[i] = nr ? 0.25f : fr && !mask ? 0.75f : 1.0f;
        }
    unsigned bad_c = 0, bad_d = 0;
    for (size_t i = 0; i < n; ++i) {
        bad_c += memcmp(colour + 4 * i, expected + 4 * i, 4) != 0;
        bad_d += !depth16 && memcmp(depth + i, expected_depth + i, 4) != 0;
    }
    if (out) {
        FILE *f = fopen(out, "wb");
        if (!f || fwrite(colour, 4, n, f) != n || fwrite(depth, 4, n, f) != n)
            return fail("write-out");
        fclose(f);
    }
    uint64_t ch = m3r_hash(colour, n * 4), dh = m3r_hash((const uint8_t *)depth, n * 4);
    uint64_t ech = m3r_hash(expected, n * 4), edh = m3r_hash((const uint8_t *)expected_depth, n * 4);
    // At 128x128 the oracle must be Codex's native reference.
    int oracle_ok = W != M3R_WIDTH || H != M3R_HEIGHT || mask || depth16 ||
                    (ech == UINT64_C(0xea42fa760955c9b5) && edh == UINT64_C(0x30603b84bfb46c25));
    int pass = !bad_c && !bad_d && oracle_ok;
    printf("G15_GL_DEPTH_RESULT %s colour_mismatches=%u depth_mismatches=%u near=%u far_only=%u overlap=%u "
           "colour_fnv=%016" PRIx64 " depth_fnv=%016" PRIx64 " expected=%016" PRIx64 "/%016" PRIx64 "\n",
           pass ? "PASS" : "FAIL", bad_c, bad_d, near_px, far_px, overlap, ch, dh, ech, edh);
    return pass ? 0 : 1;
}

static int fail(const char *what) {
    printf("G15_GL_RESULT FAIL mismatches=-1 red=-1 canary=-1 error=%s egl=0x%x gl=0x%x\n", what,
           eglGetError(), glGetError());
    return 1;
}

static PFNEGLQUERYDEVICESEXTPROC query_devices;
static PFNEGLQUERYDEVICESTRINGEXTPROC query_device_string;
static PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;

static int driver_matches(EGLDeviceEXT dev, const char *want) {
    const char *node = query_device_string(dev, EGL_DRM_RENDER_NODE_FILE_EXT);
    if (!strcmp(want, "software"))
        return !node && !query_device_string(dev, EGL_DRM_DEVICE_FILE_EXT);
    if (!node)
        return 0;
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return 0;
    drmVersionPtr v = drmGetVersion(fd);
    int ok = v && !strcmp(v->name, want);
    if (ok)
        fprintf(stderr, "using %s (%s)\n", node, v->name);
    drmFreeVersion(v);
    close(fd);
    return ok;
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        return 0;
    }
    return s;
}

struct external_dumb {
    int drm_fd, dma_fd;
    uint32_t handle, stride;
    uint64_t size;
    uint8_t *map;
};

static void external_pixel(uint8_t pixel[4], int x, int y) {
    pixel[0] = (uint8_t)(17 * x + 3 * y);
    pixel[1] = (uint8_t)(5 * x + 11 * y);
    pixel[2] = (uint8_t)(13 * (x ^ y));
    pixel[3] = 255;
}

static uint8_t external_nv12_y(int x, int y) {
    return (uint8_t)(48 + ((13 * x + 7 * y) % 160));
}

static void external_nv12_uv(uint8_t *u, uint8_t *v, int x, int y) {
    *u = (uint8_t)(64 + ((23 * x + 11 * y) % 128));
    *v = (uint8_t)(64 + ((7 * x + 31 * y + 37) % 128));
}

static uint8_t external_unorm8(float value) {
    if (value <= 0.0f)
        return 0;
    if (value >= 1.0f)
        return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

static void external_nv12_pixel(uint8_t pixel[4], int x, int y) {
    uint8_t u, v;
    external_nv12_uv(&u, &v, x / 2, y / 2);
    float yf = (external_nv12_y(x, y) - 16.0f) / 219.0f;
    float cb = (u - 128.0f) / 224.0f;
    float cr = (v - 128.0f) / 224.0f;
    pixel[0] = external_unorm8(yf + 1.402f * cr);
    pixel[1] = external_unorm8(yf - 0.3441363f * cb - 0.7141363f * cr);
    pixel[2] = external_unorm8(yf + 1.772f * cb);
    pixel[3] = 255;
}

static void external_dumb_close(struct external_dumb *image) {
    if (image->map && image->map != MAP_FAILED)
        munmap(image->map, image->size);
    if (image->dma_fd >= 0)
        close(image->dma_fd);
    if (image->drm_fd >= 0 && image->handle) {
        struct drm_mode_destroy_dumb destroy = {.handle = image->handle};
        drmIoctl(image->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    if (image->drm_fd >= 0)
        close(image->drm_fd);
}

static int external_dma_sync(int fd, uint64_t flags) {
    struct dma_buf_sync sync = {.flags = flags};
    int ret;
    do {
        ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

static int external_dumb_open(struct external_dumb *image, int width, int height,
                              int bpp) {
    *image = (struct external_dumb){.drm_fd = -1, .dma_fd = -1};
    const char *card = getenv("G15_DRM_CARD");
    char path[64];
    if (!card || !*card)
        card = "card1";
    if (card[0] == '/')
        snprintf(path, sizeof(path), "%s", card);
    else
        snprintf(path, sizeof(path), "/dev/dri/%s", card);
    image->drm_fd = open(path, O_RDWR | O_CLOEXEC);
    if (image->drm_fd < 0)
        return -1;

    struct drm_mode_create_dumb create = {
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .bpp = (uint32_t)bpp,
    };
    if (drmIoctl(image->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
        goto fail;
    image->handle = create.handle;
    image->stride = create.pitch;
    image->size = create.size;

    struct drm_prime_handle prime = {
        .handle = image->handle,
        .flags = DRM_CLOEXEC | DRM_RDWR,
    };
    if (drmIoctl(image->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0)
        goto fail;
    image->dma_fd = prime.fd;

    struct drm_mode_map_dumb map = {.handle = image->handle};
    if (drmIoctl(image->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
        goto fail;
    image->map = mmap(NULL, image->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      image->drm_fd, map.offset);
    if (image->map == MAP_FAILED)
        goto fail;
    return 0;

fail:
    external_dumb_close(image);
    return -1;
}

static const char *vs_external_src =
    "#version 300 es\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = gl_VertexID == 0 ? vec2(-1.0, -1.0)\n"
    "           : gl_VertexID == 1 ? vec2( 3.0, -1.0)\n"
    "                              : vec2(-1.0,  3.0);\n"
    "    uv = p * 0.5 + 0.5;\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char *fs_external_src =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "uniform samplerExternalOES source;\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "void main() { color = texture(source, uv); }\n";

static int run_external(int W, int H, const char *out, int nv12) {
    const char *egl_ext = eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS);
    const char *gl_ext = (const char *)glGetString(GL_EXTENSIONS);
    if (!egl_ext || !strstr(egl_ext, "EGL_EXT_image_dma_buf_import"))
        return fail("no-EGL_EXT_image_dma_buf_import");
    if (!gl_ext || !strstr(gl_ext, "GL_OES_EGL_image_external_essl3"))
        return fail("no-GL_OES_EGL_image_external_essl3");

    PFNEGLCREATEIMAGEKHRPROC create_image =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy_image =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!create_image || !destroy_image || !image_target)
        return fail("external-image-entrypoints");

    if (nv12 && (W < 2 || H < 2 || (W & 1) || (H & 1)))
        return fail("external-nv12-size");
    struct external_dumb source;
    int alloc_h = nv12 ? H + H / 2 : H;
    if (external_dumb_open(&source, W, alloc_h, nv12 ? 8 : 32) < 0)
        return fail("external-dumb-buffer");
    if (external_dma_sync(source.dma_fd,
                          DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) < 0) {
        external_dumb_close(&source);
        return fail("external-dma-sync-start");
    }
    if (nv12) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                source.map[(size_t)y * source.stride + x] =
                    external_nv12_y(x, y);
        for (int y = 0; y < H / 2; ++y)
            for (int x = 0; x < W / 2; ++x) {
                uint8_t *uv = source.map + (size_t)(H + y) * source.stride + 2 * x;
                external_nv12_uv(&uv[0], &uv[1], x, y);
            }
    } else {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                external_pixel(source.map + (size_t)y * source.stride + 4 * x,
                               x, y);
    }
    if (external_dma_sync(source.dma_fd,
                          DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) < 0) {
        external_dumb_close(&source);
        return fail("external-dma-sync-end");
    }

    EGLint rgba_attrs[] = {
        EGL_WIDTH, W, EGL_HEIGHT, H,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, source.dma_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)source.stride,
        EGL_NONE,
    };
    EGLint nv12_attrs[] = {
        EGL_WIDTH, W, EGL_HEIGHT, H,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12,
        EGL_DMA_BUF_PLANE0_FD_EXT, source.dma_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)source.stride,
        EGL_DMA_BUF_PLANE1_FD_EXT, source.dma_fd,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLint)((uint64_t)source.stride * H),
        EGL_DMA_BUF_PLANE1_PITCH_EXT, (EGLint)source.stride,
        EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC601_EXT,
        EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,
        EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_EXT,
        EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_EXT,
        EGL_NONE,
    };
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLImageKHR image = create_image(dpy, EGL_NO_CONTEXT,
                                     EGL_LINUX_DMA_BUF_EXT, NULL,
                                     nv12 ? nv12_attrs : rgba_attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        external_dumb_close(&source);
        return fail("eglCreateImageKHR-dma-buf");
    }

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_target(GL_TEXTURE_EXTERNAL_OES, image);

    GLuint rb, fbo;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        destroy_image(dpy, image);
        external_dumb_close(&source);
        return fail("external-fbo-incomplete");
    }

    GLuint prog = link_program(vs_external_src, fs_external_src);
    if (!prog) {
        destroy_image(dpy, image);
        external_dumb_close(&source);
        return fail("external-compile");
    }
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "source"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glDisable(GL_DITHER);
    glViewport(0, 0, W, H);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();

    size_t bytes = (size_t)W * H * 4;
    uint8_t *actual = malloc(bytes);
    if (!actual) {
        destroy_image(dpy, image);
        external_dumb_close(&source);
        return fail("external-malloc");
    }
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, actual);
    GLenum err = glGetError();
    unsigned direct = 0, flipped = 0;
    unsigned direct_channels = 0, flipped_channels = 0;
    unsigned direct_max = 0, flipped_max = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t expected[4], expected_flipped[4];
            if (nv12) {
                external_nv12_pixel(expected, x, y);
                external_nv12_pixel(expected_flipped, x, H - 1 - y);
            } else {
                external_pixel(expected, x, y);
                external_pixel(expected_flipped, x, H - 1 - y);
            }
            const uint8_t *pixel = actual + ((size_t)y * W + x) * 4;
            direct += memcmp(pixel, expected, 4) != 0;
            flipped += memcmp(pixel, expected_flipped, 4) != 0;
            for (unsigned c = 0; c < 4; ++c) {
                unsigned dd = abs((int)pixel[c] - expected[c]);
                unsigned fd = abs((int)pixel[c] - expected_flipped[c]);
                direct_channels += dd != 0;
                flipped_channels += fd != 0;
                direct_max = MAX2(direct_max, dd);
                flipped_max = MAX2(flipped_max, fd);
            }
        }
    if (out) {
        FILE *file = fopen(out, "wb");
        if (!file || fwrite(actual, 1, bytes, file) != bytes) {
            free(actual);
            destroy_image(dpy, image);
            external_dumb_close(&source);
            return fail("external-write-out");
        }
        fclose(file);
    }
    uint64_t hash = m3r_hash(actual, bytes);
    int pixels_ok = nv12 ? direct_max <= 1 : !direct;
    int pass = err == GL_NO_ERROR && pixels_ok;
    printf("G15_GL_EXTERNAL%s %s direct_mismatches=%u flipped_mismatches=%u "
           "direct_channels=%u flipped_channels=%u direct_max=%u "
           "flipped_max=%u pitch=%u tight=%u fnv1a64=%016" PRIx64 "\n",
           nv12 ? "_NV12" : "", pass ? "PASS" : "FAIL", direct, flipped,
           direct_channels, flipped_channels, direct_max, flipped_max,
           source.stride, nv12 ? (unsigned)W : (unsigned)W * 4, hash);

    free(actual);
    destroy_image(dpy, image);
    external_dumb_close(&source);
    return pass ? 0 : 1;
}

static int run_vertextex(int W, int H, const char *out) {
    GLuint rb, fbo;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, colour_format, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("fbo-incomplete");

    GLuint prog = link_program(vs_vertextex_src, fs_vertextex_src);
    if (!prog)
        return fail("compile");
    glUseProgram(prog);

    static const uint8_t texels[2][4] = {
        {255, 0, 64, 255},
        {0, 255, 128, 255},
    };
    GLuint tex[2];
    glGenTextures(2, tex);
    for (unsigned i = 0; i < 2; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, texels[i]);
    }
    glUniform1i(glGetUniformLocation(prog, "u_tex0"), 0);
    glUniform1i(glGetUniformLocation(prog, "u_tex1"), 1);
    glUniform4f(glGetUniformLocation(prog, "u_bias"), 0, 0, 0, 0);

    static const float positions[6] = {
        -0.75f, -0.75f, 0.765625f, -0.75f, -0.75f, 0.75f,
    };
    static const float zeros[3] = {0, 0, 0};
    GLuint buffers[4];
    glGenBuffers(4, buffers);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    for (unsigned i = 1; i < 4; ++i) {
        glBindBuffer(GL_ARRAY_BUFFER, buffers[i]);
        glBufferData(GL_ARRAY_BUFFER, sizeof(zeros), zeros, GL_STATIC_DRAW);
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, 1, GL_FLOAT, GL_FALSE, 0, 0);
    }

    glViewport(0, 0, M3R_WIDTH, M3R_HEIGHT);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    size_t bytes = (size_t)W * H * 4;
    uint8_t *actual = malloc(bytes), *expected = malloc(bytes);
    if (!actual || !expected)
        return fail("malloc");
    memset(actual, M3R_CANARY_ALPHA, bytes);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, actual);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
        return fail("readpixels");

    expected_image(W, H, 0, expected);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (!m3r_inside(x, y))
                continue;
            uint8_t *p = expected + ((size_t)y * W + x) * 4;
            if (x < 48)
                memcpy(p, texels[0], 4);
            else if (x < 80)
                memcpy(p, texels[1], 4);
            else if (x < 96) {
                p[0] = 255, p[1] = 0, p[2] = 0, p[3] = 255;
            } else {
                p[0] = 64, p[1] = 128, p[2] = 255, p[3] = 255;
            }
        }
    }
    if (out) {
        FILE *f = fopen(out, "wb");
        if (!f || fwrite(actual, 1, bytes, f) != bytes)
            return fail("write-out");
        fclose(f);
    }
    struct m3r_check c = check_image(actual, expected, bytes);
    printf("G15_GL_VERTEXTEX_RESULT %s mismatches=%u canary=%u "
           "fnv1a64=%016" PRIx64 " expected=%016" PRIx64 "\n",
           !c.mismatched_pixels ? "PASS" : "FAIL", c.mismatched_pixels,
           c.canary_pixels, c.actual_hash, c.expected_hash);
    return c.mismatched_pixels ? 1 : 0;
}


// --mode stress[depth][var|var1|var4|flat1|flat4|linear1|linear4]: SError hunt.
// A full-viewport grid of --tris
// triangles (two per cell) drawn --repeat frames in one context, each frame
// cleared and finished; "depth" adds a D32F buffer with LESS testing and a
// per-frame depth slope. The varying suffixes select one or four smooth
// perspective, flat, or screen-linear components; "var" aliases "var4".
// No image check: it reports frames completed and GL errors. --gap-ms N draws
// each frame in 128-triangle renders (the shape of Mesa's batch split) and
// idles N ms after each one: a power/duty-cycle versus per-render discriminator.
static const char *vs_stress_src =
    "#version 300 es\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "out vec4 v_col;\n"
    "void main() { v_col = vec4(a_pos.xy * 0.5 + 0.5, 1.0 - a_pos.z, 1.0); gl_Position = vec4(a_pos, 1.0); }\n";
static const char *fs_stress_src =
    "#version 300 es\nprecision mediump float;\n"
    "uniform vec4 u_color;\nout vec4 o;\nvoid main() { o = u_color; }\n";
static const char *fs_stress_var_src =
    "#version 300 es\nprecision mediump float;\n"
    "in vec4 v_col;\nuniform vec4 u_color;\nout vec4 o;\nvoid main() { o = v_col * u_color; }\n";

static const char *vs_stress_var1_src =
    "#version 300 es\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "out float v_value;\n"
    "void main() { v_value = a_pos.x * 0.25 + 0.5; gl_Position = vec4(a_pos, 1.0); }\n";
static const char *fs_stress_var1_src =
    "#version 300 es\nprecision mediump float;\n"
    "in float v_value;\nuniform vec4 u_color;\nout vec4 o;\n"
    "void main() { o = vec4(v_value, v_value, v_value, 1.0) * u_color; }\n";

static const char *vs_stress_flat4_src =
    "#version 300 es\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "flat out vec4 v_col;\n"
    "void main() { v_col = vec4(a_pos.xy * 0.5 + 0.5, 1.0 - a_pos.z, 1.0); gl_Position = vec4(a_pos, 1.0); }\n";
static const char *fs_stress_flat4_src =
    "#version 300 es\nprecision mediump float;\n"
    "flat in vec4 v_col;\nuniform vec4 u_color;\nout vec4 o;\nvoid main() { o = v_col * u_color; }\n";
static const char *vs_stress_flat1_src =
    "#version 300 es\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "flat out float v_value;\n"
    "void main() { v_value = a_pos.x * 0.25 + 0.5; gl_Position = vec4(a_pos, 1.0); }\n";
static const char *fs_stress_flat1_src =
    "#version 300 es\nprecision mediump float;\n"
    "flat in float v_value;\nuniform vec4 u_color;\nout vec4 o;\n"
    "void main() { o = vec4(v_value, v_value, v_value, 1.0) * u_color; }\n";

static const char *vs_stress_linear4_src =
    "#version 300 es\n"
    "#extension GL_NV_shader_noperspective_interpolation : require\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "noperspective out vec4 v_col;\n"
    "void main() { v_col = vec4(a_pos.xy * 0.5 + 0.5, 1.0 - a_pos.z, 1.0); gl_Position = vec4(a_pos, 1.0); }\n";
static const char *fs_stress_linear4_src =
    "#version 300 es\n"
    "#extension GL_NV_shader_noperspective_interpolation : require\n"
    "precision mediump float;\n"
    "noperspective in vec4 v_col;\nuniform vec4 u_color;\nout vec4 o;\nvoid main() { o = v_col * u_color; }\n";
static const char *vs_stress_linear1_src =
    "#version 300 es\n"
    "#extension GL_NV_shader_noperspective_interpolation : require\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "noperspective out float v_value;\n"
    "void main() { v_value = a_pos.x * 0.25 + 0.5; gl_Position = vec4(a_pos, 1.0); }\n";
static const char *fs_stress_linear1_src =
    "#version 300 es\n"
    "#extension GL_NV_shader_noperspective_interpolation : require\n"
    "precision mediump float;\n"
    "noperspective in float v_value;\nuniform vec4 u_color;\nout vec4 o;\n"
    "void main() { o = vec4(v_value, v_value, v_value, 1.0) * u_color; }\n";

static void stress_shaders(const char *mode, const char **vs, const char **fs) {
    *vs = vs_stress_src;
    *fs = fs_stress_src;
    if (strstr(mode, "linear1")) {
        *vs = vs_stress_linear1_src;
        *fs = fs_stress_linear1_src;
    } else if (strstr(mode, "linear4")) {
        *vs = vs_stress_linear4_src;
        *fs = fs_stress_linear4_src;
    } else if (strstr(mode, "flat1")) {
        *vs = vs_stress_flat1_src;
        *fs = fs_stress_flat1_src;
    } else if (strstr(mode, "flat4")) {
        *vs = vs_stress_flat4_src;
        *fs = fs_stress_flat4_src;
    } else if (strstr(mode, "var1")) {
        *vs = vs_stress_var1_src;
        *fs = fs_stress_var1_src;
    } else if (strstr(mode, "var")) {
        *fs = fs_stress_var_src;
    }
}

static long gap_ms = -1; /* -1: unchunked (Mesa splits); 0..60000: chunked */

static int run_stress(const char *mode, int W, int H, long repeat, long tris) {
    int depth = strstr(mode, "depth") != NULL;
    GLuint rb[2], fbo;
    glGenRenderbuffers(2, rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorage(GL_RENDERBUFFER, colour_format, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    if (depth) {
        glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, W, H);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("fbo-incomplete");
    const char *vs, *fs;
    stress_shaders(mode, &vs, &fs);
    GLuint prog = link_program(vs, fs);
    if (!prog)
        return fail("compile");
    long cells = (tris + 1) / 2, side = 1;
    while (side * side < cells)
        side++;
    float *v = malloc(sizeof(float) * 18 * side * side);
    if (!v)
        return fail("malloc");
    long nv = 0;
    for (long y = 0; y < side; ++y)
        for (long x = 0; x < side; ++x) {
            float x0 = 2.0f * x / side - 1, x1 = 2.0f * (x + 1) / side - 1;
            float y0 = 2.0f * y / side - 1, y1 = 2.0f * (y + 1) / side - 1;
            float z = (float)((x * 7 + y * 13) % 17) / 17.0f;
            float q[18] = {x0, y0, z, x1, y0, z, x0, y1, z, x1, y0, z, x1, y1, z, x0, y1, z};
            memcpy(v + nv * 3, q, sizeof q);
            nv += 6;
        }
    GLuint buf;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 3 * nv, v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glViewport(0, 0, W, H);
    glUseProgram(prog);
    GLint col = glGetUniformLocation(prog, "u_color");
    if (depth) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glClearDepthf(1.0f);
    }
    // G15_STRESS_CULL=all: cull every triangle (renders whose geometry the
    // TA discards entirely), =back: cull back faces only.
    const char *cull = getenv("G15_STRESS_CULL");
    if (cull && !strcmp(cull, "all")) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT_AND_BACK);
    } else if (cull && !strcmp(cull, "back")) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }
    long count = tris < nv / 3 ? tris : nv / 3;
    for (long f = 0; f < repeat; ++f) {
        glClearColor(0.0f, 0.0f, (f & 1) ? 1.0f : 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT : 0));
        glUniform4f(col, 1.0f, (float)(f % 256) / 255.0f, 0.5f, 1.0f);
        if (gap_ms >= 0) {
            for (long t = 0; t < count; t += 128) {
                glDrawArrays(GL_TRIANGLES, (GLint)(t * 3), (GLsizei)((count - t < 128 ? count - t : 128) * 3));
                glFinish();
                if (gap_ms)
                    usleep(gap_ms * 1000);
            }
        } else {
            // G15_STRESS_LAYERS=L: the same grid L times in one render
            // (overlapping primitives; with depth, hidden-surface work).
            const char *ls = getenv("G15_STRESS_LAYERS");
            long layers = ls ? atol(ls) : 1;
            for (long l = 0; l < (layers > 0 ? layers : 1); ++l)
                glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(count * 3));
            glFinish();
        }
        if (f % 500 == 499)
            fprintf(stderr, "stress frames=%ld\n", f + 1);
    }
    GLenum err = glGetError();
    printf("G15_GL_STRESS %s mode=%s frames=%ld tris=%ld size=%dx%d gap_ms=%ld chunks_per_frame=%ld gl_error=%#x\n",
           err == GL_NO_ERROR ? "DONE" : "GLERROR", mode, repeat, count, W, H, gap_ms,
           gap_ms >= 0 ? (count + 127) / 128 : 0, err);
    return err == GL_NO_ERROR ? 0 : 1;
}

// --mode scissors --tris N: one frame of N full-viewport draws, each with
// its own 1x1 scissor at (i % W, i / W), after a black clear; reports how
// many pixels came out white (expected N).
static int run_scissors(int W, int H, long n, int use_scissor,
                        const char *post, int trace_post) {
    GLuint rb, fbo;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, colour_format, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("fbo-incomplete");
    GLuint prog = link_program(vs_stress_src, fs_stress_src);
    if (!prog)
        return fail("compile");
    static const float q[18] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    GLuint buf;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glViewport(0, 0, W, H);
    glUseProgram(prog);
    glUniform4f(glGetUniformLocation(prog, "u_color"), 1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (use_scissor)
        glEnable(GL_SCISSOR_TEST);
    for (long i = 0; i < n; ++i) {
        glScissor((GLint)(i % W), (GLint)(i / W), 1, 1);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glDisable(GL_SCISSOR_TEST);
    if (trace_post)
        fprintf(stderr,
                "G15_GL_SCISSORS_PHASE pid=%ld draws=%ld post=%s phase=primary-finish-begin\n",
                (long)getpid(), n, post);
    glFinish();
    if (trace_post)
        fprintf(stderr,
                "G15_GL_SCISSORS_PHASE pid=%ld draws=%ld post=%s phase=primary-finish-end\n",
                (long)getpid(), n, post);

    if (!strcmp(post, "finish")) {
        GLenum err = glGetError();
        printf("G15_GL_SCISSORS %s draws=%ld scissor=%d white=-1 size=%dx%d gl_error=%#x post=%s\n",
               err == GL_NO_ERROR ? "PASS" : "FAIL", n, use_scissor, W, H,
               err, post);
        return err == GL_NO_ERROR ? 0 : 1;
    }

    long want = use_scissor ? n : (long)W * H;
    if (!strcmp(post, "followup")) {
        if (trace_post)
            fprintf(stderr,
                    "G15_GL_SCISSORS_PHASE pid=%ld draws=%ld post=%s phase=followup-draw-begin\n",
                    (long)getpid(), n, post);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glFinish();
        if (trace_post)
            fprintf(stderr,
                    "G15_GL_SCISSORS_PHASE pid=%ld draws=%ld post=%s phase=followup-draw-end\n",
                    (long)getpid(), n, post);
        want = (long)W * H;
    }

    unsigned char *px = malloc((size_t)W * H * 4);
    if (!px)
        return fail("malloc");
    if (trace_post)
        fprintf(stderr,
                "G15_GL_SCISSORS_PHASE pid=%ld draws=%ld post=%s phase=readback-begin\n",
                (long)getpid(), n, post);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (trace_post)
        fprintf(stderr,
                "G15_GL_SCISSORS_PHASE pid=%ld draws=%ld post=%s phase=readback-end\n",
                (long)getpid(), n, post);
    long white = 0;
    for (long i = 0; i < (long)W * H; ++i)
        white += px[i * 4] == 255;
    free(px);
    GLenum err = glGetError();
    if (trace_post)
        printf("G15_GL_SCISSORS %s draws=%ld scissor=%d white=%ld size=%dx%d gl_error=%#x post=%s\n",
               white == want && err == GL_NO_ERROR ? "PASS" : "FAIL", n,
               use_scissor, white, W, H, err, post);
    else
        printf("G15_GL_SCISSORS %s draws=%ld scissor=%d white=%ld size=%dx%d gl_error=%#x\n",
               white == want && err == GL_NO_ERROR ? "PASS" : "FAIL", n,
               use_scissor, white, W, H, err);
    return white == want && err == GL_NO_ERROR ? 0 : 1;
}

// --mode multitex[shader] --tris N: one render of N side-by-side vertical
// strips, each drawn with its own 1x1 texture (colour i), as a compositor
// draws one textured surface per draw. "shader" alternates two programs.
// Checks every strip's centre pixel.
static const char *vs_tex_src =
    "#version 300 es\nlayout(location = 0) in vec3 a_pos;\nout vec2 v_uv;\n"
    "void main() { v_uv = a_pos.xy * 0.5 + 0.5; gl_Position = vec4(a_pos, 1.0); }\n";
static const char *fs_tex_src =
    "#version 300 es\nprecision mediump float;\nuniform sampler2D u_tex;\nin vec2 v_uv;\nout vec4 o;\n"
    "void main() { o = texture(u_tex, v_uv); }\n";
static const char *fs_tex2_src =
    "#version 300 es\nprecision mediump float;\nuniform sampler2D u_tex;\nin vec2 v_uv;\nout vec4 o;\n"
    "void main() { o = texture(u_tex, v_uv).rgba * 1.0; }\n";
static int run_multitex(const char *mode, int W, int H, long n) {
    int two = strstr(mode, "shader") != NULL;
    GLuint rb, fbo;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, colour_format, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("fbo-incomplete");
    GLuint prog[2] = {link_program(vs_tex_src, fs_tex_src), link_program(vs_tex_src, fs_tex2_src)};
    if (!prog[0] || !prog[1])
        return fail("compile");
    if (n < 1 || n > 64)
        return fail("n-range");
    GLuint tex[64];
    unsigned char col[64][4];
    glGenTextures((GLsizei)n, tex);
    for (long i = 0; i < n; ++i) {
        col[i][0] = (unsigned char)(40 + 3 * i); col[i][1] = (unsigned char)(200 - 2 * i);
        col[i][2] = (unsigned char)(17 * i); col[i][3] = 255;
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, col[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    float *v = malloc(sizeof(float) * 18 * n);
    if (!v)
        return fail("malloc");
    for (long i = 0; i < n; ++i) {
        float x0 = 2.0f * i / n - 1, x1 = 2.0f * (i + 1) / n - 1;
        float q[18] = {x0, -1, 0, x1, -1, 0, x0, 1, 0, x1, -1, 0, x1, 1, 0, x0, 1, 0};
        memcpy(v + 18 * i, q, sizeof q);
    }
    GLuint buf;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 18 * n, v, GL_STATIC_DRAW);
    free(v);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    for (long i = 0; i < n; ++i) {
        GLuint p = prog[two ? (i & 1) : 0];
        glUseProgram(p);
        glUniform1i(glGetUniformLocation(p, "u_tex"), 0);
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glDrawArrays(GL_TRIANGLES, (GLint)(6 * i), 6);
    }
    glFinish();
    long bad = 0;
    for (long i = 0; i < n; ++i) {
        unsigned char px[4];
        glReadPixels((GLint)((2 * i + 1) * W / (2 * n)), H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (memcmp(px, col[i], 3)) {
            if (bad < 4)
                fprintf(stderr, "strip %ld: got %u,%u,%u want %u,%u,%u\n", i, px[0], px[1], px[2], col[i][0], col[i][1], col[i][2]);
            bad++;
        }
    }
    GLenum err = glGetError();
    printf("G15_GL_MULTITEX %s mode=%s draws=%ld bad=%ld size=%dx%d gl_error=%#x\n",
           !bad && err == GL_NO_ERROR ? "PASS" : "FAIL", mode, n, bad, W, H, err);
    return !bad && err == GL_NO_ERROR ? 0 : 1;
}

// --mode stencil --tris N: stencil kept across renders. Clear stencil to
// 0x5a, then N nested pixel-aligned rectangles, each its own draw with
// stencil INCR and colour writes off (Mesa puts each draw in its own render,
// ASAHI_G15_BATCH_DRAWS=1), then N+1 full-target draws with stencil EQUAL
// 0x5a+k painting colour k. The colour image encodes the final stencil and is
// compared with the CPU oracle (pixels covered by k+1 rectangles get colour
// k+1). With stencil only in tile memory a split loses the increments.
static int run_stencil(int W, int H, long n) {
    if (n < 1 || n > 64 || W < 2 * (n + 1) || H < 2 * (n + 1))
        return fail("n-range");
    GLuint rb[2], fbo;
    glGenRenderbuffers(2, rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorage(GL_RENDERBUFFER, colour_format, W, H);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("fbo-incomplete");
    GLuint prog = link_program(vs_stress_src, fs_stress_src);
    if (!prog)
        return fail("compile");
    // Rectangle i covers pixels [x0, W - x0) x [y0, H - y0), x0 = i*W/(2n+2).
    float *v = malloc(sizeof(float) * 18 * (n + 1));
    if (!v)
        return fail("malloc");
    for (long i = 0; i <= n; ++i) {
        // i == n is the full-target quad for the colour passes.
        long px0 = i < n ? i * W / (2 * n + 2) : 0, py0 = i < n ? i * H / (2 * n + 2) : 0;
        float x0 = 2.0f * px0 / W - 1, x1 = 1 - 2.0f * px0 / W;
        float y0 = 2.0f * py0 / H - 1, y1 = 1 - 2.0f * py0 / H;
        float q[18] = {x0, y0, 0, x1, y0, 0, x0, y1, 0, x1, y0, 0, x1, y1, 0, x0, y1, 0};
        memcpy(v + 18 * i, q, sizeof q);
    }
    GLuint buf;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 18 * (n + 1), v, GL_STATIC_DRAW);
    free(v);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glViewport(0, 0, W, H);
    glUseProgram(prog);
    GLint col = glGetUniformLocation(prog, "u_color");
    glClearColor(0, 0, 0, 1);
    glClearStencil(0x5a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_ALWAYS, 0, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    for (long i = 0; i < n; ++i)
        glDrawArrays(GL_TRIANGLES, (GLint)(6 * i), 6);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    for (long k = 0; k <= n; ++k) {
        glStencilFunc(GL_EQUAL, (GLint)(0x5a + k), 0xff);
        glUniform4f(col, (float)((k * 37) % 256) / 255.0f, (float)((k * 91 + 40) % 256) / 255.0f,
                    (float)((255 + 256 * 64 - k * 23) % 256) / 255.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, (GLint)(6 * n), 6);
    }
    glDisable(GL_STENCIL_TEST);
    glFinish();
    unsigned char *px = malloc((size_t)W * H * 4);
    if (!px)
        return fail("malloc");
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    long bad = 0;
    for (long y = 0; y < H; ++y)
        for (long x = 0; x < W; ++x) {
            long k = 0;
            for (long i = 0; i < n; ++i) {
                long x0 = i * W / (2 * n + 2), y0 = i * H / (2 * n + 2);
                k += x >= x0 && x < W - x0 && y >= y0 && y < H - y0;
            }
            unsigned char want[3] = {(unsigned char)((k * 37) % 256), (unsigned char)((k * 91 + 40) % 256),
                                     (unsigned char)((255 + 256 * 64 - k * 23) % 256)};
            const unsigned char *p = px + 4 * (y * W + x);
            if (memcmp(p, want, 3)) {
                if (bad < 4)
                    fprintf(stderr, "(%ld,%ld): got %u,%u,%u want k=%ld %u,%u,%u\n", x, y, p[0], p[1], p[2], k,
                            want[0], want[1], want[2]);
                bad++;
            }
        }
    uint64_t h = m3r_hash(px, (size_t)W * H * 4);
    free(px);
    GLenum err = glGetError();
    printf("G15_GL_STENCIL %s rects=%ld mismatches=%ld size=%dx%d fnv1a64=%016" PRIx64 " gl_error=%#x\n",
           !bad && err == GL_NO_ERROR ? "PASS" : "FAIL", n, bad, W, H, h, err);
    return !bad && err == GL_NO_ERROR ? 0 : 1;
}

// --mode mips: render into every mip level of a WxH RGBA8 texture
// (glTexStorage2D, full chain): level l is cleared to colour A(l), then its
// left half (pixels x < w/2) drawn in colour B(l). All levels
// are read back at the end and compared with that oracle, so a store into the
// wrong level or past a level's edge shows up.
static void mip_colour(int l, int b, unsigned char c[4]) {
    c[0] = (unsigned char)(30 + 20 * l + (b ? 120 : 0));
    c[1] = (unsigned char)(200 - 15 * l);
    c[2] = (unsigned char)(b ? 240 - 9 * l : 10 + 9 * l);
    c[3] = 255;
}
static int run_mips(int W, int H) {
    int levels = 1;
    while ((W >> levels) || (H >> levels))
        levels++;
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    GLuint prog = link_program(vs_stress_src, fs_stress_src);
    if (!prog)
        return fail("compile");
    static const float q[18] = {-1, -1, 0, 0, -1, 0, -1, 1, 0, 0, -1, 0, 0, 1, 0, -1, 1, 0};
    GLuint buf;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glUseProgram(prog);
    GLint col = glGetUniformLocation(prog, "u_color");
    for (int l = 0; l < levels; ++l) {
        int lw = W >> l ? W >> l : 1, lh = H >> l ? H >> l : 1;
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, l);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return fail("fbo-incomplete");
        unsigned char a[4], b[4];
        mip_colour(l, 0, a);
        mip_colour(l, 1, b);
        glViewport(0, 0, lw, lh);
        glClearColor(a[0] / 255.0f, a[1] / 255.0f, a[2] / 255.0f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        // The quad covers NDC x in [-1, 0): pixels x < lw/2 (none when lw is 1).
        glUniform4f(col, b[0] / 255.0f, b[1] / 255.0f, b[2] / 255.0f, 1);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glFlush();
    }
    glFinish();
    long bad = 0, badlevels = 0;
    unsigned char *px = malloc((size_t)W * H * 4);
    if (!px)
        return fail("malloc");
    uint64_t h = 0xcbf29ce484222325ull;
    for (int l = 0; l < levels; ++l) {
        int lw = W >> l ? W >> l : 1, lh = H >> l ? H >> l : 1;
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, l);
        glReadPixels(0, 0, lw, lh, GL_RGBA, GL_UNSIGNED_BYTE, px);
        h ^= m3r_hash(px, (size_t)lw * lh * 4) + (uint64_t)l;
        unsigned char a[4], b[4];
        mip_colour(l, 0, a);
        mip_colour(l, 1, b);
        long lb = 0;
        for (int y = 0; y < lh; ++y)
            for (int x = 0; x < lw; ++x) {
                const unsigned char *want = x < lw / 2 ? b : a, *p = px + 4 * ((size_t)y * lw + x);
                if (memcmp(p, want, 4)) {
                    if (lb < 2)
                        fprintf(stderr, "level %d (%d,%d) of %dx%d: got %u,%u,%u,%u want %u,%u,%u,%u\n", l, x, y,
                                lw, lh, p[0], p[1], p[2], p[3], want[0], want[1], want[2], want[3]);
                    lb++;
                }
            }
        bad += lb;
        badlevels += lb != 0;
    }
    free(px);
    GLenum err = glGetError();
    printf("G15_GL_MIPS %s levels=%d bad_levels=%ld mismatches=%ld size=%dx%d hash=%016" PRIx64 " gl_error=%#x\n",
           !bad && err == GL_NO_ERROR ? "PASS" : "FAIL", levels, badlevels, bad, W, H, h, err);
    return !bad && err == GL_NO_ERROR ? 0 : 1;
}

// --mode depthonly: a shadow-map-shaped pass. Pass 1 has only a depth
// attachment (cleared to 1.0): the near triangle at z 0.25 is drawn into it.
// Pass 2 binds a colour target with the same depth (depth loaded, colour
// cleared blue) and draws a full-target quad at z 0.5 in green with LESS, so
// green lands everywhere except where pass 1 wrote depth. Compared with the
// CPU oracle (the triangle covers the native near triangle's pixels).
static int run_depthonly(int W, int H) {
    GLuint rb[2], fbo[2];
    glGenRenderbuffers(2, rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorage(GL_RENDERBUFFER, colour_format, W, H);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorage(GL_RENDERBUFFER, depth16 ? GL_DEPTH_COMPONENT16 : GL_DEPTH_COMPONENT32F, W, H);
    glGenFramebuffers(2, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("fbo0-incomplete");
    GLuint prog = link_program(vs_depth_src, fs_depth_src);
    if (!prog)
        return fail("compile");
    GLuint buf;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glViewport(0, 0, M3R_WIDTH, M3R_HEIGHT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glClearDepthf(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    draw_depth_tri(prog, near_tri, 1.0f, 0.0f);
    glFlush();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("fbo1-incomplete");
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    static const float quad[18] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glUseProgram(prog);
    glUniform4f(glGetUniformLocation(prog, "u_color"), 0.0f, 1.0f, 0.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();
    unsigned char *px = malloc((size_t)W * H * 4);
    if (!px)
        return fail("malloc");
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    static const int near_v[6] = {16, 16, 113, 16, 16, 112};
    long bad = 0, blue = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int in = inside_tri(near_v, x, y);
            static const unsigned char b[4] = {0, 0, 255, 255}, g[4] = {0, 255, 0, 255};
            const unsigned char *p = px + 4 * ((size_t)y * W + x);
            blue += in;
            if (memcmp(p, in ? b : g, 4)) {
                if (bad < 3)
                    fprintf(stderr, "(%d,%d): got %u,%u,%u,%u want %s\n", x, y, p[0], p[1], p[2], p[3],
                            in ? "blue" : "green");
                bad++;
            }
        }
    uint64_t h = m3r_hash(px, (size_t)W * H * 4);
    free(px);
    GLenum err = glGetError();
    printf("G15_GL_DEPTHONLY %s mismatches=%ld blue=%ld size=%dx%d depth16=%d fnv1a64=%016" PRIx64 " gl_error=%#x\n",
           !bad && err == GL_NO_ERROR ? "PASS" : "FAIL", bad, blue, W, H, depth16, h, err);
    return !bad && err == GL_NO_ERROR ? 0 : 1;
}

// --mode overdraw --tris N --repeat R: one draw of N full-target triangles
// stacked on every tile (the tile-list growth shape), additive blend off,
// the last one green; R frames, each glFinish'ed. Checks the image is green.
static long overdraw_draws = 1;
// --overdraw-fs discard: every fragment is discarded (a uniform-dependent
// discard the compiler cannot fold), so tiles get the same long lists but no
// pixel is written; the image must stay the red clear (G2 end-of-tile test).
static int overdraw_discard;
static const char *fs_overdraw_discard_src =
    "#version 300 es\nprecision mediump float;\n"
    "uniform vec4 u_color;\nout vec4 o;\nvoid main() { if (u_color.a > 0.0) discard; o = u_color; }\n";

static int run_overdraw(int W, int H, long n, long repeat) {
    GLuint rb, fbo;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, colour_format, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("fbo-incomplete");
    GLuint prog = link_program(vs_stress_src, overdraw_discard ? fs_overdraw_discard_src : fs_stress_src);
    if (!prog)
        return fail("compile");
    // Each triangle covers the whole target: (-1,-1), (3,-1), (-1,3).
    float *v = malloc(sizeof(float) * 9 * n);
    if (!v)
        return fail("malloc");
    for (long i = 0; i < n; ++i) {
        float t[9] = {-1, -1, 0, 3, -1, 0, -1, 3, 0};
        memcpy(v + 9 * i, t, sizeof t);
    }
    GLuint buf;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 9 * n, v, GL_STATIC_DRAW);
    free(v);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glViewport(0, 0, W, H);
    glUseProgram(prog);
    glUniform4f(glGetUniformLocation(prog, "u_color"), 0, 1, 0, 1);
    for (long f = 0; f < repeat; ++f) {
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        // --draws D: the same triangles as D consecutive draws.
        for (long d = 0, at = 0; d < overdraw_draws; ++d) {
            long next = n * (d + 1) / overdraw_draws;
            if (next > at)
                glDrawArrays(GL_TRIANGLES, (GLint)(3 * at), (GLsizei)(3 * (next - at)));
            at = next;
        }
        glFinish();
    }
    unsigned char px[4] = {0};
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLenum err = glGetError();
    int ok = (overdraw_discard ? px[0] == 255 && px[1] == 0 : px[0] == 0 && px[1] == 255) && err == GL_NO_ERROR;
    printf("G15_GL_OVERDRAW %s tris=%ld draws=%ld frames=%ld size=%dx%d discard=%d centre=%u,%u,%u gl_error=%#x\n",
           ok ? "PASS" : "FAIL", n, overdraw_draws, repeat, W, H, overdraw_discard, px[0], px[1], px[2], err);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *want = "asahi", *out = NULL, *mode = "const", *post = "readback";
    int W = M3R_WIDTH, H = M3R_HEIGHT;
    int trace_post = 0;
    long repeat = 1, tris = 1000;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--driver") && i + 1 < argc)
            want = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc)
            mode = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "--post") && i + 1 < argc &&
                 (!strcmp(argv[i + 1], "finish") ||
                  !strcmp(argv[i + 1], "readback") ||
                  !strcmp(argv[i + 1], "followup")))
            post = argv[++i], trace_post = 1;
        else if (!strcmp(argv[i], "--format") && i + 1 < argc && !strcmp(argv[i + 1], "bgra"))
            colour_format = 0x93A1 /* GL_BGRA8_EXT */, i++;
        else if (!strcmp(argv[i], "--format") && i + 1 < argc && !strcmp(argv[i + 1], "rgba"))
            colour_format = GL_RGBA8, i++;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc && sscanf(argv[++i], "%dx%d", &W, &H) == 2 &&
                 W >= 1 && W <= 16384 && H >= 1 && H <= 16384)
            ;
        else if (!strcmp(argv[i], "--draws") && i + 1 < argc && (overdraw_draws = atol(argv[++i])) >= 1)
            ;
        else if (!strcmp(argv[i], "--tris") && i + 1 < argc && (tris = atol(argv[++i])) >= 1)
            ;
        else if (!strcmp(argv[i], "--overdraw-fs") && i + 1 < argc && !strcmp(argv[i + 1], "discard"))
            overdraw_discard = 1, i++;
        else if (!strcmp(argv[i], "--depth16"))
            depth16 = 1;
        else if (!strcmp(argv[i], "--gap-ms") && i + 1 < argc && (gap_ms = atol(argv[++i])) >= 0 &&
                 gap_ms <= 60000)
            ;
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) {
            char *end = NULL;
            repeat = strtol(argv[++i], &end, 10);
            if (!end || *end || repeat < 1 || repeat > 1000000)
                return fail("invalid-repeat");
        }
        else {
            fprintf(stderr, "usage: %s [--driver NAME|software] [--mode const|loadbench|varying|gradient|linear|discard|external|externalnv12|vertextex|vbo|split|first|fan|clear|green|reload|greenreload|depth|depthload|depthexport|depthmask|stencil|mips|depthonly|overdraw|stress[depth][var|var1|var4|flat1|flat4|linear1|linear4]] [--size WxH] [--repeat N] [--tris N] [--draws D] [--gap-ms N] [--format rgba|bgra] [--post finish|readback|followup] [--out FILE]\n",
                    argv[0]);
            return 2;
        }
    }
    const size_t bytes = (size_t)W * H * 4;
    uint8_t *between = malloc(bytes), *actual = malloc(bytes), *expected = malloc(bytes);
    if (!between || !actual || !expected)
        return fail("malloc");

    query_devices = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
    query_device_string = (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
    get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!query_devices || !query_device_string || !get_platform_display)
        return fail("no-device-enumeration");
    EGLDeviceEXT devs[16];
    EGLint ndev = 0;
    if (!query_devices(16, devs, &ndev))
        return fail("eglQueryDevicesEXT");
    EGLDeviceEXT dev = EGL_NO_DEVICE_EXT;
    for (EGLint i = 0; i < ndev && dev == EGL_NO_DEVICE_EXT; ++i)
        if (driver_matches(devs[i], want))
            dev = devs[i];
    if (dev == EGL_NO_DEVICE_EXT)
        return fail("no-matching-device");

    EGLDisplay dpy = get_platform_display(EGL_PLATFORM_DEVICE_EXT, dev, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL))
        return fail("eglInitialize");
    fprintf(stderr, "EGL %s\n", eglQueryString(dpy, EGL_VENDOR));
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        return fail("eglBindAPI");
    static const EGLint ctx_attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        return fail("context");
    fprintf(stderr, "GL %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    if (!strncmp(mode, "depth", 5) && strcmp(mode, "depthonly"))
        return run_depth(mode, W, H, out);
    if (!strncmp(mode, "stress", 6))
        return run_stress(mode, W, H, repeat, tris);
    if (!strncmp(mode, "multitex", 8))
        return run_multitex(mode, W, H, tris);
    if (!strcmp(mode, "vertextex"))
        return run_vertextex(W, H, out);
    if (!strcmp(mode, "external") || !strcmp(mode, "externalnv12"))
        return run_external(W, H, out, !strcmp(mode, "externalnv12"));
    if (!strcmp(mode, "scissors") || !strcmp(mode, "scissorsoff"))
        return run_scissors(W, H, tris, !strcmp(mode, "scissors"), post,
                            trace_post);
    if (trace_post)
        return fail("post-mode");
    if (!strcmp(mode, "stencil"))
        return run_stencil(W, H, tris);
    if (!strcmp(mode, "mips"))
        return run_mips(W, H);
    if (!strcmp(mode, "depthonly"))
        return run_depthonly(W, H);
    if (!strcmp(mode, "overdraw"))
        return run_overdraw(W, H, tris, repeat);

    GLuint rb, fbo;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, colour_format, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("fbo-incomplete");

    int varying = !strcmp(mode, "varying"), vbo = !strcmp(mode, "vbo");
    int gradient = !strcmp(mode, "gradient"), linear = !strcmp(mode, "linear");
    int discard = !strcmp(mode, "discard"), split = !strcmp(mode, "split");
    int clear_only = !strcmp(mode, "clear"), fan = !strcmp(mode, "fan");
    int loadbench = !strcmp(mode, "loadbench");
    int reload = !strcmp(mode, "reload") || !strcmp(mode, "greenreload");
    int green = !strcmp(mode, "green") || !strcmp(mode, "greenreload");
    int first = !strcmp(mode, "first");
    if (repeat > 1 && strcmp(mode, "const") && strcmp(mode, "clear") && !loadbench)
        return fail("repeat-mode");
    split |= reload;
    split |= first;
    vbo |= split | fan;
    if (!varying && !vbo && !gradient && !linear && !discard && !clear_only &&
        !green && !first && !reload && !loadbench && strcmp(mode, "const"))
        return fail("unknown-mode");
    GLuint vs = compile(GL_VERTEX_SHADER, varying    ? vs_varying_src
                                          : vbo      ? vs_vbo_src
                                          : linear   ? vs_linear_src
                                          : gradient ? vs_gradient_src
                                                     : vs_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, varying    ? fs_varying_src
                                            : linear   ? fs_linear_src
                                            : discard  ? fs_discard_src
                                            : gradient ? fs_gradient_src
                                                       : fs_src);
    if (!vs || !fs)
        return fail("compile");
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked)
        return fail("link");

    glViewport(0, 0, M3R_WIDTH, M3R_HEIGHT);
    if (green)
        glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    else
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glUseProgram(prog);
    if (discard) {
        static const uint8_t texels[8] = {
            255, 255, 255, 0, 255, 255, 255, 255,
        };
        GLuint tex;
        glGenTextures(1, &tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, texels);
        glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
        glUniform4f(glGetUniformLocation(prog, "u_color"), 1.0f, 0.0f,
                    0.0f, 1.0f);
    }
    GLint loc = -1;
    if (vbo) {
        GLuint buf;
        glGenBuffers(1, &buf);
        glBindBuffer(GL_ARRAY_BUFFER, buf);
        if (split)
            glBufferData(GL_ARRAY_BUFFER, sizeof(split_positions), split_positions, GL_STATIC_DRAW);
        else if (fan)
            glBufferData(GL_ARRAY_BUFFER, sizeof(fan_positions), fan_positions, GL_STATIC_DRAW);
        else
            glBufferData(GL_ARRAY_BUFFER, sizeof(vbo_positions), vbo_positions, GL_STATIC_DRAW);
        loc = glGetAttribLocation(prog, "a_pos");
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
    }
    unsigned between_red = 0;
    if (loadbench) {
        /* Establish the image once. Every measured draw then starts a new
         * render that reloads the prior colour attachment and stores it again.
         * This isolates the ordinary BG reload/EOT path from clear handling. */
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        if (glGetError() != GL_NO_ERROR)
            return fail("loadbench-warmup");
    }
    uint64_t bench_start_ns = monotonic_ns();
    for (long frame = 0; frame < repeat; ++frame) {
        if (!loadbench)
            glClear(GL_COLOR_BUFFER_BIT);
        if (fan)
            glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        else if (!clear_only)
            glDrawArrays(GL_TRIANGLES, 0, 3);
        if (reload) {
            // Read back between the two halves: the second draw is a new batch
            // that must keep the first half.
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, between);
            for (size_t i = 0; i < bytes; i += 4)
                between_red += between[i] == 255 && between[i + 1] == 0 && between[i + 2] == 0;
            fprintf(stderr, "between draws: red=%u\n", between_red);
            glDrawArrays(GL_TRIANGLES, 3, 3);
        } else if (first) {
            glDrawArrays(GL_TRIANGLES, 3, 3);
        } else if (split) {
            glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, (const void *)(6 * sizeof(float)));
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        if (repeat > 1 || loadbench)
            glFinish();
    }
    uint64_t bench_end_ns = monotonic_ns();
    fprintf(stderr, "repeats completed=%ld\n", repeat);
    if ((repeat > 1 || loadbench) && bench_start_ns &&
        bench_end_ns >= bench_start_ns) {
        uint64_t elapsed_ns = bench_end_ns - bench_start_ns;
        printf("G15_GL_BENCH mode=%s frames=%ld wall_ns=%" PRIu64
               " ns_per_frame=%.1f\n", mode, repeat, elapsed_ns,
               repeat ? (double)elapsed_ns / repeat : 0.0);
    }

    memset(actual, M3R_CANARY_ALPHA, bytes);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, actual);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
        return fail("readpixels");

    if (out) {
        FILE *f = fopen(out, "wb");
        if (!f || fwrite(actual, 1, bytes, f) != bytes)
            return fail("write-out");
        fclose(f);
    }

    if (gradient || linear) {
        // Same coverage as the oracle; inside, red/green within 1 of
        // round(255 * (p + 0.5) / 128), blue 0, alpha 255.
        unsigned bad = 0, inside = 0, maxerr = 0;
        expected_image(W, H, 1, expected);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const uint8_t *a = actual + ((size_t)y * W + x) * 4;
                const uint8_t *e = expected + ((size_t)y * W + x) * 4;
                if (!m3r_inside(x, y)) {
                    bad += memcmp(a, e, 4) != 0;
                    continue;
                }
                ++inside;
                int er = (int)(255.0 * (x + 0.5) / 128 + 0.5);
                int eg = linear ? 0 : (int)(255.0 * (y + 0.5) / 128 + 0.5);
                unsigned d = MAX2(abs(a[0] - er), abs(a[1] - eg));
                maxerr = MAX2(maxerr, d);
                bad += d > 1 || a[2] != 0 || a[3] != 255;
            }
        printf("G15_GL_RESULT %s mismatches=%u inside=%u maxerr=%u fnv1a64=%016" PRIx64 "\n",
               bad ? "FAIL" : "PASS", bad, inside, maxerr, m3r_hash(actual, bytes));
        return bad != 0;
    }
    expected_image(W, H, !clear_only, expected);
    if (discard) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < 64 && x < W; ++x) {
                uint8_t *p = expected + ((size_t)y * W + x) * 4;
                if (p[0] == 255)
                    p[0] = 0, p[2] = 255;
            }
    }
    if (green) {
        // Background (0,255,0,255) instead of blue; same triangle.
        for (size_t i = 0; i < bytes; i += 4)
            if (expected[i + 2] == 255)
                expected[i + 1] = 255, expected[i + 2] = 0;
    }
    struct m3r_check c = check_image(actual, expected, bytes);
    // The oracle's own hash must be the native capture's where there is one.
    int oracle_ok = 1;
    if (!green && clear_only && W == M3R_WIDTH && H == M3R_HEIGHT)
        oracle_ok = c.expected_hash == M3R_CLEAR_FNV;
    for (size_t i = 0; !green && !clear_only && !discard &&
                       i < sizeof(native_triangles) / sizeof(native_triangles[0]); ++i)
        if (native_triangles[i].w == W && native_triangles[i].h == H)
            oracle_ok = c.expected_hash == native_triangles[i].fnv;
    int pass = c.mismatched_pixels == 0 && (!reload || (between_red > 0 && between_red < c.red_pixels)) && oracle_ok;
    printf("G15_GL_RESULT %s mismatches=%u red=%u canary=%u fnv1a64=%016" PRIx64 " expected=%016" PRIx64 "\n",
           pass ? "PASS" : "FAIL", c.mismatched_pixels, c.red_pixels, c.canary_pixels, c.actual_hash,
           c.expected_hash);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    return pass ? 0 : 1;
}
