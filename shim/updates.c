// Panel updates: which regions xochitl sends to the e-ink panel, and with
// which class of waveform, so the simulator can play the refresh back.
//
// SWTCON is linked into xochitl, so there's no function to hook. It does log
// every swapBuffers() to the rm.framebuffer.updates category, though:
//
//   swapBuffers: QFlags(0x1) QRegion(0,0 1620x2160)
//    - full update, STD
//   swapBuffers: QFlags() QRegion(size=2, bounds=(0,0 1620x2160) - [(0,0 1620x420), (0,430 1620x1730)])
//    - pen .......: QRegion(600,420 420x10)
//    - ui ........: QRegion(0,0 1620x2160)
//
// We switch that category on and wrap xochitl's message handler to read the
// lines (and keep them out of the log unless QT_LOGGING_RULES asks for them).
#include "common.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// Qt 6 ABI: QString is a QArrayDataPointer<char16_t> { d, ptr, size }; a null
// d marks static data (what QStringLiteral builds). QMessageLogContext is
// { int version, line; const char *file, *function, *category; }.
struct qstring { void *d; const uint16_t *ptr; long size; };
struct qlogctx { int version, line; const char *file, *function, *category; };
typedef void (*qt_handler)(int type, const struct qlogctx *ctx, const struct qstring *msg);

#define CATEGORY "rm.framebuffer.updates"

static qt_handler next;   // xochitl's own handler (NULL: Qt's default)
static int show_lines;    // the user enabled the category themselves
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct { unsigned flags; int n; int r[16][4]; } swap;  // the current swapBuffers() call

// Collect "x,y wxh" rects from a QRegion(...) dump. Multi-rect regions start
// with their bounds, which we skip in favour of the parts.
static int parse_region(const char *s, int out[16][4]) {
    const char *q = strstr(s, "QRegion(");
    if (!q) return -1;
    int n = 0, skip = strstr(q, "bounds=") != NULL;
    for (const char *p = q; (p = strchr(p, '(')) && n < 16; p++) {
        int x, y, w, h;
        if (sscanf(p + 1, "%d,%d %dx%d", &x, &y, &w, &h) != 4 || w <= 0 || h <= 0) continue;
        if (skip) { skip = 0; continue; }
        out[n][0] = x; out[n][1] = y; out[n][2] = w; out[n][3] = h;
        n++;
    }
    return n;
}

static int kind_of(const char *label) {
    static const struct { const char *prefix; int kind; } kinds[] = {
        { "pen", UPD_PEN }, { "animation", UPD_ANIMATION }, { "partial animation", UPD_ANIMATION },
        { "full screen animation", UPD_ANIMATION }, { "mono", UPD_MONO }, { "ui", UPD_UI },
        { "content", UPD_CONTENT }, { "full update", UPD_FULL }, { "sleep", UPD_FULL },
        { "merged", -1 },  // a note that ui and content went out together; both are listed already
    };
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++)
        if (!strncmp(label, kinds[i].prefix, strlen(kinds[i].prefix))) return kinds[i].kind;
    return UPD_UI;
}

static void parse_line(const char *s) {
    while (*s == ' ') s++;
    if (!strncmp(s, "swapBuffers:", 12)) {
        swap.flags = 0;  // "QFlags(0x2|0x8)"
        const char *p = strstr(s, "QFlags("), *end = p ? strchr(p, ')') : NULL;
        while (p && (p = strstr(p, "0x")) && p < end) swap.flags |= (unsigned)strtoul(p, (char **)&p, 16);
        swap.n = parse_region(s, swap.r);
        if (swap.n < 0) swap.n = 0;
        return;
    }
    if (strncmp(s, "- ", 2)) return;
    int kind = kind_of(s + 2);
    if (kind < 0) return;
    int r[16][4], n = parse_region(s, r);
    if (n < 0) { memcpy(r, swap.r, sizeof r); n = swap.n; }  // "- full update, STD" covers the swap region
    for (int i = 0; i < n; i++) fb_update(r[i][0], r[i][1], r[i][2], r[i][3], kind, swap.flags);
}

static void handler(int type, const struct qlogctx *ctx, const struct qstring *msg) {
    if (ctx && ctx->category && !strcmp(ctx->category, CATEGORY) && msg) {
        char line[2048];
        long n = msg->size < (long)sizeof line - 1 ? msg->size : (long)sizeof line - 1;
        for (long i = 0; i < n; i++) line[i] = msg->ptr[i] < 128 ? (char)msg->ptr[i] : '?';
        line[n] = 0;
        pthread_mutex_lock(&lock);
        parse_line(line);
        pthread_mutex_unlock(&lock);
        if (!show_lines) return;
    }
    if (next) next(type, ctx, msg);
    else if (msg) {  // Qt's default handler isn't exported; print like it would
        for (long i = 0; i < msg->size; i++) fputc(msg->ptr[i] < 128 ? msg->ptr[i] : '?', stderr);
        fputc('\n', stderr);
    }
}

// xochitl installs its handler early in main(), once Qt is up: a good moment
// to switch the category on. Rules from QT_LOGGING_RULES still win over ours.
qt_handler _Z22qInstallMessageHandlerPFv9QtMsgTypeRK18QMessageLogContextRK7QStringE(qt_handler h) {
    static qt_handler (*real)(qt_handler);
    static int started;
    if (!real) real = dlsym(RTLD_NEXT, "_Z22qInstallMessageHandlerPFv9QtMsgTypeRK18QMessageLogContextRK7QStringE");
    if (strcmp(program_invocation_short_name, "xochitl")) return real(h);  // leave other apps' logging alone
    if (!started++) {
        void (*set_rules)(const struct qstring *) = dlsym(RTLD_DEFAULT, "_ZN16QLoggingCategory14setFilterRulesERK7QString");
        static const uint16_t rule[] = u"" CATEGORY ".debug=true";
        struct qstring s = { NULL, rule, sizeof rule / 2 - 1 };
        if (set_rules) set_rules(&s);
        const char *env = getenv("QT_LOGGING_RULES");
        show_lines = env && strstr(env, "rm.framebuffer");
        TRACE("updates: following %s%s", CATEGORY, set_rules ? "" : " FAIL (no QLoggingCategory::setFilterRules)");
    }
    qt_handler prev = next;
    next = h;
    real(handler);
    return prev;
}
