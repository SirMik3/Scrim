// Effect discovery, manifest parsing (json-c) and shader compilation.
#include "rp.h"

#include <dirent.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef RP_DATADIR
#define RP_DATADIR "/usr/local/share/scrim"
#endif

static unsigned next_generation = 1;

static void copy_str(char *dst, size_t len, const char *src) {
    snprintf(dst, len, "%s", src ? src : "");
}

int rp_effect_dirs(char dirs[][PATH_MAX], int max) {
    int n = 0;
    const char *env = getenv("SCRIM_EFFECTS_DIR");
    if (env && *env && n < max) copy_str(dirs[n++], PATH_MAX, env);

    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (n < max) {
        if (xdg && *xdg) snprintf(dirs[n++], PATH_MAX, "%s/scrim/effects", xdg);
        else if (home) snprintf(dirs[n++], PATH_MAX, "%s/.local/share/scrim/effects", home);
    }
    if (n < max) snprintf(dirs[n++], PATH_MAX, "%s/effects", RP_DATADIR);
    return n;
}

static const char *json_str(json_object *obj, const char *key) {
    json_object *v;
    return json_object_object_get_ex(obj, key, &v) && json_object_is_type(v, json_type_string)
        ? json_object_get_string(v) : NULL;
}

static bool json_num(json_object *obj, const char *key, double *out) {
    json_object *v;
    if (!json_object_object_get_ex(obj, key, &v)) return false;
    if (!json_object_is_type(v, json_type_double) && !json_object_is_type(v, json_type_int)) return false;
    *out = json_object_get_double(v);
    return true;
}

bool rp_effect_load(rp_effect *e, const char *dir, char *err, size_t errlen) {
    memset(e, 0, sizeof *e);
    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    json_object *root = json_object_from_file(path);
    if (!root) { snprintf(err, errlen, "%s: cannot read or parse", path); return false; }

    bool ok = false;
    json_object *v, *passes, *params;
    int format = json_object_object_get_ex(root, "format", &v) ? json_object_get_int(v) : 0;
    if (format != 1 && format != 2) {
        snprintf(err, errlen, "%s: format must be 1 or 2 (a newer Scrim may be needed)", path); goto done;
    }
    const char *id = json_str(root, "id"), *name = json_str(root, "name");
    if (!id || !name) { snprintf(err, errlen, "%s: id and name are required", path); goto done; }
    copy_str(e->id, sizeof e->id, id);
    copy_str(e->name, sizeof e->name, name);
    copy_str(e->license, sizeof e->license, json_str(root, "license"));
    copy_str(e->dir, sizeof e->dir, dir);

    if (!json_object_object_get_ex(root, "passes", &passes) || !json_object_is_type(passes, json_type_array)) {
        snprintf(err, errlen, "%s: passes must be an array", path); goto done;
    }
    e->npasses = (int)json_object_array_length(passes);
    if (e->npasses < 1 || e->npasses > RP_MAX_PASSES) {
        snprintf(err, errlen, "%s: needs 1-%d passes", path, RP_MAX_PASSES); goto done;
    }
    for (int i = 0; i < e->npasses; i++) {
        json_object *p = json_object_array_get_idx(passes, i);
        const char *shader = json_str(p, "shader");
        if (!shader) { snprintf(err, errlen, "%s: pass %d has no shader", path, i); goto done; }
        copy_str(e->passes[i].stem, sizeof e->passes[i].stem, shader);
        char *dot = strrchr(e->passes[i].stem, '.');
        if (dot) *dot = 0;
        double scale = 1;
        json_num(p, "scale", &scale);
        e->passes[i].scale = scale < 0.125 ? 0.125f : scale > 1 ? 1.0f : (float)scale;

        bool last = i == e->npasses - 1;
        json_object *pv;
        if (json_object_object_get_ex(p, "persistent", &pv)) {
            if (format < 2 || !json_object_is_type(pv, json_type_boolean) || (last && json_object_get_boolean(pv))) {
                snprintf(err, errlen, "%s: pass %d: \"persistent\" needs format 2, a true/false value "
                         "and can't be on the last pass", path, i);
                goto done;
            }
            e->passes[i].persistent = json_object_get_boolean(pv);
        }
        const char *pixel_format = json_str(p, "pixelFormat");
        if (pixel_format) {
            bool known = !strcmp(pixel_format, "rgba8") || !strcmp(pixel_format, "rgba16f");
            if (format < 2 || !known || last) {
                snprintf(err, errlen, "%s: pass %d: \"pixelFormat\" must be rgba8 or rgba16f, needs "
                         "format 2 and can't be on the last pass", path, i);
                goto done;
            }
            e->passes[i].half_float = !strcmp(pixel_format, "rgba16f");
        }

        char gles[PATH_MAX + 160];
        snprintf(gles, sizeof gles, "%s/build/%s.gles", dir, e->passes[i].stem);
        struct stat st;
        if (stat(gles, &st) != 0) {
            snprintf(err, errlen, "%s: missing build/%s.gles (run tools/effectc)", dir, e->passes[i].stem);
            goto done;
        }
    }

    if (json_object_object_get_ex(root, "params", &params) && json_object_is_type(params, json_type_array)) {
        e->nparams = (int)json_object_array_length(params);
        if (e->nparams > RP_MAX_PARAMS) { snprintf(err, errlen, "%s: too many params", path); goto done; }
        for (int i = 0; i < e->nparams; i++) {
            json_object *p = json_object_array_get_idx(params, i);
            rp_param *q = &e->params[i];
            const char *pid = json_str(p, "id"), *type = json_str(p, "type");
            if (!pid || !type) { snprintf(err, errlen, "%s: param %d needs id and type", path, i); goto done; }
            copy_str(q->id, sizeof q->id, pid);
            copy_str(q->label, sizeof q->label, json_str(p, "label"));
            if (strcmp(type, "bool") == 0) {
                q->type = RP_PARAM_BOOL;
                q->min = 0; q->max = 1;
                json_object *d;
                q->def = json_object_object_get_ex(p, "default", &d) && json_object_get_boolean(d) ? 1 : 0;
            } else {
                double lo = 0, hi = 1, def = 0;
                q->type = RP_PARAM_FLOAT;
                json_num(p, "min", &lo); json_num(p, "max", &hi); json_num(p, "default", &def);
                q->min = (float)lo; q->max = (float)hi; q->def = (float)def;
            }
        }
    }
    e->generation = next_generation++;
    ok = true;
done:
    json_object_put(root);
    return ok;
}

bool rp_effect_find(rp_effect *e, const char *id, char *err, size_t errlen) {
    char dirs[4][PATH_MAX];
    int n = rp_effect_dirs(dirs, 4);
    for (int i = 0; i < n; i++) {
        char dir[2 * PATH_MAX];
        snprintf(dir, sizeof dir, "%.4000s/%.255s", dirs[i], id);
        char manifest[2 * PATH_MAX + 16];
        snprintf(manifest, sizeof manifest, "%s/manifest.json", dir);
        struct stat st;
        if (stat(manifest, &st) == 0) return rp_effect_load(e, dir, err, errlen);
    }
    snprintf(err, errlen, "effect '%s' not found (try --list)", id);
    return false;
}

void rp_effect_print_all(void) {
    char dirs[4][PATH_MAX];
    int n = rp_effect_dirs(dirs, 4);
    for (int i = 0; i < n; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        printf("%s:\n", dirs[i]);
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (ent->d_name[0] == '.') continue;
            char dir[2 * PATH_MAX], err[512];
            snprintf(dir, sizeof dir, "%.4000s/%.255s", dirs[i], ent->d_name);
            rp_effect e;
            if (!rp_effect_load(&e, dir, err, sizeof err)) continue;
            printf("  %-14s %s%s%s%s\n", e.id, e.name, *e.license ? "  [" : "", e.license, *e.license ? "]" : "");
            for (int k = 0; k < e.nparams; k++) {
                const rp_param *p = &e.params[k];
                if (p->type == RP_PARAM_BOOL)
                    printf("      %-12s %-20s bool, default %s\n", p->id, p->label, p->def > 0.5f ? "true" : "false");
                else
                    printf("      %-12s %-20s %g..%g, default %g\n", p->id, p->label, p->min, p->max, p->def);
            }
        }
        closedir(d);
    }
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); buf = NULL; }
    if (buf) buf[len] = 0;
    fclose(f);
    return buf;
}

bool rp_effect_compile(rp_effect *e, char *err, size_t errlen) {
    for (int i = 0; i < e->npasses; i++) {
        if (e->passes[i].half_float && !rp_render_half_float()) {
            snprintf(err, errlen, "effect '%s' needs float render targets, which this GPU driver "
                     "doesn't offer (GL_EXT_color_buffer_half_float)", e->id);
            return false;
        }
    }
    for (int i = 0; i < e->npasses; i++) {
        rp_pass *p = &e->passes[i];
        char path[PATH_MAX + 160];
        snprintf(path, sizeof path, "%s/build/%s.gles", e->dir, p->stem);
        char *src = read_file(path);
        if (!src) { snprintf(err, errlen, "cannot read %s", path); return false; }
        char perr[1024] = "";
        p->program = rp_compile_program(src, perr, sizeof perr);
        free(src);
        if (!p->program) { snprintf(err, errlen, "%s/%s: %s", e->id, p->stem, perr); return false; }

        glUseProgram(p->program);
        GLuint block = glGetUniformBlockIndex(p->program, "Host");
        if (block != GL_INVALID_INDEX) glUniformBlockBinding(p->program, block, 0);
        p->loc_wallpaper = glGetUniformLocation(p->program, "uWallpaper");
        p->loc_previous = glGetUniformLocation(p->program, "uPrevious");
        p->loc_feedback = glGetUniformLocation(p->program, "uFeedback");
        p->loc_flip = glGetUniformLocation(p->program, "uFlip");
        if (p->loc_wallpaper >= 0) glUniform1i(p->loc_wallpaper, 1);
        if (p->loc_previous >= 0) glUniform1i(p->loc_previous, 2);
        if (p->loc_feedback >= 0) glUniform1i(p->loc_feedback, 3);
    }
    e->compiled = true;
    return true;
}

void rp_effect_release(rp_effect *e) {
    for (int i = 0; i < e->npasses; i++) {
        if (e->passes[i].program) glDeleteProgram(e->passes[i].program);
        e->passes[i].program = 0;
    }
    e->compiled = false;
}
