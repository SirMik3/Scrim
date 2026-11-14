// Wallpaper loading: JPEG (libjpeg, decoded at reduced size), PNG (libpng) and WebP
// (libwebp, decoded at the target size), recognised by their first bytes and
// box-downscaled so the longest side is at most the render size, uploaded with mips.
#include "rp.h"

#include <ctype.h>
#include <stdio.h>   // must precede jpeglib.h
#include <jpeglib.h>
#include <png.h>
#include <setjmp.h>
#include <webp/decode.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *rgba;      // top row first
    int width, height;
} image;

// ---- JPEG ----

struct jpeg_err {
    struct jpeg_error_mgr mgr;
    jmp_buf jump;
};

static void jpeg_fail(j_common_ptr cinfo) {
    longjmp(((struct jpeg_err *)cinfo->err)->jump, 1);
}

static bool load_jpeg(const char *path, int max_side, image *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    struct jpeg_decompress_struct cinfo;
    struct jpeg_err jerr;
    cinfo.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit = jpeg_fail;
    uint8_t *volatile pixels = NULL;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        free(pixels);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    // Let libjpeg scale down by 1/2, 1/4 or 1/8 in the DCT — far less memory and work.
    int longest = cinfo.image_width > cinfo.image_height ? (int)cinfo.image_width : (int)cinfo.image_height;
    int denom = 1;
    while (denom < 8 && longest / (denom * 2) >= max_side) denom *= 2;
    cinfo.scale_num = 1;
    cinfo.scale_denom = (unsigned)denom;
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width, h = (int)cinfo.output_height;
    pixels = malloc((size_t)w * h * 4);
    uint8_t *row = malloc((size_t)w * 3);
    if (!pixels || !row) longjmp(jerr.jump, 1);
    while (cinfo.output_scanline < cinfo.output_height) {
        int y = (int)cinfo.output_scanline;
        JSAMPROW rows[1] = { row };
        jpeg_read_scanlines(&cinfo, rows, 1);
        uint8_t *dst = pixels + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            dst[x * 4 + 0] = row[x * 3 + 0];
            dst[x * 4 + 1] = row[x * 3 + 1];
            dst[x * 4 + 2] = row[x * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
    }
    free(row);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    out->rgba = pixels;
    out->width = w;
    out->height = h;
    return true;
}

// ---- PNG ----

static bool load_png(const char *path, image *out) {
    png_image img;
    memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path)) return false;
    img.format = PNG_FORMAT_RGBA;
    uint8_t *pixels = malloc(PNG_IMAGE_SIZE(img));
    if (!pixels || !png_image_finish_read(&img, NULL, pixels, 0, NULL)) {
        free(pixels);
        png_image_free(&img);
        return false;
    }
    out->rgba = pixels;
    out->width = (int)img.width;
    out->height = (int)img.height;
    return true;
}

// ---- WebP ----

static bool load_webp(const char *path, int max_side, image *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = size > 0 ? malloc((size_t)size) : NULL;
    bool read_ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    WebPDecoderConfig config;
    if (!read_ok || !WebPInitDecoderConfig(&config) || WebPGetFeatures(data, (size_t)size, &config.input) != VP8_STATUS_OK) {
        free(data);
        return false;
    }
    // Let libwebp scale while decoding, so a huge image never needs a full-size buffer.
    int w = config.input.width, h = config.input.height, longest = w > h ? w : h;
    if (longest > max_side) {
        config.options.use_scaling = 1;
        config.options.scaled_width = (int)((double)w * max_side / longest + 0.5);
        config.options.scaled_height = (int)((double)h * max_side / longest + 0.5);
        if (config.options.scaled_width < 1) config.options.scaled_width = 1;
        if (config.options.scaled_height < 1) config.options.scaled_height = 1;
        w = config.options.scaled_width;
        h = config.options.scaled_height;
    }
    config.output.colorspace = MODE_RGBA;   // top row first, as upload() expects
    bool ok = WebPDecode(data, (size_t)size, &config) == VP8_STATUS_OK;
    free(data);
    if (!ok) return false;
    // Copy out of libwebp's buffer so the caller can free() it like the others.
    out->rgba = malloc((size_t)w * h * 4);
    if (out->rgba) {
        for (int y = 0; y < h; y++)
            memcpy(out->rgba + (size_t)y * w * 4, config.output.u.RGBA.rgba + (size_t)y * config.output.u.RGBA.stride,
                   (size_t)w * 4);
        out->width = w;
        out->height = h;
    }
    WebPFreeDecBuffer(&config.output);
    return out->rgba != NULL;
}

// ---- helpers ----

typedef enum { FORMAT_UNKNOWN, FORMAT_JPEG, FORMAT_PNG, FORMAT_WEBP } image_format;

// Recognises an image by its first bytes, whatever its name says.
static image_format sniff(const char *path) {
    unsigned char m[12] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return FORMAT_UNKNOWN;
    size_t n = fread(m, 1, sizeof m, f);
    fclose(f);
    if (n >= 3 && m[0] == 0xFF && m[1] == 0xD8 && m[2] == 0xFF) return FORMAT_JPEG;
    if (n >= 8 && !memcmp(m, "\x89PNG\r\n\x1a\n", 8)) return FORMAT_PNG;
    if (n >= 12 && !memcmp(m, "RIFF", 4) && !memcmp(m + 8, "WEBP", 4)) return FORMAT_WEBP;
    return FORMAT_UNKNOWN;
}

// Area-average downscale so the longest side is at most max_side.
static void downscale(image *im, int max_side) {
    int longest = im->width > im->height ? im->width : im->height;
    if (longest <= max_side) return;
    double k = (double)max_side / longest;
    int dw = (int)(im->width * k + 0.5), dh = (int)(im->height * k + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    uint8_t *dst = malloc((size_t)dw * dh * 4);
    if (!dst) return;
    for (int dy = 0; dy < dh; dy++) {
        int y0 = (int)((long long)dy * im->height / dh), y1 = (int)((long long)(dy + 1) * im->height / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int dx = 0; dx < dw; dx++) {
            int x0 = (int)((long long)dx * im->width / dw), x1 = (int)((long long)(dx + 1) * im->width / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned sum[4] = {0, 0, 0, 0}, n = 0;
            for (int y = y0; y < y1; y++) {
                const uint8_t *s = im->rgba + ((size_t)y * im->width + x0) * 4;
                for (int x = x0; x < x1; x++, s += 4, n++) {
                    sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2]; sum[3] += s[3];
                }
            }
            uint8_t *d = dst + ((size_t)dy * dw + dx) * 4;
            for (int c = 0; c < 4; c++) d[c] = (uint8_t)(sum[c] / n);
        }
    }
    free(im->rgba);
    im->rgba = dst;
    im->width = dw;
    im->height = dh;
}

static void upload(rp_texture *t, const image *im) {
    int levels = 1;
    for (int s = im->width > im->height ? im->width : im->height; s > 1; s >>= 1) levels++;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA8, im->width, im->height);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Rows are uploaded top row first, so t = 0 is the top of the image (spec).
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, im->width, im->height, GL_RGBA, GL_UNSIGNED_BYTE, im->rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    t->width = im->width;
    t->height = im->height;
    t->levels = levels;
}

bool rp_wallpaper_load(rp_texture *out, const char *path, int max_side) {
    memset(out, 0, sizeof *out);
    image im = {0};
    bool ok = false;
    if (path && *path) {
        switch (sniff(path)) {
        case FORMAT_JPEG: ok = load_jpeg(path, max_side, &im); break;
        case FORMAT_PNG: ok = load_png(path, &im); break;
        case FORMAT_WEBP: ok = load_webp(path, max_side, &im); break;
        case FORMAT_UNKNOWN: break;
        }
        if (!ok) rp_log("cannot read wallpaper %s (JPEG, PNG and WebP are supported)", path);
    }
    if (ok) {
        downscale(&im, max_side);
    } else {
        // Dark blue-grey vertical gradient.
        im.width = 4;
        im.height = 64;
        im.rgba = malloc(4 * 64 * 4);
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 4; x++) {
                uint8_t *p = im.rgba + (y * 4 + x) * 4;
                double t = y / 63.0;
                p[0] = (uint8_t)(28 + 20 * t); p[1] = (uint8_t)(36 + 22 * t);
                p[2] = (uint8_t)(52 + 18 * t); p[3] = 255;
            }
    }
    upload(out, &im);
    free(im.rgba);
    return ok;
}

void rp_wallpaper_release(rp_texture *t) {
    if (t->tex) glDeleteTextures(1, &t->tex);
    memset(t, 0, sizeof *t);
}
