// Framebuffer capture: grab the QImage xochitl renders the screen into and
// push dirty rectangles to rmemud, which relays them to the host.
//
// Display socket protocol (little-endian), shim -> rmemud:
//   hello: "RMFB" u16 width u16 height
//   rect:  'F' u16 x u16 y u16 w u16 h u32 zlen, zlib(RGBA rows)
//   wave:  'W' u16 x u16 y u16 w u16 h u8 kind u8 flags u16 ms
//          A panel update starts in this region: kind is UPD_* (common.h),
//          flags SWTCON's update flags, ms how long its waveform drives the
//          panel. Sent before the region's 'F' rects, so a viewer still has the
//          old image to animate from.
//   activity: 'A'  (the panel is being driven; sent at most every 100ms)
// rmemud -> shim: 'R' = resend the whole screen (a new viewer joined).
//
// Once updates.c reports xochitl's panel updates, rects go out when xochitl
// hands a region to the panel (like the real screen, nothing shows before
// that), with a slow sweep as a safety net. Until then, or for other apps that
// render into a screen-sized QImage, the screen is polled.
#include "common.h"
#include "devices.h"
#include <fcntl.h>
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

// How long each kind of update drives the panel: the main pass of SWTCON's
// phase frames for UI, content and full refreshes, measured at the fake
// sensor's 23 °C. Mono is a guess (a DU-style waveform); pen strokes and
// animations use fast waveforms that read as instant.
static const uint16_t wave_ms[] = { [UPD_MONO] = 250, [UPD_UI] = 500, [UPD_CONTENT] = 600, [UPD_FULL] = 700 };

struct update { int x, y, w, h, kind; unsigned flags; };
static struct update queue[64];
static int queued, overflow;
static volatile int follows_updates;  // updates.c has reported at least one
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static int wake[2] = { -1, -1 };      // fb_update() -> scanner

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

static int send_wave(int fd, const struct update *u) {
    unsigned char m[13] = { 'W' };
    uint16_t v[4] = { (uint16_t)u->x, (uint16_t)u->y, (uint16_t)u->w, (uint16_t)u->h };
    memcpy(m + 1, v, 8);
    m[9] = (unsigned char)u->kind;
    m[10] = (unsigned char)u->flags;
    uint16_t ms = u->kind < (int)(sizeof wave_ms / sizeof *wave_ms) ? wave_ms[u->kind] : wave_ms[UPD_UI];
    memcpy(m + 11, &ms, 2);
    return send_all(fd, m, sizeof m);
}

void fb_update(int x, int y, int w, int h, int kind, unsigned flags) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    pthread_mutex_lock(&qlock);
    if (queued < (int)(sizeof queue / sizeof *queue)) queue[queued++] = (struct update){ x, y, w, h, kind, flags };
    else overflow = 1;
    follows_updates = 1;
    pthread_mutex_unlock(&qlock);
    if (wake[1] >= 0 && write(wake[1], "", 1) < 0) { /* already awake */ }
}

static int tile_dirty(int tx, int ty) {
    for (int r = 0; r < TILE; r++) {
        int y = ty * TILE + r;
        const uint32_t *src = (const uint32_t *)((const char *)screen + (long)y * screen_bpl) + tx * TILE;
        if (memcmp(src, shadow + (size_t)y * W + tx * TILE, TILE * 4)) return 1;
    }
    return 0;
}

// Within pixel bounds [x0,x1) x [y0,y1), send each horizontal band of dirty
// tiles as one rect spanning its dirty columns. Returns how many went out (-1
// on error); a dry run just reports whether anything is dirty.
static int scan(int fd, int x0, int y0, int x1, int y1, int dry) {
    int n = 0;
    for (int ty = y0 / TILE; ty < (y1 + TILE - 1) / TILE; ty++) {
        int c0 = -1, c1 = -1;
        for (int tx = x0 / TILE; tx < (x1 + TILE - 1) / TILE; tx++)
            if (tile_dirty(tx, ty)) { if (dry) return 1; if (c0 < 0) c0 = tx; c1 = tx; }
        if (c0 < 0) continue;
        if (send_rect(fd, c0 * TILE, ty * TILE, (c1 - c0 + 1) * TILE, TILE)) return -1;
        n++;
    }
    return n;
}

// Announce queued panel updates, then send their pixels.
static int send_updates(int fd, int *pen_only) {
    struct update u[64];
    pthread_mutex_lock(&qlock);
    int n = queued, all = overflow;
    memcpy(u, queue, sizeof *u * n);
    queued = overflow = 0;
    pthread_mutex_unlock(&qlock);
    int x0 = W, y0 = H, x1 = 0, y1 = 0;
    *pen_only = !all;
    for (int i = 0; i < n; i++) {
        if (u[i].kind != UPD_PEN) {
            *pen_only = 0;
            if (send_wave(fd, &u[i])) return -1;
        }
        if (u[i].x < x0) x0 = u[i].x;
        if (u[i].y < y0) y0 = u[i].y;
        if (u[i].x + u[i].w > x1) x1 = u[i].x + u[i].w;
        if (u[i].y + u[i].h > y1) y1 = u[i].y + u[i].h;
    }
    if (all) return scan(fd, 0, 0, W, H, 0);
    return x0 < x1 ? scan(fd, x0, y0, x1, y1, 0) : 0;
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
    int fd = -1, idle = 0, quiet_ticks = 0, stale = 0;
    uint64_t seen = 0;
    for (;;) {
        if (fd < 0 && (fd = connect_display()) < 0) { sleep(1); continue; }
        // Tick at ~60Hz; wake early on 'R' or a panel update.
        struct pollfd p[2] = { { fd, POLLIN, 0 }, { wake[0], POLLIN, 0 } };
        if (poll(p, 2, 16) > 0) {
            char c[64];
            if (p[1].revents & POLLIN) while (read(wake[0], c, sizeof c) > 0) ;
            if (p[0].revents & (POLLIN | POLLHUP)) {
                if (recv(fd, c, 1, 0) <= 0) { close(fd); fd = -1; continue; }
                if (c[0] == 'R' && send_rect(fd, 0, 0, W, H)) { close(fd); fd = -1; continue; }
            }
        }
        if (poke) { poke = 0; idle = 0; }
        // Waveforms keep running after the image changes; tell the host so
        // "wait until idle" covers the whole refresh.
        if (drm_commits != seen && ++quiet_ticks >= 6) {
            seen = drm_commits; quiet_ticks = 0;
            if (send_all(fd, "A", 1)) { close(fd); fd = -1; continue; }
        }
        if (follows_updates) {
            int pen_only;
            if (__atomic_load_n(&queued, __ATOMIC_RELAXED) || __atomic_load_n(&overflow, __ATOMIC_RELAXED)) {
                idle = stale = 0;
                if (send_updates(fd, &pen_only) < 0) { close(fd); fd = -1; continue; }
                // Pen strokes come as hundreds of tiny updates a second; batch them.
                if (pen_only) usleep(6000);
                continue;
            }
            // Safety net for pixels that reach the screen unannounced: send them
            // once they've sat through a whole sweep (xochitl paints right
            // before it swaps, so anything announced is gone by then).
            if (++idle % 15) continue;
            if (!stale) { stale = scan(fd, 0, 0, W, H, 1) > 0; continue; }
            stale = 0;
        } else {
            // Poll ~60Hz right after display activity, ~4Hz otherwise.
            if (idle++ > 30 && idle % 15) continue;
        }
        if (scan(fd, 0, 0, W, H, 0) < 0) { close(fd); fd = -1; }
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
    if (pipe2(wake, O_CLOEXEC | O_NONBLOCK)) LOG("fb: no wake pipe, polling only");
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
