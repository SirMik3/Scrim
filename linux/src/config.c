// Config file: ~/.config/scrim/scrim.conf (INI-style). Linux has no menu bar,
// so this file is the settings UI; the app reloads it as soon as it is saved.
//
//   effect = rain
//   fps = 30
//   hidpi = false
//   pause_on_battery = true
//   wallpaper = ~/Pictures/lake.jpg
//
//   [rain]
//   fog = 0.4
//   darken = 0.5
#include "rp.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rp_config_defaults(rp_config *c) {
    memset(c, 0, sizeof *c);
    snprintf(c->effect, sizeof c->effect, "rain");
    c->fps = 30;
    c->hidpi = false;
    c->pause_on_battery = true;
}

void rp_config_path(char *out, size_t len) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(out, len, "%s/scrim/scrim.conf", xdg);
    else snprintf(out, len, "%s/.config/scrim/scrim.conf", getenv("HOME") ? getenv("HOME") : ".");
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static bool parse_bool(const char *v, bool fallback) {
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1")) return true;
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0")) return false;
    return fallback;
}

void rp_config_set_value(rp_config *c, const char *section, const char *key, const char *value) {
    for (int i = 0; i < c->nvalues; i++) {
        if (!strcmp(c->values[i].section, section) && !strcmp(c->values[i].key, key)) {
            snprintf(c->values[i].value, sizeof c->values[i].value, "%s", value);
            return;
        }
    }
    if (c->nvalues >= (int)(sizeof c->values / sizeof *c->values)) return;
    rp_param_value *v = &c->values[c->nvalues++];
    snprintf(v->section, sizeof v->section, "%s", section);
    snprintf(v->key, sizeof v->key, "%s", key);
    snprintf(v->value, sizeof v->value, "%s", value);
}

bool rp_config_load(rp_config *c, const char *path, char *err, size_t errlen) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return true;
        snprintf(err, errlen, "cannot read %s", path);
        return false;
    }
    char line[PATH_MAX + 128], section[64] = "";
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end) continue;
            *end = 0;
            snprintf(section, sizeof section, "%s", trim(s + 1));
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) { rp_log("%s:%d: expected key = value", path, lineno); continue; }
        *eq = 0;
        char *key = trim(s), *value = trim(eq + 1);
        if (section[0]) {
            rp_config_set_value(c, section, key, value);
        } else if (!strcmp(key, "effect")) {
            snprintf(c->effect, sizeof c->effect, "%s", value);
        } else if (!strcmp(key, "fps")) {
            int fps = atoi(value);
            c->fps = fps < 1 ? 1 : fps > 120 ? 120 : fps;
        } else if (!strcmp(key, "hidpi")) {
            c->hidpi = parse_bool(value, c->hidpi);
        } else if (!strcmp(key, "pause_on_battery")) {
            c->pause_on_battery = parse_bool(value, c->pause_on_battery);
        } else if (!strcmp(key, "wallpaper")) {
            if (value[0] == '~' && getenv("HOME"))
                snprintf(c->wallpaper, sizeof c->wallpaper, "%s%s", getenv("HOME"), value + 1);
            else
                snprintf(c->wallpaper, sizeof c->wallpaper, "%s", value);
        } else {
            rp_log("%s:%d: unknown setting '%s'", path, lineno, key);
        }
    }
    fclose(f);
    return true;
}

void rp_config_params(const rp_config *c, const rp_effect *e, float *params) {
    for (int i = 0; i < e->nparams; i++) {
        const rp_param *p = &e->params[i];
        float v = p->def;
        for (int k = 0; k < c->nvalues; k++) {
            if (strcmp(c->values[k].section, e->id) || strcmp(c->values[k].key, p->id)) continue;
            if (p->type == RP_PARAM_BOOL) v = parse_bool(c->values[k].value, v > 0.5f) ? 1.0f : 0.0f;
            else v = strtof(c->values[k].value, NULL);
        }
        params[i] = v < p->min ? p->min : v > p->max ? p->max : v;
    }
}
