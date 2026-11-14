// Control socket: `scrim msg COMMAND` talks to the running instance, so pausing or
// switching effects can be bound to a key or a status-bar button. One line in, one
// or more lines out, then the connection closes. The socket also keeps a second
// instance from starting (e.g. from both autostart and the systemd unit).
//
//   $XDG_RUNTIME_DIR/scrim.sock   (or /tmp/scrim-UID.sock without XDG_RUNTIME_DIR)
#include "rp.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

void rp_control_path(char *out, size_t len) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (dir && *dir) snprintf(out, len, "%s/scrim.sock", dir);
    else snprintf(out, len, "/tmp/scrim-%u.sock", (unsigned)getuid());
}

static bool make_address(struct sockaddr_un *addr, const char *path) {
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr->sun_path) return false;
    memcpy(addr->sun_path, path, strlen(path) + 1);
    return true;
}

static int connect_to(const char *path) {
    struct sockaddr_un addr;
    if (!make_address(&addr, path)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int rp_control_listen(void) {
    char path[PATH_MAX];
    rp_control_path(path, sizeof path);
    struct sockaddr_un addr;
    if (!make_address(&addr, path)) { rp_log("control socket path too long: %s", path); return -1; }

    int other = connect_to(path);
    if (other >= 0) {             // someone is listening: another Scrim is running
        close(other);
        return RP_CONTROL_RUNNING;
    }
    if (errno == ECONNREFUSED) unlink(path);   // left over from a crash

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    mode_t old = umask(077);      // only this user may control us
    int ok = bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0 && listen(fd, 4) == 0;
    umask(old);
    if (!ok) {
        rp_log("cannot create control socket %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

void rp_control_close(int fd) {
    if (fd < 0) return;
    close(fd);
    char path[PATH_MAX];
    rp_control_path(path, sizeof path);
    unlink(path);
}

// Reads one line (without the newline), waiting at most timeout_ms in total.
static bool read_line(int fd, char *buf, size_t len, int timeout_ms) {
    size_t n = 0;
    while (n + 1 < len) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, timeout_ms) <= 0) return false;
        ssize_t r = read(fd, buf + n, 1);
        if (r <= 0) break;
        if (buf[n] == '\n') break;
        n++;
    }
    buf[n] = 0;
    return n > 0;
}

// MSG_NOSIGNAL: a client that already hung up must not kill us with SIGPIPE.
static void write_all(int fd, const char *s) {
    for (size_t len = strlen(s); len > 0;) {
        ssize_t w = send(fd, s, len, MSG_NOSIGNAL);
        if (w <= 0) return;
        s += w;
        len -= (size_t)w;
    }
}

void rp_control_accept(int listen_fd, rp_control_handler handler) {
    for (;;) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) return;   // EAGAIN: no more clients
        char line[512], reply[4096] = "";
        // Clients send their command right away; don't let a stuck one stall rendering.
        // Nothing at all is the "is Scrim running?" probe from a second instance.
        if (read_line(fd, line, sizeof line, 200)) {
            handler(line, reply, sizeof reply);
            write_all(fd, reply);
        }
        close(fd);
    }
}

int rp_control_client(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr,
            "usage: scrim msg COMMAND\n"
            "  pause | resume | toggle     pause or resume rendering\n"
            "  effect ID                   switch effect (until `reload` or a restart)\n"
            "  next                        switch to the next effect\n"
            "  param ID=VALUE              set a parameter of the current effect\n"
            "  reload                      re-read the config file, dropping msg changes\n"
            "  status                      show the current state\n");
        return 2;
    }
    char line[512] = "";
    for (int i = 0; i < argc; i++) {
        if (i) strncat(line, " ", sizeof line - strlen(line) - 1);
        strncat(line, argv[i], sizeof line - strlen(line) - 1);
    }
    strncat(line, "\n", sizeof line - strlen(line) - 1);

    char path[PATH_MAX];
    rp_control_path(path, sizeof path);
    int fd = connect_to(path);
    if (fd < 0) {
        fprintf(stderr, "scrim: Scrim doesn't seem to be running (no %s)\n", path);
        return 1;
    }
    write_all(fd, line);
    shutdown(fd, SHUT_WR);
    char buf[4096];
    bool error = false, first = true;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        if (first && n >= 6 && !strncmp(buf, "error:", 6)) error = true;
        first = false;
        fwrite(buf, 1, (size_t)n, error ? stderr : stdout);
    }
    close(fd);
    return error ? 1 : 0;
}
