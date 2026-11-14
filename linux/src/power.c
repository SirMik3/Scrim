// Power state from sysfs (no daemon or D-Bus dependency): battery, and the
// power-saver profile. SCRIM_SYS_ROOT prefixes every path, for tests.
#include "rp.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sys_path(char *out, size_t len, const char *path) {
    const char *root = getenv("SCRIM_SYS_ROOT");
    snprintf(out, len, "%s%s", root ? root : "", path);
}

static bool read_first_line(const char *path, char *out, size_t len) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)len, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\n")] = 0;
    return ok;
}

static bool read_line(const char *dir, const char *name, const char *file, char *out, size_t len) {
    char path[PATH_MAX + 512];   // dir + a directory entry name (at most 255) + file
    snprintf(path, sizeof path, "%s/%s/%s", dir, name, file);
    return read_first_line(path, out, len);
}

// True only when a battery exists and no mains adapter is online.
bool rp_on_battery(void) {
    char dir[PATH_MAX];
    sys_path(dir, sizeof dir, "/sys/class/power_supply");
    DIR *d = opendir(dir);
    if (!d) return false;
    bool mains_online = false, battery_discharging = false;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        char type[32], value[32];
        if (!read_line(dir, ent->d_name, "type", type, sizeof type)) continue;
        if (!strcmp(type, "Mains") && read_line(dir, ent->d_name, "online", value, sizeof value))
            mains_online |= !strcmp(value, "1");
        if (!strcmp(type, "Battery") && read_line(dir, ent->d_name, "status", value, sizeof value))
            battery_discharging |= !strcmp(value, "Discharging");
    }
    closedir(d);
    return battery_discharging && !mains_online;
}

// The Linux counterpart of macOS Low Power Mode: the "power-saver" profile of
// power-profiles-daemon (GNOME/KDE's power menu). It sets the firmware's
// platform_profile to "low-power" where the hardware has one; otherwise its saved
// state is the only place to read it without D-Bus.
bool rp_power_saver(void) {
    char path[PATH_MAX], line[128];
    sys_path(path, sizeof path, "/sys/firmware/acpi/platform_profile");
    if (read_first_line(path, line, sizeof line) && !strcmp(line, "low-power")) return true;
    sys_path(path, sizeof path, "/var/lib/power-profiles-daemon/state.ini");
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool saver = false;
    while (!saver && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        saver = !strcmp(line, "Profile=power-saver");
    }
    fclose(f);
    return saver;
}
