#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define OSC_BIND_IP "10.101.1.1"
#define OSC_PORT 8000
#define SQ_IP "192.168.2.100"
#define SQ_PORT 51325
#define MIDI_CH 1
#define MAX_PACKET 65535

static volatile sig_atomic_t running = 1;
static int udp_fd = -1, tcp_fd = -1;
static time_t next_connect = 0;

static void logmsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
static void on_signal(int sig) { (void)sig; running = 0; }

static uint32_t read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static size_t osc_string_end(const unsigned char *p, size_t len, size_t off) {
    if (off >= len) return 0;
    size_t i = off;
    while (i < len && p[i] != 0) i++;
    if (i == len) return 0;
    return (i + 4) & ~(size_t)3;
}

/* Accept only literal no-argument OSC messages. Bundles are traversed recursively. */
static bool parse_message(const unsigned char *p, size_t len,
                          unsigned char *midi, size_t *midi_len) {
    size_t path_end = osc_string_end(p, len, 0);
    if (!path_end) return false;
    size_t tags_end = osc_string_end(p, len, path_end);
    if (!tags_end || path_end >= len || p[path_end] != ',') return false;
    if (p[path_end + 1] != '\0') return false; /* no OSC arguments */

    char path[128];
    size_t plen = 0;
    while (plen < sizeof(path)-1 && plen < len && p[plen]) {
        path[plen] = (char)p[plen]; plen++;
    }
    path[plen] = '\0';

    unsigned int n = 0;
    char tail = '\0';
    unsigned char status = (unsigned char)(0xB0 | (MIDI_CH - 1));

    if (sscanf(path, "/scene/%u/run%c", &n, &tail) == 1 && n >= 1 && n <= 300) {
        unsigned int zero = n - 1;
        unsigned char bank = (unsigned char)(zero / 128);
        unsigned char program = (unsigned char)(zero % 128);
        /* Bank Select MSB 0, then Program Change. */
        midi[0] = status; midi[1] = 0x00; midi[2] = bank;
        midi[3] = (unsigned char)(0xC0 | (MIDI_CH - 1)); midi[4] = program;
        *midi_len = 5;
        logmsg("OSC %s -> scene %u (bank %u, program %u)", path, n, bank, program);
        return true;
    }
    tail = '\0'; n = 0;
    if (sscanf(path, "/sqcue/%u/run%c", &n, &tail) == 1 &&
        n >= 1 && n <= 300) {
        unsigned int zero = n - 1;
        unsigned char bank = (unsigned char)(0x0A + zero / 128);
        unsigned char program = (unsigned char)(zero % 128);

        /* Cue List bank select, then Program Change. */
        midi[0] = status;
        midi[1] = 0x00;
        midi[2] = bank;
        midi[3] = (unsigned char)(0xC0 | (MIDI_CH - 1));
        midi[4] = program;
        *midi_len = 5;

        logmsg("OSC %s -> cue position %u (bank %02X, program %u)",
               path, n, bank, program);
        return true;
    }
    tail = '\0'; n = 0;
    if (sscanf(path, "/chan/%u/toggle%c", &n, &tail) == 1 && n >= 1 && n <= 48) {
        unsigned int index = n - 1;
        midi[0] = status; midi[1] = 0x63; midi[2] = 0x00;
        midi[3] = status; midi[4] = 0x62; midi[5] = (unsigned char)index;
        midi[6] = status; midi[7] = 0x60; midi[8] = 0x00;
        *midi_len = 9;
        logmsg("OSC %s -> input channel %u mute toggle", path, n);
        return true;
    }
    tail = '\0'; n = 0;
    if (sscanf(path, "/mtgrp/%u/toggle%c", &n, &tail) == 1 && n >= 1 && n <= 8) {
        unsigned int index = n - 1;
        midi[0] = status; midi[1] = 0x63; midi[2] = 0x04;
        midi[3] = status; midi[4] = 0x62; midi[5] = (unsigned char)index;
        midi[6] = status; midi[7] = 0x60; midi[8] = 0x00;
        *midi_len = 9;
        logmsg("OSC %s -> mute group %u toggle", path, n);
        return true;
    }
    logmsg("Ignoring unsupported OSC path: %s", path);
    return false;
}

static void handle_osc(const unsigned char *p, size_t len, int depth) {
    if (depth > 8 || len < 4) return;
    if (len >= 8 && memcmp(p, "#bundle\0", 8) == 0) {
        size_t off = 16; /* bundle header plus 64-bit timetag */
        while (off + 4 <= len) {
            uint32_t item_len = read_be32(p + off);
            off += 4;
            if (item_len == 0 || item_len > len - off) {
                logmsg("Malformed OSC bundle element");
                return;
            }
            handle_osc(p + off, item_len, depth + 1);
            off += item_len;
        }
        return;
    }
    unsigned char midi[16];
    size_t midi_len = 0;
    if (!parse_message(p, len, midi, &midi_len)) return;
    if (tcp_fd < 0) {
        logmsg("SQ disconnected; dropping command (not queued)");
        return;
    }
    /* One send attempt only. A short/failed write is ambiguous: close and never replay. */
    ssize_t sent = send(tcp_fd, midi, midi_len, MSG_NOSIGNAL);
    if (sent != (ssize_t)midi_len) {
        logmsg("SQ MIDI write failed/partial (%zd of %zu); dropping connection, not retrying",
               sent, midi_len);
        close(tcp_fd);
        tcp_fd = -1;
        next_connect = time(NULL) + 2;
        return;
    }
    logmsg("Sent %zu MIDI bytes to SQ", midi_len);
}

static int make_udp(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(OSC_PORT);
    if (inet_pton(AF_INET, OSC_BIND_IP, &a.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        logmsg("UDP bind %s:%d failed: %s", OSC_BIND_IP, OSC_PORT, strerror(errno));
        close(fd); return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

static void try_connect(void) {
    if (tcp_fd >= 0 || time(NULL) < next_connect) return;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { next_connect = time(NULL) + 3; return; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(SQ_PORT);
    inet_pton(AF_INET, SQ_IP, &a.sin_addr);
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        logmsg("SQ connect %s:%d failed: %s", SQ_IP, SQ_PORT, strerror(errno));
        close(fd); next_connect = time(NULL) + 3; return;
    }
    tcp_fd = fd;
    logmsg("Connected to SQ at %s:%d; draining initial TCP data", SQ_IP, SQ_PORT);

    /* SQ sends a surface-state burst on connect. Wait for data, then drain;
       require 100 ms of quiet before considering the initial burst complete. */
    unsigned char buf[4096];
    for (;;) {
        struct pollfd p = { .fd = tcp_fd, .events = POLLIN, .revents = 0 };
        int ready = poll(&p, 1, 100);
        if (ready == 0) break; /* quiet interval */
        if (ready < 0) {
            if (errno == EINTR) continue;
            logmsg("SQ poll failed during initial drain: %s", strerror(errno));
            close(tcp_fd); tcp_fd = -1; next_connect = time(NULL) + 3;
            break;
        }
        ssize_t n = recv(tcp_fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) continue;
        if (n == 0) {
            logmsg("SQ closed connection during initial state drain");
            close(tcp_fd); tcp_fd = -1; next_connect = time(NULL) + 3;
            break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        logmsg("SQ read failed during initial drain: %s", strerror(errno));
        close(tcp_fd); tcp_fd = -1; next_connect = time(NULL) + 3;
        break;
    }
}

int main(void) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    udp_fd = make_udp();
    if (udp_fd < 0) return 1;
    logmsg("SQ OSC bridge listening on %s:%d; SQ=%s:%d; MIDI channel=%d",
           OSC_BIND_IP, OSC_PORT, SQ_IP, SQ_PORT, MIDI_CH);

    unsigned char packet[MAX_PACKET];
    while (running) {
        try_connect();
        struct pollfd pfd = { .fd = udp_fd, .events = POLLIN, .revents = 0 };
        int r = poll(&pfd, 1, 200);
        if (r < 0) {
            if (errno == EINTR) continue;
            logmsg("poll failed: %s", strerror(errno));
            break;
        }
        if (r > 0 && (pfd.revents & POLLIN)) {
            for (;;) {
                ssize_t n = recvfrom(udp_fd, packet, sizeof(packet), 0, NULL, NULL);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    logmsg("UDP receive failed: %s", strerror(errno));
                    break;
                }
                handle_osc(packet, (size_t)n, 0);
            }
        }
        if (tcp_fd >= 0) {
            unsigned char discard[1024];
            ssize_t n = recv(tcp_fd, discard, sizeof(discard), MSG_DONTWAIT);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                logmsg("SQ connection lost");
                close(tcp_fd); tcp_fd = -1; next_connect = time(NULL) + 2;
            }
        }
    }
    if (tcp_fd >= 0) close(tcp_fd);
    if (udp_fd >= 0) close(udp_fd);
    logmsg("Stopped");
    return 0;
}

