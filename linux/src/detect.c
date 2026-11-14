// Finds the desktop's own wallpaper, so Scrim can draw over it without a
// `wallpaper =` setting. Looks at what is actually running or configured, most
// specific first. `scrim --print-wallpaper` shows the result and where it came from.
#include "rp.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ---- helpers -------------------------------------------------------------------

// Runs a shell command and returns its first output line containing `marker`
// (or the first line if marker is NULL), without the newline.
static bool command_line(const char *cmd, const char *marker, char *out, size_t len) {
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    char line[PATH_MAX + 256];
    bool found = false;
    while (!found && fgets(line, sizeof line, p)) {
        if (marker && !strstr(line, marker)) continue;
        line[strcspn(line, "\n")] = 0;
        snprintf(out, len, "%s", line);
        found = true;
    }
    pclose(p);
    return found;
}

static void trim(char *s) {
    char *start = s;
    while (isspace((unsigned char)*start)) start++;
    memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

// file:///a%20b.jpg -> /a b.jpg; also strips surrounding quotes. In place.
static void normalise_path(char *s) {
    trim(s);
    size_t n = strlen(s);
    if (n >= 2 && (s[0] == '\'' || s[0] == '"') && s[n - 1] == s[0]) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = 0;
    }
    if (!strncmp(s, "file://", 7)) memmove(s, s + 7, strlen(s + 7) + 1);
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
    if (s[0] == '~' && getenv("HOME")) {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s%s", getenv("HOME"), s + 1);
        snprintf(s, PATH_MAX, "%s", tmp);
    }
}

static bool usable(const char *path) {
    struct stat st;
    return path[0] == '/' && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// Finds a running process of this user by name; copies its NUL-separated
// command line into args (returns its length) or returns -1.
static int find_process(const char *name, char *args, size_t len) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *ent;
    int result = -1;
    while (result < 0 && (ent = readdir(d))) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        char path[300], comm[64] = "";
        snprintf(path, sizeof path, "/proc/%s/comm", ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || st.st_uid != getuid()) continue;
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0;
        fclose(f);
        if (strcmp(comm, name) != 0) continue;
        snprintf(path, sizeof path, "/proc/%s/cmdline", ent->d_name);
        if (!(f = fopen(path, "r"))) continue;
        size_t n = fread(args, 1, len - 1, f);
        fclose(f);
        args[n] = 0;
        result = (int)n;
    }
    closedir(d);
    return result;
}

static void config_file(char *out, size_t len, const char *relative) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(out, len, "%s/%s", xdg, relative);
    else snprintf(out, len, "%s/.config/%s", getenv("HOME") ? getenv("HOME") : "", relative);
}

static bool desktop_is(const char *name) {
    const char *d = getenv("XDG_CURRENT_DESKTOP");
    return d && strcasestr(d, name);
}

// ---- sources -------------------------------------------------------------------

// swww query: "eDP-1: 1920x1080, scale: 1, currently displaying: image: /path"
// (newer versions start the line with ": "). The first output wins.
static bool from_swww(char *out, size_t len) {
    char args[16], line[PATH_MAX + 256];
    if (find_process("swww-daemon", args, sizeof args) < 0) return false;
    if (!command_line("swww query 2>/dev/null", "image: ", line, sizeof line)) return false;
    snprintf(out, len, "%s", strstr(line, "image: ") + 7);
    return true;
}

// hyprctl hyprpaper listactive: "eDP-1 = /path"
static bool from_hyprpaper(char *out, size_t len) {
    char args[16], line[PATH_MAX + 256];
    if (find_process("hyprpaper", args, sizeof args) < 0) return false;
    if (!command_line("hyprctl hyprpaper listactive 2>/dev/null", " = ", line, sizeof line)) return false;
    snprintf(out, len, "%s", strstr(line, " = ") + 3);
    return true;
}

// swaybg -o '*' -i /path -m fill   (what Sway's `output * bg /path fill` runs)
static bool from_swaybg(char *out, size_t len) {
    char args[4096];
    int n = find_process("swaybg", args, sizeof args);
    for (int i = 0; i < n;) {
        const char *arg = args + i;
        i += (int)strlen(arg) + 1;
        if ((!strcmp(arg, "-i") || !strcmp(arg, "--image")) && i < n) {
            snprintf(out, len, "%s", args + i);
            return true;
        }
        if (!strncmp(arg, "--image=", 8)) { snprintf(out, len, "%s", arg + 8); return true; }
    }
    return false;
}

// KDE Plasma: plasma-org.kde.plasma.desktop-appletsrc, in a section like
// [Containments][1][Wallpaper][org.kde.image][General]: Image=file:///path
static bool from_plasma(char *out, size_t len) {
    if (!desktop_is("KDE")) return false;
    char path[PATH_MAX], line[PATH_MAX + 64];
    config_file(path, sizeof path, "plasma-org.kde.plasma.desktop-appletsrc");
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool in_image = false, found = false;
    while (!found && fgets(line, sizeof line, f)) {
        if (line[0] == '[') in_image = strstr(line, "[Wallpaper][org.kde.image][General]") != NULL;
        else if (in_image && !strncmp(line, "Image=", 6)) {
            line[strcspn(line, "\n")] = 0;
            snprintf(out, len, "%s", line + 6);
            found = true;
        }
    }
    fclose(f);
    return found;
}

// XFCE: xfce4-desktop.xml, <property name="last-image" type="string" value="/path"/>
static bool from_xfce(char *out, size_t len) {
    if (!desktop_is("XFCE")) return false;
    char path[PATH_MAX], line[PATH_MAX + 128];
    config_file(path, sizeof path, "xfce4/xfconf/xfce-perchannel-xml/xfce4-desktop.xml");
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        char *v;
        if (!strstr(line, "name=\"last-image\"") || !(v = strstr(line, "value=\""))) continue;
        v += 7;
        char *end = strchr(v, '"');
        if (!end) continue;
        *end = 0;
        // The only XML escape that matters in a path.
        char *w = v;
        for (char *r = v; *r; r++) {
            if (!strncmp(r, "&amp;", 5)) { *w++ = '&'; r += 4; }
            else *w++ = *r;
        }
        *w = 0;
        snprintf(out, len, "%s", v);
        found = true;
    }
    fclose(f);
    return found;
}

// GNOME, Cinnamon, Budgie, MATE: gsettings. GNOME shows picture-uri-dark only in dark mode.
static bool from_gsettings(char *out, size_t len) {
    // Other desktops may have GNOME's settings installed too (with GNOME's default
    // wallpaper in them), which isn't what's on screen there.
    static const char *users[] = {"GNOME", "Cinnamon", "Budgie", "MATE", "Unity", "Pantheon"};
    bool applies = false;
    for (size_t i = 0; i < sizeof users / sizeof *users; i++) applies |= desktop_is(users[i]);
    if (!applies) return false;
    char line[PATH_MAX + 64];
    if (desktop_is("MATE") &&
        command_line("gsettings get org.mate.background picture-filename 2>/dev/null", NULL, line, sizeof line)) {
        snprintf(out, len, "%s", line);
        return true;
    }
    if (desktop_is("Cinnamon") &&
        command_line("gsettings get org.cinnamon.desktop.background picture-uri 2>/dev/null", NULL, line, sizeof line)) {
        snprintf(out, len, "%s", line);
        return true;
    }
    bool dark = command_line("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null",
                             "prefer-dark", line, sizeof line);
    if (dark && command_line("gsettings get org.gnome.desktop.background picture-uri-dark 2>/dev/null",
                             "file://", line, sizeof line)) {
        snprintf(out, len, "%s", line);
        return true;
    }
    if (command_line("gsettings get org.gnome.desktop.background picture-uri 2>/dev/null", "file://", line, sizeof line)) {
        snprintf(out, len, "%s", line);
        return true;
    }
    return false;
}

// ~/.fehbg: feh --no-fehbg --bg-fill '/path'
static bool from_feh(char *out, size_t len) {
    char path[PATH_MAX], line[PATH_MAX + 128];
    snprintf(path, sizeof path, "%s/.fehbg", getenv("HOME") ? getenv("HOME") : "");
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        char *bg = strstr(line, "--bg-"), *q1, *q2;
        if (!bg || !(q1 = strchr(bg, '\'')) || !(q2 = strchr(q1 + 1, '\''))) continue;
        *q2 = 0;
        snprintf(out, len, "%s", q1 + 1);
        found = true;
    }
    fclose(f);
    return found;
}

// nitrogen: ~/.config/nitrogen/bg-saved.cfg, file=/path
static bool from_nitrogen(char *out, size_t len) {
    char path[PATH_MAX], line[PATH_MAX + 16];
    config_file(path, sizeof path, "nitrogen/bg-saved.cfg");
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        if (strncmp(line, "file=", 5) != 0) continue;
        line[strcspn(line, "\n")] = 0;
        snprintf(out, len, "%s", line + 5);
        found = true;
    }
    fclose(f);
    return found;
}

void rp_wallpaper_detect(char *out, size_t len, const char **source) {
    static const struct {
        const char *name;
        bool (*find)(char *, size_t);
    } sources[] = {
        {"swww", from_swww}, {"hyprpaper", from_hyprpaper}, {"swaybg", from_swaybg},
        {"KDE Plasma", from_plasma}, {"XFCE", from_xfce}, {"gsettings", from_gsettings},
        {"feh", from_feh}, {"nitrogen", from_nitrogen},
    };
    out[0] = 0;
    if (source) *source = NULL;
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        char path[PATH_MAX] = "";
        if (!sources[i].find(path, sizeof path)) continue;
        normalise_path(path);
        if (!usable(path)) continue;   // e.g. a solid colour, or a file that has gone
        snprintf(out, len, "%s", path);
        if (source) *source = sources[i].name;
        return;
    }
}
