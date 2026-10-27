// Bible reader for Miyoo Mini / Mini Plus on Onion OS.
// Draws straight to /dev/fb0 (rotated 180, like Onion's own screenshot code) and reads buttons from /dev/input/event0,
// so it needs no terminal and no SDL. Text: data/<kjv|web>/NN.txt made by make_data.py. Font: Literata (OFL).
// Build (device):  zig cc -target arm-linux-musleabihf -mcpu=cortex_a7 -O2 -static bible.c -o bible -lm
// Build (preview): zig cc -target x86_64-windows-gnu -DPREVIEW -O2 bible.c -o bible_preview.exe
//                  bible_preview.exe <key> <key> ...   -> frames/NNN.png after every key
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_X, K_Y, K_L1, K_R1, K_L2, K_R2, K_SELECT, K_START, K_MENU, K_QUIT };
static const char *KEYNAMES[] __attribute__((unused)) = { "up", "down", "left", "right", "a", "b", "x", "y", "l1", "r1", "l2", "r2", "select", "start", "menu" };
static int W = 640, H = 480;
static uint32_t *px;                         // back buffer, 0xRRGGBB

// ---------------------------------------------------------------- platform
#ifdef PREVIEW
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
static char **pkeys; static int npkeys, ipkey, frame;
static int plat_init(void)
{
    if (getenv("W")) W = atoi(getenv("W"));
    if (getenv("H")) H = atoi(getenv("H"));
    px = malloc(W * H * 4);
    return 0;
}
static void plat_present(void)
{
    char fn[64]; unsigned char *rgb = malloc(W * H * 3);
    for (int i = 0; i < W * H; i++) { rgb[i * 3] = px[i] >> 16; rgb[i * 3 + 1] = px[i] >> 8; rgb[i * 3 + 2] = px[i]; }
    sprintf(fn, "frames/%03d.png", frame++);
    stbi_write_png(fn, W, H, 3, rgb, W * 3); free(rgb);
}
static int plat_key(void)
{
    while (ipkey < npkeys) {
        const char *k = pkeys[ipkey++];
        for (int i = 0; i < K_QUIT; i++) if (!strcmp(k, KEYNAMES[i])) return i;
    }
    return K_QUIT;
}
static void plat_sync(void) {}
#else
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
static int fbfd = -1, infd = -1, stride, bpp;
static uint8_t *fbmem;
static struct fb_var_screeninfo vinfo;
static volatile sig_atomic_t quit_flag;
static void on_signal(int s) { (void)s; quit_flag = 1; }
static int plat_init(void)
{
    struct fb_fix_screeninfo finfo;
    if ((fbfd = open("/dev/fb0", O_RDWR)) < 0) return -1;
    ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo);
    ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo);
    vinfo.yoffset = 0;                          // show page 0 (same as Onion's display_reset)
    ioctl(fbfd, FBIOPUT_VSCREENINFO, &vinfo);
    ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo);
    W = vinfo.xres; H = vinfo.yres; bpp = vinfo.bits_per_pixel / 8;
    stride = finfo.line_length ? (int)finfo.line_length : (int)vinfo.xres_virtual * bpp;
    fbmem = mmap(0, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbmem == MAP_FAILED) return -1;
    px = malloc(W * H * 4);
    infd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGHUP, on_signal);
    return 0;
}
static void plat_present(void)                  // the panel is mounted upside down: rotate 180
{
    uint8_t *base = fbmem + vinfo.yoffset * stride;
    for (int y = 0; y < H; y++) {
        const uint32_t *s = px + y * W;
        if (bpp == 4) {
            uint32_t *d = (uint32_t *)(base + (H - 1 - y) * stride) + (W - 1);
            for (int x = 0; x < W; x++) *d-- = s[x] | 0xFF000000;
        }
        else {
            uint16_t *d = (uint16_t *)(base + (H - 1 - y) * stride) + (W - 1);
            for (int x = 0; x < W; x++) *d-- = ((s[x] >> 8) & 0xF800) | ((s[x] >> 5) & 0x07E0) | ((s[x] >> 3) & 0x001F);
        }
    }
}
static long long now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}
static int map_key(int code)
{
    switch (code) {             // Onion keymap_hw.h
    case KEY_UP: return K_UP;           case KEY_DOWN: return K_DOWN;
    case KEY_LEFT: return K_LEFT;       case KEY_RIGHT: return K_RIGHT;
    case KEY_SPACE: return K_A;         case KEY_LEFTCTRL: return K_B;
    case KEY_LEFTSHIFT: return K_X;     case KEY_LEFTALT: return K_Y;
    case KEY_E: return K_L1;            case KEY_T: return K_R1;
    case KEY_TAB: return K_L2;          case KEY_BACKSPACE: return K_R2;
    case KEY_RIGHTCTRL: return K_SELECT; case KEY_ENTER: return K_START;
    case KEY_ESC: return K_MENU;
    }
    return -1;
}
// Held D-pad keys repeat. Menu only counts when released without another button in between,
// so Onion's Menu+button shortcuts (brightness etc.) don't quit the reader.
static int plat_key(void)
{
    static int queue[32], qn, rep_key = -1, menu_down, menu_combo;
    static long long rep_at;
    for (;;) {
        if (quit_flag) return K_QUIT;
        if (qn) { int k = queue[0]; memmove(queue, queue + 1, --qn * sizeof(int)); return k; }
        int timeout = -1;
        if (rep_key >= 0) { long long d = rep_at - now_ms(); timeout = d < 0 ? 0 : (int)d; }
        struct pollfd p = { infd, POLLIN, 0 };
        int r = poll(&p, 1, timeout);
        if (r < 0) { if (errno == EINTR) continue; return K_QUIT; }
        if (r == 0) {
            if (rep_key >= 0) { rep_at = now_ms() + (rep_key <= K_DOWN ? 60 : 140); return rep_key; }
            continue;
        }
        struct input_event ev[16];
        ssize_t n = read(infd, ev, sizeof ev);
        for (int i = 0; i < n / (ssize_t)sizeof ev[0]; i++) {
            if (ev[i].type != EV_KEY) continue;
            int k = map_key(ev[i].code);
            if (k < 0) continue;
            if (ev[i].value == 1) {
                if (k == K_MENU) { menu_down = 1; menu_combo = 0; continue; }
                if (menu_down) { menu_combo = 1; continue; }
                if (k <= K_RIGHT) { rep_key = k; rep_at = now_ms() + (k <= K_DOWN ? 320 : 420); }
                if (qn < 32) queue[qn++] = k;
            }
            else if (ev[i].value == 0) {
                if (k == K_MENU) { if (menu_down && !menu_combo && qn < 32) queue[qn++] = K_MENU; menu_down = 0; }
                if (k == rep_key) rep_key = -1;
            }
        }
    }
}
static void plat_sync(void) { sync(); }
#endif

// ---------------------------------------------------------------- drawing
typedef struct { uint32_t bg, fg, dim, accent, bar, line, seltext, red; } Theme;
static const Theme THEMES[2] = {
    { 0xF6F0E2, 0x2A2420, 0x8C8072, 0x9A2A2A, 0xEAE1CC, 0xD8CDB4, 0xFFF8EC, 0xB8201C },   // day: warm paper
    { 0x14120F, 0xD9D0C1, 0x7D7468, 0xD08A5C, 0x1F1C17, 0x34302A, 0x14120F, 0xE8705E },   // night
};

static inline uint32_t mix(uint32_t d, uint32_t c, int a)
{
    int ia = 255 - a;
    return ((((c & 0xFF00FF) * a + (d & 0xFF00FF) * ia) >> 8) & 0xFF00FF) | ((((c & 0xFF00) * a + (d & 0xFF00) * ia) >> 8) & 0xFF00);
}
static void fill(int x, int y, int w, int h, uint32_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px[(y + j) * W + x + i] = c;
}
static void blend_px(int x, int y, uint32_t c, int a)
{
    if (x >= 0 && x < W && y >= 0 && y < H) px[y * W + x] = mix(px[y * W + x], c, a);
}
static void rrect(int x, int y, int w, int h, int r, uint32_t c)      // anti-aliased corners
{
    for (int j = 0; j < h; j++) {
        float dy = j < r ? r - j - 0.5f : j >= h - r ? j - (h - r) + 0.5f : 0;
        float in = dy > 0 ? r - sqrtf(r * r - dy * dy) : 0;
        int ii = (int)in, a = (int)((1 - (in - ii)) * 255);
        fill(x + ii + 1, y + j, w - 2 * (ii + 1), 1, c);
        blend_px(x + ii, y + j, c, a);
        blend_px(x + w - 1 - ii, y + j, c, a);
    }
}
static void darken(int a) { for (int i = 0; i < W * H; i++) px[i] = mix(px[i], 0, a); }

// ---------------------------------------------------------------- fonts
typedef struct { int cp, gi, x0, y0, w, h; float adv; unsigned char *bmp; } Glyph;
typedef struct { int size; float scale; int asc, lineh; Glyph tab[512]; } Face;
static stbtt_fontinfo font;
static Face *faces[24];
static int nfaces;

static Face *face(int size)
{
    for (int i = 0; i < nfaces; i++) if (faces[i]->size == size) return faces[i];
    if (nfaces == 24) {                             // font size changed a lot: drop everything
        for (int i = 0; i < nfaces; i++) { for (int g = 0; g < 512; g++) free(faces[i]->tab[g].bmp); free(faces[i]); }
        nfaces = 0;
    }
    Face *f = calloc(1, sizeof *f);
    int a, d, g;
    f->size = size; f->scale = stbtt_ScaleForPixelHeight(&font, size);
    stbtt_GetFontVMetrics(&font, &a, &d, &g);
    f->asc = (int)ceilf(a * f->scale);
    f->lineh = (int)ceilf((a - d + g) * f->scale * 1.12f);
    return faces[nfaces++] = f;
}
static Glyph *glyph(Face *f, int cp)
{
    unsigned h = (unsigned)cp * 2654435761u % 512;
    for (int i = 0; i < 512; i++, h = (h + 1) % 512) {
        Glyph *g = &f->tab[h];
        if (g->cp == cp && (g->bmp || g->adv)) return g;
        if (!g->bmp && !g->adv) {
            int adv, lsb;
            g->cp = cp; g->gi = stbtt_FindGlyphIndex(&font, cp);
            stbtt_GetGlyphHMetrics(&font, g->gi, &adv, &lsb);
            g->adv = adv * f->scale + 0.0001f;
            g->bmp = stbtt_GetGlyphBitmap(&font, f->scale, f->scale, g->gi, &g->w, &g->h, &g->x0, &g->y0);
            return g;
        }
    }
    return &f->tab[0];
}
#define RED_ON 1
#define RED_OFF 2
static const Theme *T;
static int utf8(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    if (s[0] < 0x80) { *p += 1; return s[0]; }
    if ((s[0] & 0xE0) == 0xC0 && s[1]) { *p += 2; return ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) { *p += 3; return ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); }
    *p += 1; return '?';
}
static float kern(Face *f, int a, int b)               // GPOS lookups are slow on the Mini: cache per glyph pair
{
    static struct { int key, k; } cache[4096];
    int key = (a << 16 | b) + 1;
    unsigned h = (unsigned)key * 2654435761u >> 20;
    if (cache[h].key != key) { cache[h].key = key; cache[h].k = stbtt_GetGlyphKernAdvance(&font, a, b); }
    return cache[h].k * f->scale;
}
static float text_w(Face *f, const char *s, int n)
{
    const char *e = s + n; float x = 0; int prev = 0;
    while (s < e) {
        int cp = utf8(&s);
        if (cp == RED_ON || cp == RED_OFF) continue;
        Glyph *g = glyph(f, cp);
        if (prev) x += kern(f, prev, g->gi);
        x += g->adv; prev = g->gi;
    }
    return x;
}
// Words of Jesus are wrapped in RED_ON/RED_OFF bytes; *red carries the state from line to line.
static float draw_text_red(Face *f, float x, int base, const char *s, int n, uint32_t normal, int *red)
{
    const char *e = s + n; int prev = 0;
    uint32_t col = *red ? T->red : normal;
    while (s < e) {
        int cp = utf8(&s);
        if (cp == RED_ON) { *red = 1; col = T->red; continue; }
        if (cp == RED_OFF) { *red = 0; col = normal; continue; }
        Glyph *g = glyph(f, cp);
        if (prev) x += kern(f, prev, g->gi);
        int gx = (int)(x + 0.5f) + g->x0, gy = base + g->y0;
        for (int j = 0; j < g->h; j++) {
            int y = gy + j;
            if (y < 0 || y >= H) continue;
            for (int i = 0; i < g->w; i++) {
                int a = g->bmp[j * g->w + i], xx = gx + i;
                if (!a || xx < 0 || xx >= W) continue;
                uint32_t *d = &px[y * W + xx];
                *d = a == 255 ? col : mix(*d, col, a);
            }
        }
        x += g->adv; prev = g->gi;
    }
    return x;
}
static float draw_text(Face *f, float x, int base, const char *s, int n, uint32_t col)
{
    int red = 0;
    return draw_text_red(f, x, base, s, n, col, &red);
}
static void draw_str(Face *f, int x, int base, const char *s, uint32_t col) { draw_text(f, x, base, s, strlen(s), col); }
static void draw_center(Face *f, int cx, int base, const char *s, uint32_t col) { draw_text(f, cx - text_w(f, s, strlen(s)) / 2, base, s, strlen(s), col); }
static void draw_right(Face *f, int rx, int base, const char *s, uint32_t col) { draw_text(f, rx - text_w(f, s, strlen(s)), base, s, strlen(s), col); }

// ---------------------------------------------------------------- bible data
#define NBOOKS 66
static const char *VERS[2] = { "kjv", "web" }, *VLABEL[2] = { "KJV", "WEB" };
static char bname[NBOOKS][24];
static int bchaps[NBOOKS];
typedef struct { char kind; int num; char *text; } Item;     // kind 'V' verse, 'H' heading
static char *bookbuf, *btitle;
static Item *items;
static int nitems, chstart[152], nch, loaded_ver = -1, loaded_book = -1;

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    n = fread(b, 1, n, f); b[n] = 0; fclose(f);
    return b;
}
static int load_index(void)
{
    char *b = read_file("data/kjv/index.txt"), *p = b;
    if (!b) return -1;
    for (int i = 0; i < NBOOKS && *p; i++) {
        char *tab = strchr(p, '\t');
        if (!tab) return -1;
        snprintf(bname[i], sizeof bname[i], "%.*s", (int)(tab - p), p);
        bchaps[i] = atoi(tab + 1);
        p = strchr(tab, '\n'); p = p ? p + 1 : tab + strlen(tab);
    }
    free(b);
    return 0;
}
static int load_book(int ver, int book)
{
    char path[64];
    if (ver == loaded_ver && book == loaded_book) return 0;
    snprintf(path, sizeof path, "data/%s/%02d.txt", VERS[ver], book + 1);
    char *b = read_file(path);
    if (!b) return -1;
    free(bookbuf); free(items);
    bookbuf = b; btitle = ""; nitems = nch = 0;
    int lines = 1;
    for (char *p = b; *p; p++) lines += *p == '\n';
    items = malloc(lines * sizeof *items);
    for (char *p = b; *p;) {
        char *l = p, *e = strchr(p, '\n');
        if (e) { *e = 0; p = e + 1; } else p += strlen(p);
        if (e && e > l && e[-1] == '\r') e[-1] = 0;
        if (l[0] == 'T') btitle = l + 2;
        else if (l[0] == 'C' && nch < 151) chstart[nch++] = nitems;
        else if (l[0] == 'H') items[nitems++] = (Item){ 'H', 0, l + 2 };
        else if (l[0] == 'V') { char *t; int n = strtol(l + 2, &t, 10); items[nitems++] = (Item){ 'V', n, t + 1 }; }
    }
    chstart[nch] = nitems;
    loaded_ver = ver; loaded_book = book;
    return 0;
}

// ---------------------------------------------------------------- state
static const int SIZES[] = { 18, 20, 22, 24, 26, 28, 30, 33, 36, 40 };
#define NSIZES (int)(sizeof SIZES / sizeof SIZES[0])
static int ver = 1, book = 0, chap = 1, sizeidx = 4, night = 0, helpseen = 0, start_verse = 1;

static void load_state(void)
{
    FILE *f = fopen("state.txt", "r");
    if (!f) { if (H >= 560) sizeidx = 6; return; }
    if (fscanf(f, "%d %d %d %d %d %d %d", &ver, &book, &chap, &start_verse, &sizeidx, &night, &helpseen) != 7) helpseen = 0;
    fclose(f);
    if (ver < 0 || ver > 1) ver = 1;
    if (book < 0 || book >= NBOOKS) book = 0;
    if (chap < 1 || chap > bchaps[book]) chap = 1;
    if (sizeidx < 0 || sizeidx >= NSIZES) sizeidx = 4;
    night = !!night;
}
static void save_state(int verse)
{
    FILE *f = fopen("state.tmp", "w");
    if (!f) return;
    fprintf(f, "%d %d %d %d %d %d %d\n", ver, book, chap, verse, sizeidx, night, helpseen);
    fclose(f);
    remove("state.txt");
    rename("state.tmp", "state.txt");
}

// ---------------------------------------------------------------- reader layout
enum { L_TITLE, L_CHAP, L_HEAD, L_TEXT };
typedef struct { const char *s; int n, kind, verse, x, y, h, red; char num[5]; } Line;   // red: line starts inside red text
static Line *lines;
static int nlines, cap_lines, top, maxtop;
static int BAR, VIEW_Y, VIEW_H, MARGIN, GUTTER;

static Face *f_body(void) { return face(SIZES[sizeidx]); }
static Face *f_num(void) { return face(SIZES[sizeidx] * 55 / 100 > 11 ? SIZES[sizeidx] * 55 / 100 : 11); }
static Face *f_head(void) { return face(SIZES[sizeidx] * 80 / 100); }
static Face *f_chap(void) { return face(SIZES[sizeidx] * 3 / 2); }
static Face *f_title(void) { return face(SIZES[sizeidx] * 62 / 100 > 14 ? SIZES[sizeidx] * 62 / 100 : 14); }
static Face *f_ui(void) { return face(H / 22); }
static Face *f_small(void) { return face(H / 28); }

static Line *add_line(const char *s, int n, int kind, int verse, int x, int h)
{
    if (nlines == cap_lines) { cap_lines = cap_lines ? cap_lines * 2 : 256; lines = realloc(lines, cap_lines * sizeof *lines); }
    Line *l = &lines[nlines++];
    l->s = s; l->n = n; l->kind = kind; l->verse = verse; l->x = x; l->h = h; l->num[0] = 0;
    l->y = nlines > 1 ? lines[nlines - 2].y + lines[nlines - 2].h : 0;
    return l;
}
// Greedy word wrap; x = -1 centres the line.
static Line *wrap(Face *f, const char *s, int kind, int verse, int x, int maxw, int h)
{
    Line *first = NULL;
    const char *p = s;
    int red = 0;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *ls = p, *le = p, *q = p;
        for (;;) {
            while (*q && *q != ' ') q++;
            if (le != ls && text_w(f, ls, q - ls) > maxw) break;
            le = q;
            if (!*q) break;
            while (*q == ' ') q++;
        }
        Line *l = add_line(ls, le - ls, kind, verse, x, h);
        l->red = red;
        for (const char *c = ls; c < le; c++) if (*c == RED_ON) red = 1; else if (*c == RED_OFF) red = 0;
        if (!first) first = l;
        p = le;
    }
    return first;
}
static void layout(void)
{
    Face *body = f_body(), *num = f_num(), *head = f_head(), *chf = f_chap(), *tf = f_title();
    BAR = H / 13; VIEW_Y = BAR + H / 40; VIEW_H = H - VIEW_Y - H / 40;
    MARGIN = W / 26;
    GUTTER = MARGIN + (int)text_w(num, "176", 3) + body->size / 4;
    int textw = W - GUTTER - MARGIN;
    nlines = 0;
    if (chap == 1 && *btitle) {
        wrap(tf, btitle, L_TITLE, 0, -1, W - 2 * MARGIN, tf->lineh);
        lines[nlines - 1].h += tf->lineh / 3;
    }
    add_line(NULL, 0, L_CHAP, 0, -1, chf->lineh + body->lineh / 3);
    for (int i = chstart[chap - 1]; i < chstart[chap]; i++) {
        Item *it = &items[i];
        if (it->kind == 'H') {
            wrap(head, it->text, L_HEAD, i + 1 < chstart[chap] ? items[i + 1].num : 0, GUTTER, textw, head->lineh);
            lines[nlines - 1].h += body->lineh / 4;
        }
        else {
            Line *l = wrap(body, it->text, L_TEXT, it->num, GUTTER, textw, body->lineh);
            if (l) snprintf(l->num, sizeof l->num, "%d", it->num);
            lines[nlines - 1].h += body->lineh / 5;
        }
    }
    // last top line that still fills the screen
    int total = nlines ? lines[nlines - 1].y + lines[nlines - 1].h : 0;
    maxtop = 0;
    while (maxtop < nlines - 1 && total - lines[maxtop].y > VIEW_H) maxtop++;
}
static int line_of_verse(int v)
{
    if (v <= 1) return 0;
    for (int i = 0; i < nlines; i++) if (lines[i].kind >= L_HEAD && lines[i].verse >= v) return i < maxtop ? i : maxtop;
    return maxtop;
}
static int top_verse(void)
{
    for (int i = top; i < nlines; i++) if (lines[i].kind == L_TEXT) return top == 0 ? 1 : lines[i].verse;
    return 1;
}
static int open_chapter(int b, int c, int verse)
{
    if (load_book(ver, b)) return -1;
    book = b; chap = c < 1 ? 1 : c > nch ? nch : c;
    layout();
    top = verse < 0 ? maxtop : line_of_verse(verse);
    save_state(top_verse());
    return 0;
}
static void relayout_keep_place(void)
{
    int v = top_verse();
    loaded_ver = -1;
    open_chapter(book, chap, v);
}
static void next_chapter(int dir, int at_end)
{
    int b = book, c = chap + dir;
    if (c < 1) { if (--b < 0) return; c = bchaps[b]; }
    else if (c > bchaps[b]) { if (++b >= NBOOKS) return; c = 1; }
    open_chapter(b, c, at_end ? -1 : 1);
}
static void page(int dir)
{
    if (dir > 0) {
        if (top >= maxtop) { next_chapter(1, 0); return; }
        int t = top;
        while (t < nlines && lines[t].y + lines[t].h - lines[top].y <= VIEW_H) t++;
        top = t > maxtop ? maxtop : t == top ? top + 1 : t;
    }
    else {
        if (top == 0) { next_chapter(-1, 1); return; }
        int t = top;
        while (t > 0 && lines[top].y - lines[t - 1].y <= VIEW_H) t--;
        top = t;
    }
}

// ---------------------------------------------------------------- screens
enum { S_READER, S_BOOKS, S_CHAPTERS };
static int screen = S_READER, show_help, sel, browse_book, list_scroll;

static void draw_bar(const char *left, const char *right, int progress)
{
    Face *ui = f_ui();
    fill(0, 0, W, BAR, T->bar);
    fill(0, BAR - 1, W, 1, T->line);
    if (progress >= 0) fill(0, BAR - 2, W * progress / 100, 2, T->accent);
    int base = (BAR - ui->lineh) / 2 + ui->asc + 1;
    draw_str(ui, MARGIN, base, left, T->fg);
    draw_right(ui, W - MARGIN, base, right, T->dim);
}
static void draw_hints(const char *s)
{
    Face *sm = f_small();
    int y = H - sm->lineh - H / 60;
    fill(0, y - H / 60, W, H - y + H / 60, T->bar);
    fill(0, y - H / 60, W, 1, T->line);
    draw_center(sm, W / 2, y + sm->asc, s, T->dim);
}
static const char *chap_word(int b) { return strcmp(bname[b], "Psalms") ? "Chapter" : "Psalm"; }

static void draw_reader(void)
{
    char left[48], right[24];
    int total = nlines ? lines[nlines - 1].y + lines[nlines - 1].h : 1;
    int prog = maxtop == 0 || top >= maxtop ? 100 : (lines[top].y + VIEW_H) * 100 / total;
    snprintf(left, sizeof left, "%s %d", strcmp(bname[book], "Psalms") ? bname[book] : "Psalm", chap);
    snprintf(right, sizeof right, "%s  %d%%", VLABEL[ver], prog);
    fill(0, 0, W, H, T->bg);
    Face *body = f_body(), *num = f_num(), *head = f_head(), *chf = f_chap(), *tf = f_title();
    for (int i = top; i < nlines; i++) {
        Line *l = &lines[i];
        int y = VIEW_Y + l->y - lines[top].y;
        if (l->y + l->h - lines[top].y > VIEW_H) break;      // same rule as page()
        if (l->kind == L_TITLE) draw_text(tf, (W - text_w(tf, l->s, l->n)) / 2, y + tf->asc, l->s, l->n, T->dim);
        else if (l->kind == L_CHAP) {
            char s[24]; snprintf(s, sizeof s, "%s %d", chap_word(book), chap);
            draw_center(chf, W / 2, y + chf->asc, s, T->accent);
        }
        else if (l->kind == L_HEAD) draw_text(head, l->x, y + head->asc, l->s, l->n, T->dim);
        else {
            int base = y + body->asc;
            if (l->num[0]) draw_right(num, GUTTER - body->size / 4, base - body->asc / 3, l->num, T->accent);
            int red = l->red;
            draw_text_red(body, l->x, base, l->s, l->n, T->fg, &red);
        }
    }
    draw_bar(left, right, prog);
}
static void draw_help(void)
{
    static const char *rows[][2] = {
        { "Up / Down", "Scroll" }, { "Left / Right", "Page back / forward" }, { "A", "Next page" },
        { "L1 / R1", "Smaller / larger text" }, { "L2 / R2", "Previous / next chapter" },
        { "X", "Switch KJV / WEB" }, { "Y", "Day / night" }, { "B", "Chapters" }, { "Select", "Books" },
        { "Start", "This help" }, { "Menu", "Quit (keeps your place)" },
    };
    int n = sizeof rows / sizeof rows[0];
    Face *ui = f_ui(), *sm = f_small();
    int rowh = sm->lineh + H / 120, pw = W * 84 / 100, ph = ui->lineh * 2 + rowh * n + H / 14;
    int x = (W - pw) / 2, y = (H - ph) / 2;
    darken(150);
    rrect(x, y, pw, ph, H / 40, T->bg);
    draw_center(ui, W / 2, y + H / 40 + ui->asc, "Controls", T->accent);
    int ry = y + H / 40 + ui->lineh * 3 / 2, cx = x + pw * 40 / 100;
    for (int i = 0; i < n; i++, ry += rowh) {
        draw_right(sm, cx - H / 40, ry + sm->asc, rows[i][0], T->accent);
        draw_str(sm, cx + H / 40, ry + sm->asc, rows[i][1], T->fg);
    }
    draw_center(sm, W / 2, y + ph - H / 40 - sm->lineh + sm->asc, "Press any button", T->dim);
}

// Books: Old Testament (39) and New Testament (27), three columns each.
#define BCOLS 3
static int book_row_y(int i, int rowh, int headh)     // y of book i inside the scrolling list
{
    return i < 39 ? headh + (i / BCOLS) * rowh : headh * 2 + 13 * rowh + ((i - 39) / BCOLS) * rowh;
}
static void draw_books(void)
{
    Face *ui = f_ui(), *sm = f_small();
    int rowh = ui->lineh + H / 40, headh = sm->lineh + H / 30, listy = BAR, listh = H - BAR - (sm->lineh + H / 30);
    int cw = (W - 2 * MARGIN) / BCOLS, total = headh * 2 + 22 * rowh;
    int sy = book_row_y(sel, rowh, headh);
    int want = sel < BCOLS ? 0 : sel >= 39 && sel < 39 + BCOLS ? sy - headh : sy;   // first row: show the heading too
    if (want < list_scroll) list_scroll = want;
    if (sy + rowh - list_scroll > listh) list_scroll = sy + rowh - listh;
    if (list_scroll > total - listh) list_scroll = total - listh;
    if (list_scroll < 0) list_scroll = 0;
    fill(0, 0, W, H, T->bg);
    for (int part = 0; part < 2; part++) {
        int hy = listy - list_scroll + (part ? headh + 13 * rowh : 0);
        draw_str(sm, MARGIN, hy + headh - H / 80 - (sm->lineh - sm->asc), part ? "NEW TESTAMENT" : "OLD TESTAMENT", T->dim);
    }
    for (int i = 0; i < NBOOKS; i++) {
        int y = listy - list_scroll + book_row_y(i, rowh, headh), x = MARGIN + ((i < 39 ? i : i - 39) % BCOLS) * cw;
        if (y + rowh < listy || y > listy + listh) continue;
        uint32_t col = i == book ? T->accent : T->fg;
        if (i == sel) { rrect(x, y + 2, cw - H / 80, rowh - 4, H / 60, T->accent); col = T->seltext; }
        draw_str(ui, x + H / 40, y + (rowh - ui->lineh) / 2 + ui->asc, bname[i], col);
    }
    draw_bar("Books", VLABEL[ver], -1);
    draw_hints("A  Open      B  Back      L1 / R1  Old / New Testament      X  KJV / WEB");
}
static int chapter_cols(void) { return (W - 2 * MARGIN) / (H / 7); }
static void draw_chapters(void)
{
    Face *ui = f_ui(), *sm = f_small();
    int cols = chapter_cols(), cw = (W - 2 * MARGIN) / cols, chh = cw * 7 / 10, n = bchaps[browse_book];
    int listy = BAR + H / 60, listh = H - listy - (sm->lineh + H / 30), rows = (n + cols - 1) / cols;
    int sy = (sel / cols) * chh;
    if (sy < list_scroll) list_scroll = sy;
    if (sy + chh > list_scroll + listh) list_scroll = sy + chh - listh;
    if (list_scroll > rows * chh - listh) list_scroll = rows * chh - listh;
    if (list_scroll < 0) list_scroll = 0;
    fill(0, 0, W, H, T->bg);
    for (int i = 0; i < n; i++) {
        int x = MARGIN + (i % cols) * cw, y = listy + (i / cols) * chh - list_scroll;
        if (y + chh < listy || y > listy + listh) continue;
        char s[8]; snprintf(s, sizeof s, "%d", i + 1);
        uint32_t col = browse_book == book && i + 1 == chap ? T->accent : T->fg;
        if (i == sel) { rrect(x + 3, y + 3, cw - 6, chh - 6, H / 60, T->accent); col = T->seltext; }
        draw_center(ui, x + cw / 2, y + (chh - ui->lineh) / 2 + ui->asc, s, col);
    }
    char right[32]; snprintf(right, sizeof right, "%d %s", n, strcmp(bname[browse_book], "Psalms") ? (n == 1 ? "chapter" : "chapters") : "psalms");
    draw_bar(bname[browse_book], right, -1);
    draw_hints("A  Read      B  Books      L1 / R1  Previous / next book");
}
static void draw(void)
{
    T = &THEMES[night];
    if (screen == S_READER) { draw_reader(); if (show_help) draw_help(); }
    else if (screen == S_BOOKS) draw_books();
    else draw_chapters();
    plat_present();
}

static void open_books(void) { screen = S_BOOKS; sel = book; list_scroll = 0; }
static void open_chapters(int b) { screen = S_CHAPTERS; browse_book = b; sel = b == book ? chap - 1 : 0; list_scroll = 0; }

// returns 0 to quit
static int handle(int k)
{
    if (k == K_MENU || k == K_QUIT) return 0;
    if (show_help) { show_help = 0; if (!helpseen) { helpseen = 1; save_state(top_verse()); } return 1; }
    if (k == K_Y) { night = !night; save_state(top_verse()); return 1; }
    if (k == K_X) { ver = !ver; relayout_keep_place(); return 1; }
    if (screen == S_READER) {
        switch (k) {
        case K_UP: if (top > 0) top--; break;
        case K_DOWN: if (top < maxtop) top++; break;
        case K_LEFT: page(-1); break;
        case K_RIGHT: case K_A: page(1); break;
        case K_L1: if (sizeidx > 0) { sizeidx--; relayout_keep_place(); } break;
        case K_R1: if (sizeidx < NSIZES - 1) { sizeidx++; relayout_keep_place(); } break;
        case K_L2: next_chapter(-1, 0); break;
        case K_R2: next_chapter(1, 0); break;
        case K_B: open_chapters(book); break;
        case K_SELECT: open_books(); break;
        case K_START: show_help = 1; break;
        }
    }
    else if (screen == S_BOOKS) {
        int col = (sel < 39 ? sel : sel - 39) % BCOLS;
        switch (k) {
        case K_UP: if (sel >= BCOLS) sel -= BCOLS; break;
        case K_DOWN: if (sel + BCOLS < NBOOKS) sel += BCOLS; break;
        case K_LEFT: if (col > 0) sel--; break;
        case K_RIGHT: if (col < BCOLS - 1 && sel < NBOOKS - 1) sel++; break;
        case K_L1: sel = 0; break;
        case K_R1: sel = 39; break;
        case K_A: if (bchaps[sel] == 1) { open_chapter(sel, 1, 1); screen = S_READER; } else open_chapters(sel); break;
        case K_B: case K_SELECT: screen = S_READER; break;
        }
    }
    else {
        int cols = chapter_cols(), n = bchaps[browse_book];
        switch (k) {
        case K_UP: if (sel >= cols) sel -= cols; break;
        case K_DOWN: if (sel + cols < n) sel += cols; else if (sel / cols < (n - 1) / cols) sel = n - 1; break;
        case K_LEFT: if (sel > 0) sel--; break;
        case K_RIGHT: if (sel < n - 1) sel++; break;
        case K_L1: if (browse_book > 0) open_chapters(browse_book - 1); break;
        case K_R1: if (browse_book < NBOOKS - 1) open_chapters(browse_book + 1); break;
        case K_A: open_chapter(browse_book, sel + 1, 1); screen = S_READER; break;
        case K_B: sel = browse_book; screen = S_BOOKS; list_scroll = 0; break;
        case K_SELECT: open_books(); break;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
#ifdef PREVIEW
    pkeys = argv + 1; npkeys = argc - 1;
#else
    (void)argc;
    char dir[512]; snprintf(dir, sizeof dir, "%s", argv[0]);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; if (chdir(dir)) return 1; }
#endif
    unsigned char *ttf = (unsigned char *)read_file("Literata.ttf");
    if (!ttf || !stbtt_InitFont(&font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) return 1;
    if (load_index() || plat_init()) return 1;
    load_state();
    if (open_chapter(book, chap, start_verse)) return 1;
    show_help = !helpseen;
    draw();
    for (;;) {
        int k = plat_key();
        if (!handle(k)) break;
        draw();
    }
    save_state(top_verse());
    plat_sync();
    return 0;
}
