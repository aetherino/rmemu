// Framebuffer capture: grab the QImage xochitl renders the screen into and
// push dirty rectangles to rmemud, which relays them to the host.
//
// Display socket protocol (little-endian), shim -> rmemud:
//   hello: "RMFB" u16 width u16 height
//   rect:  'F' u16 x u16 y u16 w u16 h u32 zlen, zlib(RGBA rows)
//   activity: 'A'  (the panel is being driven; sent at most every 100ms)
// rmemud -> shim: 'R' = resend the whole screen (a new viewer joined).
#include "common.h"
#include "devices.h"
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define W RMEMU_SCREEN_W
#define H RMEMU_SCREEN_H
#define TILE 60  // divides both 1620 and 2160

static const uint32_t *screen;  // xochitl's RGB32 framebuffer (0xffRRGGBB)
static long screen_bpl;
static uint32_t *shadow;        // what rmemud last got
static volatile int poke;       // set on display activity; makes the scanner look now

static int send_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int send_rect(int fd, int x, int y, int w, int h) {
    size_t raw_len = (size_t)w * h * 4;
    unsigned char *raw = malloc(raw_len);
    for (int r = 0; r < h; r++) {
        const uint32_t *src = (const uint32_t *)((const char *)screen + (long)(y + r) * screen_bpl) + x;
        unsigned char *dst = raw + (size_t)r * w * 4;
        for (int c = 0; c < w; c++) {
            uint32_t px = src[c];
            dst[c * 4] = px >> 16; dst[c * 4 + 1] = px >> 8; dst[c * 4 + 2] = px; dst[c * 4 + 3] = 255;
        }
        memcpy(shadow + (size_t)(y + r) * W + x, src, (size_t)w * 4);
    }
    uLongf zlen = compressBound(raw_len);
    unsigned char *msg = malloc(13 + zlen);
    int rc = -1;
    if (compress2(msg + 13, &zlen, raw, raw_len, 1) == Z_OK) {
        msg[0] = 'F';
        uint16_t hdr[4] = { (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h };
        memcpy(msg + 1, hdr, 8);
        uint32_t zl = (uint32_t)zlen;
        memcpy(msg + 9, &zl, 4);
        rc = send_all(fd, msg, 13 + zlen);
    }
    free(raw); free(msg);
    return rc;
}

static int tile_dirty(int tx, int ty) {
    for (int r = 0; r < TILE; r++) {
        int y = ty * TILE + r;
        const uint32_t *src = (const uint32_t *)((const char *)screen + (long)y * screen_bpl) + tx * TILE;
        if (memcmp(src, shadow + (size_t)y * W + tx * TILE, TILE * 4)) return 1;
    }
    return 0;
}

// Send each horizontal band of dirty tiles as one rect spanning its dirty columns.
static int scan(int fd) {
    for (int ty = 0; ty < H / TILE; ty++) {
        int x0 = -1, x1 = -1;
        for (int tx = 0; tx < W / TILE; tx++)
            if (tile_dirty(tx, ty)) { if (x0 < 0) x0 = tx; x1 = tx; }
        if (x0 >= 0 && send_rect(fd, x0 * TILE, ty * TILE, (x1 - x0 + 1) * TILE, TILE)) return -1;
    }
    return 0;
}

static int connect_display(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX, .sun_path = RMEMU_DISPLAY_SOCK };
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) { if (fd >= 0) close(fd); return -1; }
    unsigned char hello[8] = { 'R', 'M', 'F', 'B' };
    uint16_t dims[2] = { W, H };
    memcpy(hello + 4, dims, 4);
    if (send_all(fd, hello, sizeof hello) || send_rect(fd, 0, 0, W, H)) { close(fd); return -1; }
    LOG("fb: connected to rmemud");
    return fd;
}

extern uint64_t drm_commits;

static void *scanner(void *arg) {
    (void)arg;
    int fd = -1, idle = 0, quiet_ticks = 0;
    uint64_t seen = 0;
    for (;;) {
        if (fd < 0 && (fd = connect_display()) < 0) { sleep(1); continue; }
        // Poll ~60Hz right after display activity, ~4Hz otherwise; wake early on 'R'.
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 16) > 0) {
            char c;
            if (recv(fd, &c, 1, 0) <= 0) { close(fd); fd = -1; continue; }
            if (c == 'R' && send_rect(fd, 0, 0, W, H)) { close(fd); fd = -1; continue; }
        }
        if (poke) { poke = 0; idle = 0; }
        // Waveforms keep running after the image changes; tell the host so
        // "wait until idle" covers the whole refresh.
        if (drm_commits != seen && ++quiet_ticks >= 6) {
            seen = drm_commits; quiet_ticks = 0;
            if (send_all(fd, "A", 1)) { close(fd); fd = -1; continue; }
        }
        if (idle++ > 30 && idle % 15) continue;
        if (scan(fd)) { close(fd); fd = -1; }
    }
    return NULL;
}

static void on_flip(void) { poke = 1; }

static void found_screen(const void *data, long bpl) {
    screen = data; screen_bpl = bpl;
    if (!shadow) shadow = calloc((size_t)W * H, 4);
    TRACE("fb: captured screen buffer %p (bpl %ld)", data, bpl);
    static int started;
    if (started++) return;
    drm_on_flip = on_flip;
    pthread_t th;
    pthread_create(&th, NULL, scanner, NULL);
}

// xochitl's epaper renderer paints the whole UI into a 1620x2160 RGB32 QImage
// wrapped around SWTCON-owned memory; SWTCON diffs it into waveform phases.
// Grab it as it's constructed. (Other full-screen QImages, e.g. the boot
// splash pixmap, are transient.)
void _ZN6QImageC1EPhiixNS_6FormatEPFvPvES2_(void *self, unsigned char *data, int w, int h, long long bpl, int fmt,
                                             void (*cleanup)(void *), void *info) {
    static void (*real)(void *, unsigned char *, int, int, long long, int, void (*)(void *), void *);
    if (!real) real = dlsym(RTLD_NEXT, "_ZN6QImageC1EPhiixNS_6FormatEPFvPvES2_");
    real(self, data, w, h, bpl, fmt, cleanup, info);
    if (w == W && h == H && fmt == 4 /* Format_RGB32 */) found_screen(data, (long)bpl);
}

void fb_init(void) {}
