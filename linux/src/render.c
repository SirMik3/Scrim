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

// Matches the std140 `Host` block in the spec: 3 + 16 vec4 = 304 bytes.
typedef struct {
    float time_frame[4];
    float res[4];
    float sizes[4];
    float params[RP_MAX_PARAMS][4];
} host_block;
_Static_assert(sizeof(host_block) == 304, "Host block must be 304 bytes");

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

bool rp_render_init(char *err, size_t errlen) {
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
    for (int i = 0; i < t->npasses; i++) {
        if (t->fbo[i]) glDeleteFramebuffers(1, &t->fbo[i]);
        if (t->tex[i]) glDeleteTextures(1, &t->tex[i]);
    }
    memset(t, 0, sizeof *t);
}

static void ensure_targets(const rp_effect *e, rp_targets *t, int width, int height) {
    if (t->width == width && t->height == height && t->npasses == e->npasses) {
        bool same = true;   // same effect shape? (scales may differ between effects)
        for (int i = 0; i < e->npasses - 1; i++) {
            int w = (int)(width * e->passes[i].scale + 0.5f), h = (int)(height * e->passes[i].scale + 0.5f);
            if (t->tw[i] != (w < 1 ? 1 : w) || t->th[i] != (h < 1 ? 1 : h)) same = false;
        }
        if (same) return;
    }
    rp_targets_release(t);
    t->width = width;
    t->height = height;
    t->npasses = e->npasses;
    for (int i = 0; i < e->npasses - 1; i++) {   // the last pass draws to the window
        int w = (int)(width * e->passes[i].scale + 0.5f), h = (int)(height * e->passes[i].scale + 0.5f);
        t->tw[i] = w < 1 ? 1 : w;
        t->th[i] = h < 1 ? 1 : h;
        glGenTextures(1, &t->tex[i]);
        glBindTexture(GL_TEXTURE_2D, t->tex[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, t->tw[i], t->th[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &t->fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void rp_render_frame(const rp_effect *e, rp_targets *t, const rp_texture *wallpaper,
                     const float *params, double time, double frame, double delta,
                     int width, int height) {
    ensure_targets(e, t, width, height);

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
    for (int i = 0; i < e->npasses; i++) {
        const rp_pass *p = &e->passes[i];
        bool last = i == e->npasses - 1;
        int w = last ? width : t->tw[i], h = last ? height : t->th[i];
        glBindFramebuffer(GL_FRAMEBUFFER, last ? 0 : t->fbo[i]);
        glViewport(0, 0, w, h);

        b.res[0] = (float)w; b.res[1] = (float)h;
        b.res[2] = 1.0f / (float)w; b.res[3] = 1.0f / (float)h;
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof b, &b);

        glUseProgram(p->program);
        if (p->loc_flip >= 0) glUniform1f(p->loc_flip, last ? 1.0f : -1.0f);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, wallpaper->tex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, previous);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (!last) previous = t->tex[i];
    }
}
