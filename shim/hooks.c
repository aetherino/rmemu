// libc interposition: routes hardware paths to fakes (DRM, sysfs tree) and
// logs device access so missing hardware shows up in the container log.
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sched.h>
#include <sys/timerfd.h>
#include <time.h>

int rmemu_debug;
static const char *fakeroot = "/opt/rmemu/root";

static int hw_path(const char *p) {
    return p && (!strncmp(p, "/dev/", 5) || !strncmp(p, "/sys/", 5) || !strncmp(p, "/proc/device-tree", 17));
}

// Redirect hardware paths into the fake tree when a fake file exists there.
static const char *redirect(const char *path, char *buf, size_t n) {
    if (!hw_path(path)) return path;
    snprintf(buf, n, "%s%s", fakeroot, path);
    static int (*real_faccessat)(int, const char *, int, int);
    if (!real_faccessat) real_faccessat = dlsym(RTLD_NEXT, "faccessat");
    return real_faccessat(AT_FDCWD, buf, F_OK, 0) == 0 ? buf : path;
}

static int do_open(int dirfd, const char *path, int flags, mode_t mode) {
    if (path && !strcmp(path, "/dev/dri/card0")) return drm_open();
    if (path && !strncmp(path, "/dev/input/", 11)) {
        int fd = input_open(path, flags);
        if (fd != -2) return fd;
    }
    char buf[512];
    const char *p = redirect(path, buf, sizeof buf);
    REAL(openat);
    int fd = real_openat(dirfd, p, flags, mode);
    if (hw_path(path)) TRACE("open %s%s -> %d%s", path, p != path ? " (fake)" : "", fd, fd < 0 ? " FAIL" : "");
    return fd;
}

#define MODE_ARG mode_t mode = 0; if (flags & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
int open(const char *path, int flags, ...) { MODE_ARG; return do_open(AT_FDCWD, path, flags, mode); }
int open64(const char *path, int flags, ...) { MODE_ARG; return do_open(AT_FDCWD, path, flags, mode); }
int __open_2(const char *path, int flags) { return do_open(AT_FDCWD, path, flags, 0); }
int __open64_2(const char *path, int flags) { return do_open(AT_FDCWD, path, flags, 0); }
int openat(int dirfd, const char *path, int flags, ...) { MODE_ARG; return do_open(dirfd, path, flags, mode); }
int openat64(int dirfd, const char *path, int flags, ...) { MODE_ARG; return do_open(dirfd, path, flags, mode); }

FILE *fopen(const char *path, const char *m) {
    REAL(fopen);
    char buf[512];
    const char *p = redirect(path, buf, sizeof buf);
    FILE *f = real_fopen(p, m);
    if (hw_path(path)) TRACE("fopen %s%s -> %s", path, p != path ? " (fake)" : "", f ? "ok" : "FAIL");
    return f;
}
FILE *fopen64(const char *path, const char *m) { return fopen(path, m); }

DIR *opendir(const char *path) {
    REAL(opendir);
    char buf[512];
    const char *p = redirect(path, buf, sizeof buf);
    DIR *d = real_opendir(p);
    if (hw_path(path)) TRACE("opendir %s%s -> %s", path, p != path ? " (fake)" : "", d ? "ok" : "FAIL");
    return d;
}

// Path-taking calls that must see the same fake tree as open().
#define PATH_HOOK(ret, name, fail, params, args)                                        \
    ret name params {                                                                   \
        REAL(name);                                                                     \
        char buf[512];                                                                  \
        const char *orig = path;                                                        \
        path = redirect(path, buf, sizeof buf);                                         \
        ret r = real_##name args;                                                       \
        if (hw_path(orig)) TRACE(#name " %s%s -> %s", orig, path != orig ? " (fake)" : "", \
                               r == fail ? "FAIL" : "ok");                              \
        return r;                                                                       \
    }
PATH_HOOK(int, stat, -1, (const char *path, struct stat *st), (path, st))
PATH_HOOK(int, lstat, -1, (const char *path, struct stat *st), (path, st))
PATH_HOOK(int, stat64, -1, (const char *path, struct stat64 *st), (path, st))
PATH_HOOK(int, lstat64, -1, (const char *path, struct stat64 *st), (path, st))
PATH_HOOK(int, access, -1, (const char *path, int mode), (path, mode))
PATH_HOOK(ssize_t, readlink, -1, (const char *path, char *b, size_t n), (path, b, n))
PATH_HOOK(char *, realpath, NULL, (const char *path, char *resolved), (path, resolved))
PATH_HOOK(int, fstatat, -1, (int dirfd, const char *path, struct stat *st, int fl), (dirfd, path, st, fl))
PATH_HOOK(int, fstatat64, -1, (int dirfd, const char *path, struct stat64 *st, int fl), (dirfd, path, st, fl))
PATH_HOOK(int, faccessat, -1, (int dirfd, const char *path, int mode, int fl), (dirfd, path, mode, fl))
PATH_HOOK(int, statx, -1, (int dirfd, const char *path, int fl, unsigned mask, struct statx *st), (dirfd, path, fl, mask, st))

int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (drm_is_fd(fd)) return drm_ioctl(req, arg);
    if (input_is_fd(fd)) return input_ioctl(fd, req, arg);
    REAL(ioctl);
    return real_ioctl(fd, req, arg);
}

int close(int fd) {
    REAL(close);
    if (input_is_fd(fd)) input_close(fd);
    return real_close(fd);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    if (drm_is_fd(fd)) return drm_mmap(addr, len, prot, flags, off);
    REAL(mmap);
    return real_mmap(addr, len, prot, flags, fd, off);
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) { return mmap(addr, len, prot, flags, fd, off); }

// SWTCON wants realtime priority for its vsync/framegen threads and reads it
// back to verify. A container can't grant that and timing here doesn't need
// it, so remember what was asked for and report it.
static struct { pthread_t t; int policy, prio; } fake_sched[32];
static pthread_mutex_t sched_lock = PTHREAD_MUTEX_INITIALIZER;

int pthread_setschedparam(pthread_t t, int policy, const struct sched_param *p) {
    REAL(pthread_setschedparam);
    int r = real_pthread_setschedparam(t, policy, p);
    if (r != EPERM) return r;
    pthread_mutex_lock(&sched_lock);
    int i;
    for (i = 0; i < 32 && fake_sched[i].t && !pthread_equal(fake_sched[i].t, t); i++) ;
    if (i < 32) fake_sched[i].t = t, fake_sched[i].policy = policy, fake_sched[i].prio = p ? p->sched_priority : 0;
    pthread_mutex_unlock(&sched_lock);
    return 0;
}
int pthread_getschedparam(pthread_t t, int *policy, struct sched_param *p) {
    REAL(pthread_getschedparam);
    int r = real_pthread_getschedparam(t, policy, p);
    pthread_mutex_lock(&sched_lock);
    for (int i = 0; i < 32 && fake_sched[i].t; i++)
        if (pthread_equal(fake_sched[i].t, t)) { *policy = fake_sched[i].policy; p->sched_priority = fake_sched[i].prio; }
    pthread_mutex_unlock(&sched_lock);
    return r;
}
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *p) {
    REAL(sched_setscheduler);
    int r = real_sched_setscheduler(pid, policy, p);
    if (r < 0 && errno == EPERM) { TRACE("sched_setscheduler faked"); errno = 0; r = 0; }
    return r;
}

// Wake-from-suspend alarm clocks need CAP_WAKE_ALARM; nothing suspends here,
// so the plain clock behaves the same.
int timerfd_create(int clockid, int flags) {
    REAL(timerfd_create);
    int fd = real_timerfd_create(clockid, flags);
    if (fd < 0 && errno == EPERM && (clockid == CLOCK_REALTIME_ALARM || clockid == CLOCK_BOOTTIME_ALARM)) {
        TRACE("timerfd_create: alarm clock %d -> plain clock", clockid);
        fd = real_timerfd_create(clockid == CLOCK_REALTIME_ALARM ? CLOCK_REALTIME : CLOCK_BOOTTIME, flags);
    }
    return fd;
}

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    REAL(pthread_create);
    int r = real_pthread_create(t, attr, fn, arg);
    if (r == EPERM && attr) {
        // Explicit realtime scheduling attrs; fall back to inheriting ours.
        pthread_attr_setinheritsched((pthread_attr_t *)attr, PTHREAD_INHERIT_SCHED);
        r = real_pthread_create(t, attr, fn, arg);
        TRACE("pthread_create with realtime attrs -> retried without: %d", r);
    }
    return r;
}

void *dlopen(const char *file, int mode) {
    REAL(dlopen);
    // OP-TEE's PKCS#11 client needs the secure world; use our software token.
    if (file && strstr(file, "libckteec.so")) file = "/opt/rmemu/rmemu-pkcs11.so";
    void *h = real_dlopen(file, mode);
    if (file && !strstr(file, "/plugins/")) TRACE("dlopen %s -> %s", file, h ? "ok" : dlerror());
    return h;
}

__attribute__((constructor)) static void init(void) {
    const char *d = getenv("RMEMU_DEBUG");
    rmemu_debug = (d && strcmp(d, "0")) || access("/run/rmemu-debug", F_OK) == 0;
    if (getenv("RMEMU_FAKEROOT")) fakeroot = getenv("RMEMU_FAKEROOT");
    // xochitl's digitizer discovery would otherwise scan for a real Elan device.
    setenv("XOCHITL_DIGITIZER_PATH", "/dev/input/event1", 0);
    // Only xochitl gets the full treatment; child processes just inherit path fakes.
    TRACE("loaded into pid %d", getpid());
    fb_init();
}
