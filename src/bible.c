// Bible reader for Miyoo Mini / Mini Plus on Onion OS. Any number of translations, one folder each under data/.
// Draws straight to /dev/fb0 (rotated 180, like Onion's own screenshot code) and reads buttons from /dev/input/event0,
// so it needs no terminal and no SDL. Text: data/<id>/ made by make_data_intl.py. Fonts: Literata + Noto Serif (OFL), CJK fonts per translation.
// Build (device):  zig cc -target arm-linux-musleabihf -mcpu=cortex_a7 -O2 -static bible.c -o bible -lm
// Build (preview): zig cc -target x86_64-windows-gnu -DPREVIEW -O2 bible.c -o bible_preview.exe
//                  bible_preview.exe <key> <key> ...   -> frames/NNN.png after every key
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>
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
// Fonts, tried in order: Literata (the book face), Noto Serif (extra Latin / Cyrillic letters), the current
// translation's own font if it ships one (Chinese, Japanese, Korean), then two tiny fonts holding just the
// characters of the CJK language names for the language list. All share one em size.
#define NFONTS 5
typedef struct { int cp, gi, fi, x0, y0, w, h; float adv; unsigned char *bmp; } Glyph;
#define GCAP 2048
typedef struct { int size, used, asc, lineh; float scale[NFONTS]; Glyph tab[GCAP]; } Face;
static stbtt_fontinfo fonts[NFONTS];
static unsigned char *fontdata[NFONTS];
static int font_ok[NFONTS];
#define MAXFACES 24
static Face *faces[MAXFACES];
static int nfaces, red_letters = 1;

// Only call between frames: drawing code holds Face pointers. Done when the text size or the font set changes.
static void clear_faces(void)
{
    for (int i = 0; i < nfaces; i++) { for (int g = 0; g < GCAP; g++) free(faces[i]->tab[g].bmp); free(faces[i]); }
    nfaces = 0;
}
static Face *face(int size)
{
    for (int i = 0; i < nfaces; i++) if (faces[i]->size == size) return faces[i];
    if (nfaces == MAXFACES) {                   // full: borrow the closest size rather than free faces in use
        Face *best = faces[0];
        for (int i = 1; i < nfaces; i++) if (abs(faces[i]->size - size) < abs(best->size - size)) best = faces[i];
        return best;
    }
    Face *f = calloc(1, sizeof *f);
    int a, d, g;
    f->size = size;
    float em = stbtt_ScaleForPixelHeight(&fonts[0], size) * ttUSHORT(fonts[0].data + fonts[0].head + 18);   // Literata's em in pixels
    for (int k = 0; k < NFONTS; k++) if (font_ok[k]) f->scale[k] = stbtt_ScaleForMappingEmToPixels(&fonts[k], em);
    stbtt_GetFontVMetrics(&fonts[0], &a, &d, &g);
    f->asc = (int)ceilf(a * f->scale[0]);
    f->lineh = (int)ceilf((a - d + g) * f->scale[0] * 1.12f);
    return faces[nfaces++] = f;
}
static Glyph *glyph(Face *f, int cp)
{
    unsigned h = (unsigned)cp * 2654435761u % GCAP;
    for (int i = 0; i < GCAP; i++, h = (h + 1) % GCAP) {
        Glyph *g = &f->tab[h];
        if (g->adv && g->cp == cp) return g;
        if (g->adv) continue;
        if (f->used > GCAP * 3 / 4) {                           // a long Chinese chapter: start the cache over
            for (int k = 0; k < GCAP; k++) { free(f->tab[k].bmp); memset(&f->tab[k], 0, sizeof f->tab[k]); }
            f->used = 0;
            return glyph(f, cp);
        }
        int adv, lsb;
        g->cp = cp; g->fi = 0; g->gi = 0;
        for (int k = 0; k < NFONTS && !g->gi; k++) if (font_ok[k] && (g->gi = stbtt_FindGlyphIndex(&fonts[k], cp))) g->fi = k;
        stbtt_GetGlyphHMetrics(&fonts[g->fi], g->gi, &adv, &lsb);
        g->adv = adv * f->scale[g->fi] + 0.0001f;
        g->bmp = stbtt_GetGlyphBitmap(&fonts[g->fi], f->scale[g->fi], f->scale[g->fi], g->gi, &g->w, &g->h, &g->x0, &g->y0);
        f->used++;
        return g;
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
    if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) { *p += 4; return ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); }
    *p += 1; return '?';
}
static float kern(Face *f, const Glyph *a, const Glyph *b)    // GPOS lookups are slow on the Mini: cache per glyph pair
{
    static struct { int key, k; } cache[4096];
    if (a->fi != b->fi) return 0;
    int key = ((a->fi << 30) | (a->gi << 15) | b->gi) + 1;
    unsigned h = (unsigned)key * 2654435761u >> 20;
    if (cache[h].key != key) { cache[h].key = key; cache[h].k = stbtt_GetGlyphKernAdvance(&fonts[a->fi], a->gi, b->gi); }
    return cache[h].k * f->scale[a->fi];
}
static float text_w(Face *f, const char *s, int n)
{
    const char *e = s + n; float x = 0; Glyph *prev = NULL;
    while (s < e) {
        int cp = utf8(&s);
        if (cp == RED_ON || cp == RED_OFF) continue;
        Glyph *g = glyph(f, cp);
        if (prev) x += kern(f, prev, g);
        x += g->adv; prev = g;
    }
    return x;
}
// Words of Jesus are wrapped in RED_ON/RED_OFF bytes; *red carries the state from line to line.
static float draw_text_red(Face *f, float x, int base, const char *s, int n, uint32_t normal, int *red)
{
    const char *e = s + n; Glyph *prev = NULL;
    uint32_t col = *red && red_letters ? T->red : normal;
    while (s < e) {
        int cp = utf8(&s);
        if (cp == RED_ON) { *red = 1; col = red_letters ? T->red : normal; continue; }
        if (cp == RED_OFF) { *red = 0; col = normal; continue; }
        Glyph *g = glyph(f, cp);
        if (prev) x += kern(f, prev, g);
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
        x += g->adv; prev = g;
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
static float draw_fit(Face *f, int x, int base, const char *s, int maxw, uint32_t col)    // cut with "..." to fit
{
    int n = strlen(s);
    if (text_w(f, s, n) <= maxw) return draw_text(f, x, base, s, n, col);
    float dots = text_w(f, "...", 3);
    while (n > 0 && text_w(f, s, n) + dots > maxw) { n--; while (n > 0 && (s[n] & 0xC0) == 0x80) n--; }
    return draw_text(f, draw_text(f, x, base, s, n, col), base, "...", 3, col);
}

// ---------------------------------------------------------------- bible data
// data/<id>/meta.txt describes a translation, index.txt lists its books, NN.txt holds each book.
typedef struct {
    char id[32], lang[48], lang_en[48], title[96], label[48], sections[3][48], font[32], copyright[200], license[40], source[96];
    int breaks_any;                     // Chinese / Japanese: a line may break between any two characters
} Version;
typedef struct { char code[5], name[64]; int chapters, section; } Book;
static Version *vers;
static int nvers, cur_ver = -1, prev_ver = -1;
static Book books[100];
static int nbooks;
typedef struct { char kind; int num; char *text; } Item;     // kind 'V' verse, 'H' heading
static char *bookbuf, *btitle;
static Item *items;
static int nitems, chstart[152], nch, loaded_book = -1;
static const char *CODES66[] = { "GEN", "EXO", "LEV", "NUM", "DEU", "JOS", "JDG", "RUT", "1SA", "2SA", "1KI", "2KI", "1CH", "2CH", "EZR", "NEH",
    "EST", "JOB", "PSA", "PRO", "ECC", "SNG", "ISA", "JER", "LAM", "EZK", "DAN", "HOS", "JOL", "AMO", "OBA", "JON", "MIC", "NAM", "HAB", "ZEP",
    "HAG", "ZEC", "MAL", "MAT", "MRK", "LUK", "JHN", "ACT", "ROM", "1CO", "2CO", "GAL", "EPH", "PHP", "COL", "1TH", "2TH", "1TI", "2TI", "TIT",
    "PHM", "HEB", "JAS", "1PE", "2PE", "1JN", "2JN", "3JN", "JUD", "REV" };

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    n = fread(b, 1, n, f); b[n] = 0; fclose(f);
    return b;
}
static void meta_get(const char *meta, const char *key, char *out, size_t n)
{
    char pat[40]; snprintf(pat, sizeof pat, "\n%s\t", key);
    const char *p;
    if (!strncmp(meta, pat + 1, strlen(pat + 1))) p = meta + strlen(pat + 1);       // key on the first line
    else if ((p = strstr(meta, pat))) p += strlen(pat);
    out[0] = 0;
    if (!p) return;
    const char *e = strchr(p, '\n');
    snprintf(out, n, "%.*s", (int)(e ? e - p : (long)strlen(p)), p);
}
static int cmp_vers(const void *a, const void *b)
{
    const Version *x = a, *y = b;
    int c = strcasecmp(x->lang_en, y->lang_en);
    return c ? c : strcasecmp(x->title, y->title);
}
static int scan_versions(void)
{
    DIR *d = opendir("data");
    struct dirent *e;
    if (!d) return -1;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[300]; snprintf(p, sizeof p, "data/%s/meta.txt", e->d_name);
        char *m = read_file(p);
        if (!m) continue;
        vers = realloc(vers, (nvers + 1) * sizeof *vers);
        Version *v = &vers[nvers++];
        memset(v, 0, sizeof *v);
        char sec[160], breaks[16];
        snprintf(v->id, sizeof v->id, "%s", e->d_name);
        meta_get(m, "language", v->lang, sizeof v->lang);
        meta_get(m, "language_en", v->lang_en, sizeof v->lang_en);
        meta_get(m, "title", v->title, sizeof v->title);
        meta_get(m, "label", v->label, sizeof v->label);
        meta_get(m, "font", v->font, sizeof v->font);
        meta_get(m, "copyright", v->copyright, sizeof v->copyright);
        meta_get(m, "license", v->license, sizeof v->license);
        meta_get(m, "source", v->source, sizeof v->source);
        meta_get(m, "breaks", breaks, sizeof breaks);
        meta_get(m, "sections", sec, sizeof sec);
        v->breaks_any = !strcmp(breaks, "any");
        char *s = sec;
        for (int k = 0; k < 3; k++) {
            char *bar = strchr(s, '|');
            snprintf(v->sections[k], sizeof v->sections[k], "%.*s", (int)(bar ? bar - s : (long)strlen(s)), s);
            s = bar ? bar + 1 : s + strlen(s);
        }
        if (!v->label[0]) snprintf(v->label, sizeof v->label, "%s", v->lang);
        free(m);
    }
    closedir(d);
    qsort(vers, nvers, sizeof *vers, cmp_vers);
    return nvers ? 0 : -1;
}
static int find_version(const char *id) { for (int i = 0; i < nvers; i++) if (!strcmp(vers[i].id, id)) return i; return -1; }
static int find_book(const char *code) { for (int i = 0; i < nbooks; i++) if (!strcmp(books[i].code, code)) return i; return -1; }
static int load_version(int v)
{
    char p[300];
    snprintf(p, sizeof p, "data/%s/index.txt", vers[v].id);
    char *b = read_file(p), *s = b;
    if (!b) return -1;
    nbooks = 0;
    while (*s && nbooks < 100) {
        char *e = strchr(s, '\n'); if (e) *e = 0;
        char *t1 = strchr(s, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL, *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
        if (t3) {
            Book *bk = &books[nbooks++];
            snprintf(bk->code, sizeof bk->code, "%.*s", (int)(t1 - s), s);
            snprintf(bk->name, sizeof bk->name, "%.*s", (int)(t2 - t1 - 1), t1 + 1);
            bk->chapters = atoi(t2 + 1); bk->section = atoi(t3 + 1);
        }
        s = e ? e + 1 : s + strlen(s);
    }
    free(b);
    // the translation's own font (CJK) becomes the third fallback
    if (cur_ver < 0 || strcmp(vers[v].font, vers[cur_ver].font) || strcmp(vers[v].id, vers[cur_ver].id)) {
        int had = font_ok[2];
        free(fontdata[2]); fontdata[2] = NULL; font_ok[2] = 0;
        if (vers[v].font[0]) {
            snprintf(p, sizeof p, "data/%s/%s", vers[v].id, vers[v].font);
            if ((fontdata[2] = (unsigned char *)read_file(p)))
                font_ok[2] = stbtt_InitFont(&fonts[2], fontdata[2], stbtt_GetFontOffsetForIndex(fontdata[2], 0));
        }
        if (had || font_ok[2]) clear_faces();
    }
    cur_ver = v; loaded_book = -1;
    return nbooks ? 0 : -1;
}
static int load_book(int b)
{
    char path[300];
    if (b == loaded_book) return 0;
    snprintf(path, sizeof path, "data/%s/%02d.txt", vers[cur_ver].id, b + 1);
    char *buf = read_file(path);
    if (!buf) return -1;
    free(bookbuf); free(items);
    bookbuf = buf; btitle = ""; nitems = nch = 0;
    int lines = 1;
    for (char *p = buf; *p; p++) lines += *p == '\n';
    items = malloc(lines * sizeof *items);
    for (char *p = buf; *p;) {
        char *l = p, *e = strchr(p, '\n');
        if (e) { *e = 0; p = e + 1; } else p += strlen(p);
        if (e && e > l && e[-1] == '\r') e[-1] = 0;
        if (l[0] == 'T') btitle = l + 2;
        else if (l[0] == 'C' && nch < 151) chstart[nch++] = nitems;
        else if (l[0] == 'H') items[nitems++] = (Item){ 'H', 0, l + 2 };
        else if (l[0] == 'V') { char *t; int n = strtol(l + 2, &t, 10); items[nitems++] = (Item){ 'V', n, *t ? t + 1 : t }; }
    }
    chstart[nch] = nitems;
    loaded_book = b;
    return 0;
}

// ---------------------------------------------------------------- state
static const int SIZES[] = { 18, 20, 22, 24, 26, 28, 30, 33, 36, 40 };
#define NSIZES (int)(sizeof SIZES / sizeof SIZES[0])
static int book = 0, chap = 1, sizeidx = 4, night = 0, helpseen = 0, start_verse = 1;
static char start_ver[32] = "engwebp", start_code[5] = "GEN", prev_id[32] = "-";

// state.txt v2: "v2 <version id> <book code> <chapter> <verse> <size> <night> <help seen> <red letters> <previous version id>"
// v1.0 wrote seven numbers (0 = KJV, 1 = WEB; book = position in the 66).
static void load_state(void)
{
    FILE *f = fopen("state.txt", "r");
    char first[8];
    if (!f) { if (H >= 560) sizeidx = 6; return; }
    if (fscanf(f, "%7s", first) == 1 && !strcmp(first, "v2")) {
        if (fscanf(f, "%31s %4s %d %d %d %d %d %d %31s", start_ver, start_code, &chap, &start_verse, &sizeidx, &night, &helpseen, &red_letters, prev_id) < 8) helpseen = 0;
    }
    else {
        int v = atoi(first), b = 0;
        if (fscanf(f, "%d %d %d %d %d %d", &b, &chap, &start_verse, &sizeidx, &night, &helpseen) != 6) helpseen = 0;
        snprintf(start_ver, sizeof start_ver, "%s", v == 0 ? "eng-kjv" : "engwebp");
        snprintf(start_code, sizeof start_code, "%s", CODES66[b >= 0 && b < 66 ? b : 0]);
    }
    fclose(f);
    if (sizeidx < 0 || sizeidx >= NSIZES) sizeidx = 4;
    night = !!night; red_letters = !!red_letters;
}
static void save_state(int verse)
{
    FILE *f = fopen("state.tmp", "w");
    if (!f) return;
    fprintf(f, "v2 %s %s %d %d %d %d %d %d %s\n", vers[cur_ver].id, books[book].code, chap, verse, sizeidx, night, helpseen, red_letters,
            prev_ver >= 0 ? vers[prev_ver].id : "-");
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
static int is_cjk(int cp) { return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF) || cp >= 0x20000; }
static int closing_punct(int cp)            // never starts a line
{
    static const int set[] = { 0x3001, 0x3002, 0xFF0C, 0xFF0E, 0xFF01, 0xFF1F, 0xFF1A, 0xFF1B, 0xFF09, 0x300D, 0x300F, 0x3011, 0x3015, 0x3009, 0x300B,
                               0x30FC, 0x2026, 0x3005, 0x201D, 0x2019, ',', '.', ';', ':', '!', '?', ')', RED_ON, RED_OFF };
    for (unsigned i = 0; i < sizeof set / sizeof set[0]; i++) if (set[i] == cp) return 1;
    return 0;
}
// End of the next unbreakable piece after q: a word, or one Chinese/Japanese character with any closing punctuation.
static const char *unit_end(const char *q, int any)
{
    while (*q == ' ') q++;
    const char *t = q;
    int cp = *q ? utf8(&t) : 0;
    if (any && is_cjk(cp)) {
        q = t;
        while (*q && *q != ' ') { const char *u = q; if (!closing_punct(utf8(&u))) break; q = u; }
        return q;
    }
    const char *start = q;
    while (*q && *q != ' ') {
        const char *u = q;
        int c = utf8(&u);
        if (any && q != start && is_cjk(c)) break;          // a Chinese/Japanese character starts its own piece
        q = u;
    }
    return q;
}
// Greedy wrap; x = -1 centres the line.
static Line *wrap(Face *f, const char *s, int kind, int verse, int x, int maxw, int h)
{
    Line *first = NULL;
    const char *p = s;
    int red = 0, any = vers[cur_ver].breaks_any;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *ls = p, *le = p, *q = p;
        for (;;) {
            q = unit_end(q, any);
            if (le != ls && text_w(f, ls, q - ls) > maxw) break;
            le = q;
            if (!*q) break;
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
    if (chap == 1 && *btitle && strcmp(btitle, books[book].name)) {
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
            if (nlines) lines[nlines - 1].h += body->lineh / 5;
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
    if (load_book(b)) return -1;
    book = b; chap = c < 1 ? 1 : c > nch ? nch : c;
    layout();
    top = verse < 0 ? maxtop : line_of_verse(verse);
    save_state(top_verse());
    return 0;
}
static void relayout_keep_place(void) { int v = top_verse(); loaded_book = -1; open_chapter(book, chap, v); }
static void set_size(int s)
{
    if (s < 0 || s >= NSIZES || s == sizeidx) return;
    sizeidx = s;
    clear_faces();                              // safe here: nothing is being drawn
    relayout_keep_place();
}
// Switch translation and stay on the same book, chapter and verse (or the nearest that exists).
static void switch_version(int v)
{
    if (v == cur_ver || v < 0) return;
    char code[5]; snprintf(code, sizeof code, "%s", books[book].code);
    int verse = top_verse(), c = chap, old = cur_ver;
    if (load_version(v)) { load_version(old); return; }
    prev_ver = old;
    int b = find_book(code);
    if (b < 0) { b = 0; c = 1; verse = 1; }
    open_chapter(b, c > books[b].chapters ? books[b].chapters : c, verse);
}
static void next_chapter(int dir, int at_end)
{
    int b = book, c = chap + dir;
    if (c < 1) { if (--b < 0) return; c = books[b].chapters; }
    else if (c > books[b].chapters) { if (++b >= nbooks) return; c = 1; }
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
enum { S_READER, S_BOOKS, S_CHAPTERS, S_SETTINGS, S_VERSIONS, S_ABOUT };
static int screen = S_READER, show_help, sel, browse_book, list_scroll, settings_from, settings_sel;

static void draw_bar(const char *left, const char *right, int progress)
{
    Face *ui = f_ui(), *sm = f_small();
    fill(0, 0, W, BAR, T->bar);
    fill(0, BAR - 1, W, 1, T->line);
    if (progress >= 0) fill(0, BAR - 2, W * progress / 100, 2, T->accent);
    int base = (BAR - ui->lineh) / 2 + ui->asc + 1, rw = (int)text_w(sm, right, strlen(right));
    draw_fit(ui, MARGIN, base, left, W - 3 * MARGIN - rw, T->fg);
    draw_right(sm, W - MARGIN, base, right, T->dim);
}
static void draw_hints(const char *s)
{
    Face *sm = f_small();
    int y = H - sm->lineh - H / 60;
    fill(0, y - H / 60, W, H - y + H / 60, T->bar);
    fill(0, y - H / 60, W, 1, T->line);
    draw_center(sm, W / 2, y + sm->asc, s, T->dim);
}
static void list_scroll_to(int y, int h, int viewh)     // keep [y, y+h) visible
{
    if (y < list_scroll) list_scroll = y;
    if (y + h > list_scroll + viewh) list_scroll = y + h - viewh;
    if (list_scroll < 0) list_scroll = 0;
}

static void draw_reader(void)
{
    char left[96], right[64];
    int total = nlines ? lines[nlines - 1].y + lines[nlines - 1].h : 1;
    int prog = maxtop == 0 || top >= maxtop ? 100 : (lines[top].y + VIEW_H) * 100 / total;
    snprintf(left, sizeof left, "%s %d", books[book].name, chap);
    snprintf(right, sizeof right, "%s  %d%%", vers[cur_ver].label, prog);
    fill(0, 0, W, H, T->bg);
    Face *body = f_body(), *num = f_num(), *head = f_head(), *chf = f_chap(), *tf = f_title();
    for (int i = top; i < nlines; i++) {
        Line *l = &lines[i];
        int y = VIEW_Y + l->y - lines[top].y;
        if (l->y + l->h - lines[top].y > VIEW_H) break;      // same rule as page()
        if (l->kind == L_TITLE) draw_text(tf, (W - text_w(tf, l->s, l->n)) / 2, y + tf->asc, l->s, l->n, T->dim);
        else if (l->kind == L_CHAP) {
            char s[96];
            if (books[book].chapters > 1) snprintf(s, sizeof s, "%s %d", books[book].name, chap);
            else snprintf(s, sizeof s, "%s", books[book].name);
            if (text_w(chf, s, strlen(s)) > W - 2 * MARGIN) draw_center(f_ui(), W / 2, y + chf->asc, s, T->accent);
            else draw_center(chf, W / 2, y + chf->asc, s, T->accent);
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
        { "X", "Previous translation" }, { "Y", "Day / night" }, { "B", "Chapters" }, { "Select", "Books" },
        { "Start", "Settings (language)" }, { "Menu", "Quit (keeps your place)" },
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

// Books: one block per section (Old / New Testament, deuterocanon), in 3 columns, or 2 when the names are long.
#define MAXROWS 80
static int brow_y[MAXROWS], brow_first[MAXROWS], brow_n[MAXROWS], brow_head[MAXROWS], nbrows, bcols;
static void book_rows(void)
{
    Face *ui = f_ui(), *sm = f_small();
    int rowh = ui->lineh + H / 40, headh = sm->lineh + H / 30, y = 0, maxw = 0;
    for (int i = 0; i < nbooks; i++) { int w = (int)text_w(ui, books[i].name, strlen(books[i].name)); if (w > maxw) maxw = w; }
    bcols = maxw + H / 20 <= (W - 2 * MARGIN) / 3 ? 3 : 2;
    nbrows = 0;
    for (int i = 0; i < nbooks && nbrows < MAXROWS - 1;) {
        int s = books[i].section;
        brow_head[nbrows] = s; brow_first[nbrows] = -1; brow_n[nbrows] = 0; brow_y[nbrows] = y; y += headh; nbrows++;
        while (i < nbooks && books[i].section == s && nbrows < MAXROWS) {
            brow_head[nbrows] = -1; brow_first[nbrows] = i; brow_y[nbrows] = y; brow_n[nbrows] = 0;
            while (i < nbooks && books[i].section == s && brow_n[nbrows] < bcols) { brow_n[nbrows]++; i++; }
            y += rowh; nbrows++;
        }
    }
    brow_y[nbrows] = y;
}
static int row_of(int b) { for (int r = 0; r < nbrows; r++) if (brow_head[r] < 0 && b >= brow_first[r] && b < brow_first[r] + brow_n[r]) return r; return 0; }
static void draw_books(void)
{
    Face *ui = f_ui(), *sm = f_small();
    int rowh = ui->lineh + H / 40, headh = sm->lineh + H / 30, listy = BAR, listh = H - BAR - (sm->lineh + H / 30);
    int cw = (W - 2 * MARGIN) / bcols, r = row_of(sel);
    int want = r > 0 && brow_head[r - 1] >= 0 ? brow_y[r - 1] : brow_y[r];      // first row of a section: show its heading
    list_scroll_to(want, brow_y[r] + rowh - want, listh);
    if (list_scroll > brow_y[nbrows] - listh) list_scroll = brow_y[nbrows] - listh > 0 ? brow_y[nbrows] - listh : 0;
    fill(0, 0, W, H, T->bg);
    for (int i = 0; i < nbrows; i++) {
        int y = listy - list_scroll + brow_y[i];
        if (y + rowh < listy || y > listy + listh) continue;
        if (brow_head[i] >= 0) {
            const char *label = vers[cur_ver].sections[brow_head[i]];
            int base = y + headh - H / 80 - (sm->lineh - sm->asc);
            if (label[0]) draw_str(sm, MARGIN, base, label, T->dim);
            else if (i > 0) fill(MARGIN, y + headh / 2, W - 2 * MARGIN, 1, T->line);    // no wording for this language: plain divider
            continue;
        }
        for (int k = 0; k < brow_n[i]; k++) {
            int b = brow_first[i] + k, x = MARGIN + k * cw;
            uint32_t col = b == book ? T->accent : T->fg;
            if (b == sel) { rrect(x, y + 2, cw - H / 80, rowh - 4, H / 60, T->accent); col = T->seltext; }
            draw_fit(ui, x + H / 40, y + (rowh - ui->lineh) / 2 + ui->asc, books[b].name, cw - H / 20, col);
        }
    }
    draw_bar(vers[cur_ver].lang, vers[cur_ver].label, -1);
    draw_hints("A  Open     B  Back     L1 / R1  Section     Start  Settings");
}
static int chapter_cols(void) { return (W - 2 * MARGIN) / (H / 7); }
static void draw_chapters(void)
{
    Face *ui = f_ui(), *sm = f_small();
    int cols = chapter_cols(), cw = (W - 2 * MARGIN) / cols, chh = cw * 7 / 10, n = books[browse_book].chapters;
    int listy = BAR + H / 60, listh = H - listy - (sm->lineh + H / 30), rows = (n + cols - 1) / cols;
    list_scroll_to((sel / cols) * chh, chh, listh);
    if (list_scroll > rows * chh - listh) list_scroll = rows * chh - listh > 0 ? rows * chh - listh : 0;
    fill(0, 0, W, H, T->bg);
    for (int i = 0; i < n; i++) {
        int x = MARGIN + (i % cols) * cw, y = listy + (i / cols) * chh - list_scroll;
        if (y + chh < listy || y > listy + listh) continue;
        char s[8]; snprintf(s, sizeof s, "%d", i + 1);
        uint32_t col = browse_book == book && i + 1 == chap ? T->accent : T->fg;
        if (i == sel) { rrect(x + 3, y + 3, cw - 6, chh - 6, H / 60, T->accent); col = T->seltext; }
        draw_center(ui, x + cw / 2, y + (chh - ui->lineh) / 2 + ui->asc, s, col);
    }
    char right[16]; snprintf(right, sizeof right, "1 - %d", n);
    draw_bar(books[browse_book].name, right, -1);
    draw_hints("A  Read     B  Books     L1 / R1  Previous / next book     Start  Settings");
}

// Settings: language first.
enum { SET_LANG, SET_SIZE, SET_THEME, SET_RED, SET_HELP, SET_ABOUT, SET_COUNT };
static void draw_settings(void)
{
    static const char *names[SET_COUNT] = { "Language", "Text size", "Theme", "Red letters", "Controls", "About this translation" };
    Face *ui = f_ui(), *sm = f_small();
    int rowh = ui->lineh + H / 24, y = BAR + H / 40;
    fill(0, 0, W, H, T->bg);
    for (int i = 0; i < SET_COUNT; i++, y += rowh) {
        char v[96] = "";
        if (i == SET_LANG) snprintf(v, sizeof v, "%s", vers[cur_ver].lang);
        else if (i == SET_SIZE) snprintf(v, sizeof v, "<  %d  >", SIZES[sizeidx]);
        else if (i == SET_THEME) snprintf(v, sizeof v, "%s", night ? "Night" : "Day");
        else if (i == SET_RED) snprintf(v, sizeof v, "%s", red_letters ? "On" : "Off");
        uint32_t c = T->fg, vc = T->dim;
        if (i == settings_sel) { rrect(MARGIN / 2, y + 2, W - MARGIN, rowh - 4, H / 60, T->accent); c = vc = T->seltext; }
        int base = y + (rowh - ui->lineh) / 2 + ui->asc;
        draw_str(ui, MARGIN, base, names[i], c);
        int nw = (int)text_w(ui, names[i], strlen(names[i])) + W / 12;
        Face *vf = i == SET_LANG ? ui : sm;
        float vw = text_w(vf, v, strlen(v));
        if (vw > W - 2 * MARGIN - nw) draw_fit(vf, MARGIN + nw, base, v, W - 2 * MARGIN - nw, vc);
        else draw_right(vf, W - MARGIN, base, v, vc);
    }
    if (settings_sel == SET_LANG) {           // show the translation's full name under the list
        char s[200]; snprintf(s, sizeof s, "%s  -  %s", vers[cur_ver].title, vers[cur_ver].lang_en);
        draw_fit(sm, MARGIN, y + H / 30 + sm->asc, s, W - 2 * MARGIN, T->dim);
        snprintf(s, sizeof s, "%d translations on this card", nvers);
        draw_str(sm, MARGIN, y + H / 30 + sm->lineh + sm->asc, s, T->dim);
    }
    draw_bar("Settings", "", -1);
    draw_hints("Up / Down  Choose     Left / Right or A  Change     B  Back");
}
static void draw_versions(void)
{
    Face *ui = f_ui(), *sm = f_small();
    int rowh = ui->lineh + H / 40, listy = BAR, listh = H - BAR - (sm->lineh + H / 30);
    list_scroll_to(sel * rowh, rowh, listh);
    fill(0, 0, W, H, T->bg);
    for (int i = 0; i < nvers; i++) {
        int y = listy + i * rowh - list_scroll;
        if (y + rowh < listy || y > listy + listh) continue;
        Version *v = &vers[i];
        uint32_t c = i == cur_ver ? T->accent : T->fg, d = T->dim;
        if (i == sel) { rrect(MARGIN / 2, y + 2, W - MARGIN, rowh - 4, H / 60, T->accent); c = d = T->seltext; }
        int base = y + (rowh - ui->lineh) / 2 + ui->asc, ew = (int)text_w(sm, v->lang_en, strlen(v->lang_en));
        float x = draw_fit(ui, MARGIN, base, v->lang, W * 45 / 100, c);
        draw_fit(sm, (int)x + W / 30, base, v->title, W - MARGIN - ew - W / 20 - (int)x - W / 30, d);
        draw_right(sm, W - MARGIN, base, v->lang_en, d);
    }
    char right[24]; snprintf(right, sizeof right, "%d / %d", sel + 1, nvers);
    draw_bar("Language", right, -1);
    draw_hints("A  Choose     B  Back     Left / Right  Page");
}
static void draw_about(void)
{
    Face *ui = f_ui(), *sm = f_small();
    Version *v = &vers[cur_ver];
    char lic[120];
    if (!strcmp(v->license, "PD")) snprintf(lic, sizeof lic, "Public domain");
    else snprintf(lic, sizeof lic, "Creative Commons %s", !strncmp(v->license, "CC-", 3) ? v->license + 3 : v->license);
    const char *rows[][2] = { { "Translation", v->title }, { "Language", v->lang_en }, { "Copyright", v->copyright }, { "License", lic },
                              { "Source", v->source }, { "Fonts", "Literata, Noto Serif (SIL Open Font License)" } };
    fill(0, 0, W, H, T->bg);
    int y = BAR + H / 30;
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        draw_str(sm, MARGIN, y + sm->asc, rows[i][0], T->accent);
        y += sm->lineh;
        // wrap long values over up to three lines
        const char *s = rows[i][1];
        for (int ln = 0; ln < 3 && *s; ln++, y += ui->lineh) {
            int n = strlen(s);
            while (n > 0 && text_w(ui, s, n) > W - 2 * MARGIN) { n--; while (n > 0 && (s[n] & 0xC0) == 0x80) n--; }
            if (n < (int)strlen(s)) { int k = n; while (k > 0 && s[k] != ' ') k--; if (k > 0) n = k; }
            draw_text(ui, MARGIN, y + ui->asc, s, n, T->fg);
            s += n; while (*s == ' ') s++;
        }
        y += H / 60;
    }
    draw_bar("About this translation", v->label, -1);
    draw_hints("B  Back");
}
static void draw(void)
{
    T = &THEMES[night];
    if (screen == S_READER) { draw_reader(); if (show_help) draw_help(); }
    else if (screen == S_BOOKS) draw_books();
    else if (screen == S_CHAPTERS) draw_chapters();
    else if (screen == S_SETTINGS) { draw_settings(); if (show_help) draw_help(); }
    else if (screen == S_VERSIONS) draw_versions();
    else draw_about();
    plat_present();
}

static void open_books(void) { screen = S_BOOKS; sel = book; list_scroll = 0; book_rows(); }
static void open_chapters(int b) { screen = S_CHAPTERS; browse_book = b; sel = b == book ? chap - 1 : 0; list_scroll = 0; }
static void open_settings(void) { settings_from = screen; screen = S_SETTINGS; settings_sel = SET_LANG; }
static void open_versions(void) { screen = S_VERSIONS; sel = cur_ver; list_scroll = 0; }

// returns 0 to quit
static int handle(int k)
{
    if (k == K_MENU || k == K_QUIT) return 0;
    if (show_help) { show_help = 0; if (!helpseen) { helpseen = 1; save_state(top_verse()); } return 1; }
    if (k == K_Y && screen != S_VERSIONS) { night = !night; save_state(top_verse()); return 1; }
    if (k == K_X && screen != S_VERSIONS && screen != S_SETTINGS) {
        if (prev_ver >= 0) { switch_version(prev_ver); if (screen == S_BOOKS) open_books(); else if (screen == S_CHAPTERS) open_chapters(book); }
        else { settings_from = screen; open_versions(); }
        return 1;
    }
    if (screen == S_READER) {
        switch (k) {
        case K_UP: if (top > 0) top--; break;
        case K_DOWN: if (top < maxtop) top++; break;
        case K_LEFT: page(-1); break;
        case K_RIGHT: case K_A: page(1); break;
        case K_L1: set_size(sizeidx - 1); break;
        case K_R1: set_size(sizeidx + 1); break;
        case K_L2: next_chapter(-1, 0); break;
        case K_R2: next_chapter(1, 0); break;
        case K_B: open_chapters(book); break;
        case K_SELECT: open_books(); break;
        case K_START: open_settings(); break;
        }
    }
    else if (screen == S_BOOKS) {
        int r = row_of(sel), col = sel - brow_first[r];
        switch (k) {
        case K_UP: case K_DOWN: {
            int t = r + (k == K_UP ? -1 : 1);
            while (t >= 0 && t < nbrows && brow_head[t] >= 0) t += k == K_UP ? -1 : 1;
            if (t >= 0 && t < nbrows) sel = brow_first[t] + (col < brow_n[t] ? col : brow_n[t] - 1);
            break; }
        case K_LEFT: if (sel > 0) sel--; break;
        case K_RIGHT: if (sel < nbooks - 1) sel++; break;
        case K_L1: { int s = books[sel].section, i = sel;
            while (i > 0 && books[i - 1].section == s) i--;          // start of this section, or of the previous one
            if (i == sel && i > 0) { i--; while (i > 0 && books[i - 1].section == books[i].section) i--; }
            sel = i; break; }
        case K_R1: { int s = books[sel].section, i = sel; while (i < nbooks && books[i].section == s) i++; if (i < nbooks) sel = i; break; }
        case K_A: if (books[sel].chapters == 1) { open_chapter(sel, 1, 1); screen = S_READER; } else open_chapters(sel); break;
        case K_B: case K_SELECT: screen = S_READER; break;
        case K_START: open_settings(); break;
        }
    }
    else if (screen == S_CHAPTERS) {
        int cols = chapter_cols(), n = books[browse_book].chapters;
        switch (k) {
        case K_UP: if (sel >= cols) sel -= cols; break;
        case K_DOWN: if (sel + cols < n) sel += cols; else if (sel / cols < (n - 1) / cols) sel = n - 1; break;
        case K_LEFT: if (sel > 0) sel--; break;
        case K_RIGHT: if (sel < n - 1) sel++; break;
        case K_L1: if (browse_book > 0) open_chapters(browse_book - 1); break;
        case K_R1: if (browse_book < nbooks - 1) open_chapters(browse_book + 1); break;
        case K_A: open_chapter(browse_book, sel + 1, 1); screen = S_READER; break;
        case K_B: open_books(); sel = browse_book; break;
        case K_SELECT: open_books(); break;
        case K_START: open_settings(); break;
        }
    }
    else if (screen == S_SETTINGS) {
        int dir = k == K_LEFT ? -1 : k == K_RIGHT || k == K_A ? 1 : 0;
        switch (k) {
        case K_UP: settings_sel = (settings_sel + SET_COUNT - 1) % SET_COUNT; break;
        case K_DOWN: settings_sel = (settings_sel + 1) % SET_COUNT; break;
        case K_B: case K_START: screen = settings_from; if (screen == S_BOOKS) book_rows(); break;
        case K_LEFT: case K_RIGHT: case K_A:
            if (settings_sel == SET_LANG && k != K_LEFT) open_versions();
            else if (settings_sel == SET_SIZE) set_size(sizeidx + (k == K_LEFT ? -1 : k == K_RIGHT ? 1 : 0));
            else if (settings_sel == SET_THEME && dir) { night = !night; save_state(top_verse()); }
            else if (settings_sel == SET_RED && dir) { red_letters = !red_letters; save_state(top_verse()); }
            else if (settings_sel == SET_HELP && k == K_A) show_help = 1;
            else if (settings_sel == SET_ABOUT && k == K_A) screen = S_ABOUT;
            break;
        }
    }
    else if (screen == S_VERSIONS) {
        int page_n = (H - BAR - H / 10) / (f_ui()->lineh + H / 40);
        switch (k) {
        case K_UP: sel = sel > 0 ? sel - 1 : nvers - 1; break;
        case K_DOWN: sel = sel < nvers - 1 ? sel + 1 : 0; break;
        case K_LEFT: case K_L1: sel = sel > page_n ? sel - page_n : 0; break;
        case K_RIGHT: case K_R1: sel = sel + page_n < nvers ? sel + page_n : nvers - 1; break;
        case K_A: switch_version(sel); screen = S_READER; break;
        case K_B: screen = S_SETTINGS; break;
        }
    }
    else if (k == K_B || k == K_A || k == K_START) screen = S_SETTINGS;          // about
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
    const char *font_files[NFONTS] = { "Literata.ttf", "NotoSerif.ttf", NULL, "data/_fonts/names_1.ttf", "data/_fonts/names_2.ttf" };
    for (int k = 0; k < NFONTS; k++)
        if (font_files[k] && (fontdata[k] = (unsigned char *)read_file(font_files[k])))
            font_ok[k] = stbtt_InitFont(&fonts[k], fontdata[k], stbtt_GetFontOffsetForIndex(fontdata[k], 0));
    if (!font_ok[0] || scan_versions() || plat_init()) return 1;
    load_state();
    int v = find_version(start_ver);
    if (v < 0) v = find_version("engwebp");
    if (v < 0) v = 0;
    if (load_version(v)) return 1;
    prev_ver = find_version(prev_id);
    if (prev_ver == cur_ver) prev_ver = -1;
    int b = find_book(start_code);
    if (b < 0) { b = 0; chap = 1; start_verse = 1; }
    if (chap > books[b].chapters) chap = books[b].chapters;
    if (open_chapter(b, chap, start_verse)) return 1;
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
