// doomgeneric backend for UniFi UDR (recovery mode):
//   video -> /dev/fb0  (panel is 80x160, 16bpp RGB565)
//   input <- stdin     (raw keys over telnet, with synthetic key-release)
//   audio -> added in a later phase (FEATURE_SOUND + DG_sound_module)
//
// Doom renders into DG_ScreenBuffer at DOOMGENERIC_RESX x DOOMGENERIC_RESY
// (default 640x400), each pixel 0x00RRGGBB. We box-average that down into a
// "logical" landscape image, then rotate/flip it onto the portrait panel.
//
// Tunables (env vars):
//   DOOM_FB        framebuffer device           (default /dev/fb0)
//   DOOM_ROT       0 | 90 | 180 | 270           (default 90)
//   DOOM_FLIP      0 | 1  mirror image          (default 0)
//   DOOM_LETTERBOX 0 | 1  keep 4:3-ish aspect   (default 0 = stretch to fill)
//   DOOM_HOLD_MS   key auto-release window ms   (default 350)

#include "doomkeys.h"
#include "doomgeneric.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

// ------------------------------------------------------------------ helpers
static const char *env_def(const char *k, const char *d) {
    const char *v = getenv(k);
    return (v && *v) ? v : d;
}
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}

// ------------------------------------------------------------------ framebuffer
static int       fb_fd  = -1;
static uint8_t  *fb_mem = NULL;
static size_t    fb_size = 0;
static int       FBW, FBH;        // panel dimensions in pixels
static long      FBSTRIDE;        // bytes per fb line
static int       r_off, r_len, g_off, g_len, b_off, b_len;

static int       g_rot  = 90;     // 0 / 90 / 180 / 270
static int       g_flip = 0;
static int       g_letterbox = 0;

static int       LW, LH;          // logical image dims (pre-rotation)
static uint16_t *logical = NULL;

static inline uint16_t pack565(int r, int g, int b) {
    return (uint16_t)(((r >> (8 - r_len)) << r_off) |
                      ((g >> (8 - g_len)) << g_off) |
                      ((b >> (8 - b_len)) << b_off));
}

static void fb_init(void) {
    const char *dev = env_def("DOOM_FB", "/dev/fb0");
    fb_fd = open(dev, O_RDWR);
    if (fb_fd < 0) { perror("open framebuffer"); exit(1); }

    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &f) < 0) { perror("FSCREENINFO"); exit(1); }
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) < 0) { perror("VSCREENINFO"); exit(1); }

    FBW = v.xres; FBH = v.yres; FBSTRIDE = f.line_length;
    if (FBSTRIDE <= 0) FBSTRIDE = (long)FBW * v.bits_per_pixel / 8;

    if (v.bits_per_pixel != 16)
        fprintf(stderr, "[doom-udr] WARN: fb is %u bpp, backend assumes 16 (RGB565)\n",
                v.bits_per_pixel);

    r_off = v.red.offset;   r_len = v.red.length   ? v.red.length   : 5;
    g_off = v.green.offset; g_len = v.green.length ? v.green.length : 6;
    b_off = v.blue.offset;  b_len = v.blue.length  ? v.blue.length  : 5;
    if (r_off == 0 && g_off == 0 && b_off == 0) {   // panel left bitfields blank
        r_off = 11; r_len = 5; g_off = 5; g_len = 6; b_off = 0; b_len = 5;
    }

    fb_size = f.smem_len ? f.smem_len : (size_t)FBSTRIDE * FBH;
    fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) { perror("mmap framebuffer"); exit(1); }

    g_rot       = atoi(env_def("DOOM_ROT", "90"));
    g_flip      = atoi(env_def("DOOM_FLIP", "0"));
    g_letterbox = atoi(env_def("DOOM_LETTERBOX", "0"));

    if (g_rot == 90 || g_rot == 270) { LW = FBH; LH = FBW; }  // landscape
    else                             { LW = FBW; LH = FBH; }  // portrait
    logical = calloc((size_t)LW * LH, sizeof(uint16_t));
    if (!logical) { fprintf(stderr, "[doom-udr] OOM logical buffer\n"); exit(1); }

    printf("[doom-udr] fb %dx%d %ubpp stride=%ld  logical=%dx%d rot=%d flip=%d lb=%d\n",
           FBW, FBH, v.bits_per_pixel, FBSTRIDE, LW, LH, g_rot, g_flip, g_letterbox);
}

// Box-average DG_ScreenBuffer (SW x SH, 0x00RRGGBB) into the logical buffer.
static void scale_to_logical(void) {
    const int SW = DOOMGENERIC_RESX, SH = DOOMGENERIC_RESY;
    const uint32_t *src = (const uint32_t *)DG_ScreenBuffer;

    int dx0 = 0, dy0 = 0, dw = LW, dh = LH;
    if (g_letterbox) {
        if ((long)LW * SH > (long)LH * SW) { dh = LH; dw = (int)((long)LH * SW / SH); }
        else                               { dw = LW; dh = (int)((long)LW * SH / SW); }
        if (dw < 1) dw = 1; if (dh < 1) dh = 1;
        dx0 = (LW - dw) / 2; dy0 = (LH - dh) / 2;
        memset(logical, 0, (size_t)LW * LH * sizeof(uint16_t));
    }

    for (int oy = 0; oy < dh; oy++) {
        int sy0 = (int)((long)oy * SH / dh);
        int sy1 = (int)((long)(oy + 1) * SH / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > SH) sy1 = SH;
        uint16_t *outrow = logical + (size_t)(dy0 + oy) * LW + dx0;
        for (int ox = 0; ox < dw; ox++) {
            int sx0 = (int)((long)ox * SW / dw);
            int sx1 = (int)((long)(ox + 1) * SW / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > SW) sx1 = SW;
            uint32_t rs = 0, gs = 0, bs = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint32_t *sp = src + (size_t)sy * SW + sx0;
                for (int sx = sx0; sx < sx1; sx++) {
                    uint32_t p = *sp++;
                    rs += (p >> 16) & 0xff;
                    gs += (p >> 8) & 0xff;
                    bs += p & 0xff;
                    n++;
                }
            }
            if (!n) n = 1;
            outrow[ox] = pack565((int)(rs / n), (int)(gs / n), (int)(bs / n));
        }
    }
}

static void blit_logical_to_fb(void) {
    for (int ly = 0; ly < LH; ly++) {
        for (int lx = 0; lx < LW; lx++) {
            int sx = g_flip ? (LW - 1 - lx) : lx;
            uint16_t px = logical[(size_t)ly * LW + sx];
            int fx, fy;
            switch (g_rot) {
                case 0:   fx = lx;           fy = ly;           break;
                case 180: fx = FBW - 1 - lx; fy = FBH - 1 - ly; break;
                case 270: fx = FBW - 1 - ly; fy = FBH - 1 - lx; break;
                case 90:
                default:  fx = ly;           fy = lx;           break;
            }
            if ((unsigned)fx < (unsigned)FBW && (unsigned)fy < (unsigned)FBH)
                *(uint16_t *)(fb_mem + (size_t)fy * FBSTRIDE + (size_t)fx * 2) = px;
        }
    }
}

// ------------------------------------------------------------------ input
#define KQS 64
static unsigned short kq[KQS];
static unsigned kqw = 0, kqr = 0;
static void kq_push(int pressed, unsigned char k) {
    kq[kqw] = (unsigned short)(((pressed ? 1 : 0) << 8) | k);
    kqw = (kqw + 1) % KQS;
    if (kqw == kqr) kqr = (kqr + 1) % KQS;   // drop oldest on overflow
}

static uint64_t key_expire[256];   // 0 == up
static int      hold_ms = 350;

static void key_stimulus(unsigned char dk) {
    if (!dk) return;
    if (key_expire[dk] == 0) kq_push(1, dk);   // rising edge only
    key_expire[dk] = now_ms() + hold_ms;
}
static void expire_keys(void) {
    uint64_t now = now_ms();
    for (int i = 0; i < 256; i++)
        if (key_expire[i] && now >= key_expire[i]) {
            kq_push(0, (unsigned char)i);
            key_expire[i] = 0;
        }
}

static int map_ascii(unsigned char c) {
    switch (c) {
        case '\r': case '\n': return KEY_ENTER;
        case '\t':            return KEY_TAB;
        case 0x7f: case 0x08: return KEY_BACKSPACE;
        case ' ':             return KEY_USE;
        case 0x1b:            return KEY_ESCAPE;
    }
    unsigned char l = (unsigned char)tolower(c);
    switch (l) {
        case 'w': return KEY_UPARROW;
        case 's': return KEY_DOWNARROW;
        case 'a': return KEY_STRAFE_L;
        case 'd': return KEY_STRAFE_R;
        case 'q': return KEY_LEFTARROW;    // turn left
        case 'e': return KEY_RIGHTARROW;   // turn right
        case 'f': return KEY_FIRE;
        case 'r': return KEY_RSHIFT;       // run (hold)
        case 'y': return 'y';
        case 'n': return 'n';
    }
    if (c >= '0' && c <= '9') return c;
    if (c == '+' || c == '=') return KEY_EQUALS;
    if (c == '-' || c == '_') return KEY_MINUS;
    return 0;
}

static unsigned char inbuf[256];
static int           inlen = 0;
static uint64_t      esc_pending = 0;

static void pump_input(void) {
    for (;;) {
        unsigned char tmp[128];
        ssize_t r = read(0, tmp, sizeof(tmp));
        if (r <= 0) break;
        int room = (int)sizeof(inbuf) - inlen;
        if (room <= 0) { inlen = 0; room = sizeof(inbuf); }   // overflow guard
        if (r > room) r = room;
        memcpy(inbuf + inlen, tmp, r);
        inlen += r;
        if (r < (ssize_t)sizeof(tmp)) break;
    }

    int i = 0;
    while (i < inlen) {
        unsigned char c = inbuf[i];
        if (c == 0x1b) {
            if (i + 1 >= inlen) {                 // lone ESC at tail
                if (esc_pending == 0) esc_pending = now_ms();
                else if (now_ms() - esc_pending >= 30) {
                    key_stimulus(KEY_ESCAPE); esc_pending = 0; i++; continue;
                }
                break;                            // wait one grace period
            }
            esc_pending = 0;
            unsigned char n1 = inbuf[i + 1];
            if (n1 == '[' || n1 == 'O') {
                if (i + 2 >= inlen) break;         // incomplete CSI
                unsigned char n2 = inbuf[i + 2];
                int dk = 0;
                switch (n2) {
                    case 'A': dk = KEY_UPARROW;    break;
                    case 'B': dk = KEY_DOWNARROW;  break;
                    case 'C': dk = KEY_RIGHTARROW; break;
                    case 'D': dk = KEY_LEFTARROW;  break;
                    case 'H': dk = KEY_HOME;       break;
                    case 'F': dk = KEY_END;        break;
                    default:
                        if (n2 >= '0' && n2 <= '9') {   // ESC[<num>~
                            int j = i + 3;
                            while (j < inlen && inbuf[j] != '~') j++;
                            if (j >= inlen) goto compact;   // incomplete, keep
                            i = j + 1; continue;
                        }
                        i += 3; continue;          // unknown final, swallow
                }
                key_stimulus((unsigned char)dk);
                i += 3; continue;
            } else {
                key_stimulus(KEY_ESCAPE); i++; continue;   // ESC + other
            }
        }
        key_stimulus((unsigned char)map_ascii(c));
        i++;
    }
compact:
    if (i > 0) {
        if (i >= inlen) inlen = 0;
        else { memmove(inbuf, inbuf + i, inlen - i); inlen -= i; }
    }
    expire_keys();
}

// ------------------------------------------------------------------ termios
static struct termios orig_tio;
static int            tio_saved = 0;

static void restore(void) {
    if (tio_saved) { tcsetattr(0, TCSANOW, &orig_tio); tio_saved = 0; }
}
static void on_signal(int s) { (void)s; restore(); _exit(0); }

static void input_init(void) {
    if (tcgetattr(0, &orig_tio) == 0) {
        struct termios t = orig_tio;
        t.c_lflag &= ~(ICANON | ECHO | IEXTEN);   // keep ISIG so Ctrl-C quits
        t.c_iflag &= ~(IXON | ICRNL | INLCR | ISTRIP);
        t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
        tio_saved = 1;
    }
    int fl = fcntl(0, F_GETFL, 0);
    fcntl(0, F_SETFL, fl | O_NONBLOCK);
    atexit(restore);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);
    hold_ms = atoi(env_def("DOOM_HOLD_MS", "350"));
    if (hold_ms < 50) hold_ms = 50;
}

// ------------------------------------------------------------------ DG API
void DG_Init(void) {
    fb_init();
    input_init();
}

void DG_DrawFrame(void) {
    scale_to_logical();
    blit_logical_to_fb();
    pump_input();
}

void DG_SleepMs(uint32_t ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

uint32_t DG_GetTicksMs(void) {
    static uint64_t t0 = 0;
    if (!t0) t0 = now_ms();
    return (uint32_t)(now_ms() - t0);
}

int DG_GetKey(int *pressed, unsigned char *key) {
    if (kqr == kqw) return 0;
    unsigned short d = kq[kqr];
    kqr = (kqr + 1) % KQS;
    *pressed = d >> 8;
    *key = d & 0xff;
    return 1;
}

void DG_SetWindowTitle(const char *title) { (void)title; }

int main(int argc, char **argv) {
    fprintf(stderr,
        "\n=== DOOM on UniFi UDR ===\n"
        "Move: W/S or Up/Down   Turn: Left/Right or Q/E   Strafe: A/D\n"
        "Fire: F   Use/Open: Space   Run: hold R   Weapons: 1-7\n"
        "Map: Tab   Menu: Esc   Select: Enter   Quit: Ctrl-C\n"
        "Orientation env: DOOM_ROT=0/90/180/270  DOOM_FLIP=1  DOOM_HOLD_MS=ms\n\n");
    doomgeneric_Create(argc, argv);
    for (;;) doomgeneric_Tick();
    return 0;
}
