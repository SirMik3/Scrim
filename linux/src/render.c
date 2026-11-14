// Runs an effect's passes with OpenGL ES 3.0 — the GL side of the host contract.
#include "rp.h"

#include <stdio.h>
#include <string.h>

// One full-screen triangle providing vUV with (0,0) at the top-left, +y down.
// GL framebuffers are bottom-up, so passes that render into a texture are drawn
// flipped (uFlip = -1); then texture(uPrevious, vUV) reads the same screen spot.
static const char *vertex_source =
    "#version 300 es\n"
    "uniform float uFlip;\n"
    "out highp vec2 vUV;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    vUV = vec2(p.x, 1.0 - p.y);\n"
    "    gl_Position = vec4(p.x * 2.0 - 1.0, (p.y * 2.0 - 1.0) * uFlip, 0.0, 1.0);\n"
    "}\n";

static GLuint ubo, vao;
static GLuint black_tex;        // bound as uFeedback for passes that aren't persistent
static bool half_float_ok;

// Matches the std140 `Host` block in the spec: 3 + 16 + 1 vec4 = 320 bytes.
typedef struct {
    float time_frame[4];
    float res[4];
    float sizes[4];
    float params[RP_MAX_PARAMS][4];
    float state[4];             // reset, steps, step delta (persistent passes only)
} host_block;
_Static_assert(sizeof(host_block) == 320, "Host block must be 320 bytes");

static GLuint compile_shader(GLenum type, const char *src, char *err, size_t errlen) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(s, (GLsizei)errlen, NULL, err);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint rp_compile_program(const char *fragment_source, char *err, size_t errlen) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_source, err, errlen);
    if (!vs) return 0;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_source, err, errlen);
    if (!fs) { glDeleteShader(vs); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(prog, (GLsizei)errlen, NULL, err);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static bool has_gl_extension(const char *name) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const char *ext = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (ext && !strcmp(ext, name)) return true;
    }
    return false;
}

bool rp_render_half_float(void) { return half_float_ok; }

bool rp_render_init(char *err, size_t errlen) {
    half_float_ok = has_gl_extension("GL_EXT_color_buffer_half_float") ||
                    has_gl_extension("GL_EXT_color_buffer_float");
    static const unsigned char black[4] = {0, 0, 0, 0};
    glGenTextures(1, &black_tex);
    glBindTexture(GL_TEXTURE_2D, black_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenBuffers(1, &ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(host_block), NULL, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    if (glGetError() != GL_NO_ERROR) { snprintf(err, errlen, "GL setup failed"); return false; }
    return true;
}

void rp_targets_release(rp_targets *t) {
    for (int i = 0; i < t->npasses; i++)
        for (int k = 0; k < 2; k++) {
            if (t->fbo[i][k]) glDeleteFramebuffers(1, &t->fbo[i][k]);
            if (t->tex[i][k]) glDeleteTextures(1, &t->tex[i][k]);
        }
    memset(t, 0, sizeof *t);
}

static void pass_size(const rp_effect *e, int i, int width, int height, int *w, int *h) {
    *w = (int)(width * e->passes[i].scale + 0.5f);
    *h = (int)(height * e->passes[i].scale + 0.5f);
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

static void make_target(GLuint *tex, GLuint *fbo, int w, int h, bool half_float) {
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, half_float ? GL_RGBA16F : GL_RGBA8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        rp_log("render target %dx%d%s is incomplete (0x%x)", w, h, half_float ? " rgba16f" : "", status);
    // Persistent state starts as transparent black (spec); harmless for the others.
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
}

// (Re)builds the targets when the effect or the size changed. Rebuilding clears any
// persistent state, as the spec allows.
static void ensure_targets(const rp_effect *e, rp_targets *t, int width, int height) {
    if (t->width == width && t->height == height && t->generation == e->generation) return;
    rp_targets_release(t);
    t->width = width;
    t->height = height;
    t->npasses = e->npasses;
    t->generation = e->generation;
    for (int i = 0; i < e->npasses - 1; i++) {   // the last pass draws to the window
        const rp_pass *p = &e->passes[i];
        pass_size(e, i, width, height, &t->tw[i], &t->th[i]);
        for (int k = 0; k < (p->persistent ? 2 : 1); k++)
            make_target(&t->tex[i][k], &t->fbo[i][k], t->tw[i], t->th[i], p->half_float);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void rp_render_frame(const rp_effect *e, rp_targets *t, const rp_texture *wallpaper,
                     const float *params, double time, double frame, double delta,
                     bool step, GLuint target, int width, int height) {
    ensure_targets(e, t, width, height);
    bool reset = t->steps == 0;
    if (reset) step = true;   // persistent passes need their initial state

    // Time since this output last stepped: outputs can skip frames independently
    // (e.g. a covered Wayland output), so the shared per-tick delta isn't enough.
    double step_delta = reset ? delta : time - t->last_step_time;
    if (step_delta < 0) step_delta = delta;          // the clock wrapped (spec allows it)
    if (step_delta > 0.1) step_delta = 0.1;

    host_block b;
    memset(&b, 0, sizeof b);
    b.time_frame[0] = (float)time;
    b.time_frame[1] = (float)frame;
    b.time_frame[2] = (float)delta;
    b.time_frame[3] = (float)wallpaper->levels;
    b.sizes[0] = (float)width;
    b.sizes[1] = (float)height;
    b.sizes[2] = (float)wallpaper->width;
    b.sizes[3] = (float)wallpaper->height;
    for (int i = 0; i < e->nparams; i++) b.params[i][0] = params[i];

    glBindVertexArray(vao);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
    GLuint previous = wallpaper->tex;
    int write = 1 - t->cur;   // persistent passes: read [cur], write [1 - cur]
    for (int i = 0; i < e->npasses; i++) {
        const rp_pass *p = &e->passes[i];
        bool last = i == e->npasses - 1;
        if (p->persistent && !step) {   // paused redraw: reuse the latest state
            previous = t->tex[i][t->cur];
            continue;
        }
        int w = last ? width : t->tw[i], h = last ? height : t->th[i];
        int slot = p->persistent ? write : 0;
        glBindFramebuffer(GL_FRAMEBUFFER, last ? target : t->fbo[i][slot]);
        glViewport(0, 0, w, h);

        b.res[0] = (float)w; b.res[1] = (float)h;
        b.res[2] = 1.0f / (float)w; b.res[3] = 1.0f / (float)h;
        if (p->persistent) {
            b.state[0] = reset ? 1.0f : 0.0f;
            b.state[1] = (float)t->steps;
            b.state[2] = (float)step_delta;
        } else {
            memset(b.state, 0, sizeof b.state);
        }
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof b, &b);

        glUseProgram(p->program);
        if (p->loc_flip >= 0) glUniform1f(p->loc_flip, last ? 1.0f : -1.0f);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, wallpaper->tex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, previous);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, p->persistent ? t->tex[i][t->cur] : black_tex);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (!last) previous = t->tex[i][slot];
    }
    if (step) {
        t->cur = write;
        t->steps += 1;
        t->last_step_time = time;
    }
}
