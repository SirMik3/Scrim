// Battery detection via /sys/class/power_supply (no daemon dependency).
#include "rp.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

static bool read_line(const char *dir, const char *name, const char *file, char *out, size_t len) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s/%s", dir, name, file);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)len, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\n")] = 0;
    return ok;
}

// True only when a battery exists and no mains adapter is online.
bool rp_on_battery(void) {
    const char *dir = "/sys/class/power_supply";
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
