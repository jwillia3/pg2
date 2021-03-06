#define BEZIER_LIMIT 10
#include <assert.h>
#include <ctype.h>
#include <float.h>
#include <iso646.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "pg.h"
#include "platform.h"
#include "util.h"

#define trailing(n) ((in[n] & 0xC0) == 0x80)
#define overlong(n) (out < n? out = 0xfffd: out)
unsigned pgStepUtf8(const char **input) {
    unsigned out;
    const uint8_t *in = (const uint8_t*) *input;
    if (*in < 0x80)
        out = *in++;
    else if (~*in & 0x20 && in[1] && trailing(1))
        out =   (in[0] & 0x1f) << 6 |
                (in[1] & 0x3f),
        overlong(0x80),
        in += 2;
    else if (~*in & 0x10 && in[1] && in[2] && trailing(1) && trailing(2))
        out =   (in[0] & 0x0f) << 12 |
                (in[1] & 0x3f) << 6 |
                (in[2] & 0x3f),
        overlong(0x800),
        in += 3;
    else { // Malformed or non-BMP character
        while ((*++in & 0xc0) == 0x80);
        out = 0xfffd;
    }
    *input = (const char*) in;
    return out;
}
char *pgOutputUtf8(char **output, unsigned c) {
    if (c < 0x80)
        *(* (uint8_t**) output)++ = c;
    else if (c < 0x800)
        *(* (uint8_t**) output)++ = 0xc0 | 0x1f & c >> 6,
        *(* (uint8_t**) output)++ = 0x80 | 0x3f & c;
    else if (c <= 0xffff)
        *(* (uint8_t**) output)++ = 0xe0 | 0x0f & c >> 12,
        *(* (uint8_t**) output)++ = 0x80 | 0x3f & c >> 6,
        *(* (uint8_t**) output)++ = 0x80 | 0x3f & c;
    else return pgOutputUtf8(output, 0xfffd); // substitute character
    return *output;
}
char *pgToUtf8(const wchar_t *string) {
    char *output = malloc(3 * wcslen(string) + 1);
    char *p = output;
    for (const wchar_t *i = string; *i; i++)
        pgOutputUtf8(&p, *i);
    *p = 0;
    return realloc(output, p - output + 1);
}
wchar_t *pgFromUtf8(const char *string) {
    wchar_t *output = malloc(sizeof (wchar_t) * (strlen(string) + 1));
    wchar_t *p = output;
    while ((*p++ = pgStepUtf8(&string)));
    return realloc(output, sizeof (wchar_t) * (p - output + 1));
}
#undef trailing
#undef overlong

PgStringBuffer *pgNewStringBuffer() {
    return calloc(1, sizeof(PgStringBuffer));
}
PgStringBuffer *pgBufferCharacter(PgStringBuffer *buffer, unsigned c) {
    buffer->text = realloc(buffer->text, buffer->length + 6 + 1);
    char *p = &buffer->text[buffer->length];
    buffer->length = pgOutputUtf8(&p, c) - buffer->text;
    *p = 0;
    return buffer;
}
PgStringBuffer *pgBufferString(PgStringBuffer *buffer, const char *text, int length) {
    if (length < 0) length = strlen(text);
    for (int i = 0; i < length; i++) pgBufferCharacter(buffer, text[i]);
    return buffer;
}
PgStringBuffer *pgBufferFormat(PgStringBuffer *buffer, const char *format, ...) {
    va_list ap;
    char *temp = malloc(65536 + 1);
    va_start(ap, format);
    vsprintf(temp, format, ap);
    va_end(ap);
    pgBufferString(buffer, temp, -1);
    free(temp);

    return buffer;
}
void pgFreeStringBuffer(PgStringBuffer *buffer) {
    free(buffer->text);
    free(buffer);
}

typedef struct { float x, m, y2; } edge_t;
typedef struct { PgPt a, b; float m; } seg_t;
typedef struct { seg_t *data; int n, cap; } segs_t;
static void addSeg(segs_t *segs, PgPt a, PgPt b) {
    if (segs->n >= segs->cap) {
        segs->cap = segs->cap? segs->cap * 2: 8;
        segs->data = realloc(segs->data, segs->cap * sizeof(seg_t));
    }
    segs->data[segs->n++] = (seg_t) {
        .a = a.y < b.y? a: b,
        .b = a.y < b.y? b: a,
        .m = a.y < b.y? (b.x - a.x) / (b.y - a.y):
            a.y > b.y? (a.x - b.x) / (a.y - b.y):
            0 };
}
static void segmentQuad(segs_t *segs, PgPt a, PgPt b, PgPt c, int n, float flatness) {
    if (!n) {
        addSeg(segs, a, c);
        return;
    }
    PgPt m = { (a.x + 2.0f * b.x + c.x) / 4.0f, (a.y + 2.0f * b.y + c.y) / 4.0f };
    PgPt d = { (a.x + c.x) / 2.0f - m.x, (a.y + c.y) / 2.0f - m.y };
    if (d.x * d.x + d.y * d.y > 0.05f) {
        PgPt ab = midpoint(a, b);
        PgPt bc = midpoint(b, c);
        PgPt abc = midpoint(ab, bc);
        segmentQuad(segs, a, ab, abc, n - 1, flatness);
        segmentQuad(segs, abc, bc, c, n - 1, flatness);
    } else addSeg(segs, a, c);
}
static void segmentCubic(segs_t *segs, PgPt a, PgPt b, PgPt c, PgPt d, int n, float flatness) {
    if (!n) {
        addSeg(segs, a, d);
        return;
    }
    float ab = distance(pgPt(a.x - b.x, a.y - b.y));
    float bc = distance(pgPt(b.x - c.x, b.y - c.y));
    float cd = distance(pgPt(c.x - d.x, c.y - d.y));
    float ad = distance(pgPt(a.x - d.x, a.y - d.y));
    if (ab + bc + cd >= flatness * ad) {
        PgPt ab = midpoint(a, b);
        PgPt bc = midpoint(b, c);
        PgPt cd = midpoint(c, d);
        PgPt abc = midpoint(ab, bc);
        PgPt bcd = midpoint(bc, cd);
        PgPt abcd = midpoint(abc, bcd);
        segmentCubic(segs, a, ab, abc, abcd, n - 1, flatness);
        segmentCubic(segs, abcd, bcd, cd, d, n - 1, flatness);
    } else addSeg(segs, a, d);
}
static int sortSegsDescending(const void *x, const void *y) {
    const seg_t *a = x;
    const seg_t *b = y;
    return
        a->a.y < b->a.y? -1:
        a->a.y > b->a.y? 1:
        a->a.x < b->a.x? -1:
        a->a.x > b->a.x? 1: 0;
}

static void initGammaTable(uint16_t *toLinear, uint8_t *toGamma, float gamma) {
    for (int i = 0; i < 256; i++)
        toLinear[i] = pow(i / 255.0, gamma) * 32768.0 + 0.5;
    for (int i = 0; i < 32768; i++)
        toGamma[i] = pow(i / 32768.0, 1.0 / gamma) * 255.0 + 0.5;
}
static uint32_t fastblend(uint32_t fg, uint32_t bg, uint8_t a255, uint16_t *toLinear, uint8_t *toGamma) {
    if (a255 == 255) return fg;
    if (a255 == 0) return bg;
    int a = a255 + 1;
    int na = 255 - a;
    uint8_t r = toGamma[(a * toLinear[fg >> 16 & 255] + na * toLinear[bg >> 16 & 255]) / 256];
    uint8_t g = toGamma[(a * toLinear[fg >> 8 & 255] + na * toLinear[bg >> 8 & 255]) / 256];
    uint8_t b = toGamma[(a * toLinear[fg >> 0 & 255] + na * toLinear[bg >> 0 & 255]) / 256];
    return (r << 16) + (g << 8) + b;
}
uint32_t pgBlendWithGamma(uint32_t fg, uint32_t bg, uint8_t a255, uint16_t *toLinear, uint8_t *toGamma) {
    return fastblend(fg, bg, a255, toLinear, toGamma);
}
uint32_t pgBlend(uint32_t fg, uint32_t bg, uint8_t a255) {
    static uint16_t toLinear[256];
    static uint8_t toGamma[32768 + 1];
    if (!toLinear[255])
        initGammaTable(toLinear, toGamma, 2.2f);
    return fastblend(fg, bg, a255, toLinear, toGamma);
}


static void bmp_clear(Pg *g, uint32_t color) {
    if (g->stride == g->width)
        for (int i = 0; i < g->stride * g->height; i++)
            g->bmp[i] = color;
    else
        for (int y = g->clip.y1; y < g->clip.y2; y++)
        for (int x = g->clip.x1; x < g->clip.x2; x++)
            g->bmp[y * g->stride + x] = color;
}
static void bmp_clearSection(Pg *g, PgPt a, PgPt b, uint32_t color) {
    int x1 = clamp(g->clip.a.x, a.x, g->clip.b.x);
    int x2 = clamp(g->clip.a.x, ceil(b.x), g->clip.b.x);
    int y1 = clamp(g->clip.a.y, a.y, g->clip.b.y);
    int y2 = clamp(g->clip.a.y, ceil(b.y), g->clip.b.y);
    for (int y = y1; y < y2; y++)
    for (int x = x1; x < x2; x++)
        g->bmp[y * g->stride + x] = color;
}
static void bmp_free(Pg *g) {
    if (!g->borrowed)
        free(g->bmp);
    free(g);
}
static void bmp_resize(Pg *g, int width, int height) {
    if (g->borrowed)
        return;
    free(g->bmp);
    g->stride = width;
    g->width = width;
    g->height = height;
    g->clip = (PgRect){ {0, 0, width, height} };
    g->bmp = malloc(width * height * 4);
}
static segs_t bmp_segmentPath(PgPath *path, PgMatrix ctm, bool close, float flatness) {
    segs_t segs = { .data = NULL, .n = 0 };
    PgPt *p = path->data;
    int *t = (int*)path->types;
    // Decompose curves into line segments
    for (int *sub = path->subs; sub < path->subs + path->nsubs; sub++) {
        PgPt a = pgTransformPoint(ctm, *p++), first = a;
        for (PgPt *end = p + *sub - 1, next; p < end; p += *t++, a = next) {
            next = pgTransformPoint(ctm, p[*t - 1]);
            if (*t == PG_LINE)
                addSeg(&segs, a, next);
            else if (*t == PG_QUAD)
                segmentQuad(&segs, a, pgTransformPoint(ctm, p[0]), next, BEZIER_LIMIT, flatness);
            else if (*t == PG_CUBIC)
                segmentCubic(&segs, a, pgTransformPoint(ctm, p[0]), pgTransformPoint(ctm, p[1]), next, BEZIER_LIMIT, flatness);
        }
        if (close)
            addSeg(&segs, a, first);
    }
    return segs;
}
static void bmp_fillPath(Pg *g, uint32_t color, PgPath *path) {
    if (!path->npoints) return;

    PgMatrix ctm = g->ctm;
    PgRect binding_box = pgGetPathBindingBox(path, ctm);

    if (binding_box.a.x >= g->clip.x2 ||
        binding_box.a.y >= g->clip.y2 ||
        binding_box.b.x <  g->clip.x1 ||
        binding_box.b.y <  g->clip.y1)
        return;

    pgScaleMatrix(&ctm, 1, g->subsamples);
    segs_t segs = bmp_segmentPath(path, ctm, true, g->flatness);
    qsort(segs.data, segs.n, sizeof(seg_t), sortSegsDescending);
    float maxY = segs.data[0].b.y;
    for (seg_t *seg = segs.data + 1; seg < segs.data + segs.n; seg++)
        maxY = max(maxY, seg->b.y);
    maxY = clamp(g->clip.y1, maxY / g->subsamples + 1, g->clip.y2);

    // Scan through lines filling between edge
    typedef struct { float x, m, y2; } edge_t;
    edge_t *edges = malloc(segs.n * sizeof (edge_t));
    int nedges = 0;
    seg_t *seg = segs.data;
    seg_t *endSeg = seg + segs.n;
    uint8_t *buf = malloc(g->stride);
    int minx = g->clip.x1;
    int maxx = g->clip.x2 - 1;
    for (int scanY = max(g->clip.y1, segs.data[0].a.y / g->subsamples); scanY < maxY; scanY++) {
        if (minx <= maxx)
            memset(buf + minx, 0, maxx - minx + 1);
        minx = g->clip.x2 - 1;
        maxx = g->clip.x1;
        for (float ss = -g->subsamples * 0.5f; ss < g->subsamples * 0.5f; ss++) {
            float y = g->subsamples * scanY + ss + 0.5f;
            edge_t *endEdge = edges + nedges;
            nedges = 0;
            for (edge_t *e = edges; e < endEdge; e++)
                if (y <= e->y2) {
                    e->x += e->m;
                    edges[nedges++] = *e;
                }
            for ( ; seg < endSeg && seg->a.y < y; seg++)
                if (seg->b.y >= y)
                    edges[nedges++] = (edge_t) {
                        .x = seg->a.x + seg->m * (y - seg->a.y),
                        .m = seg->m,
                        .y2 = seg->b.y,
                    };
            for (int i = 1; i < nedges; i++)
                for (int j = i; j > 0 && edges[j - 1].x > edges[j].x; j--) {
                    edge_t tmp = edges[j];
                    edges[j] = edges[j - 1];
                    edges[j - 1] = tmp;
                }

            float level = 255.0f / g->subsamples;
            for (edge_t *e = edges + 1; e < edges + nedges; e += 2) {
                float x1 = e[-1].x;
                float x2 = e[0].x;
                if (x2 < g->clip.x1 || x1 >= g->clip.x2) continue;
                int a = clamp(g->clip.x1, x1, g->clip.x2);
                int b = clamp(g->clip.x1, x2, g->clip.x2);
                minx = min(minx, a);
                maxx = max(maxx, b);
                if (a == b)
                    buf[a] += (x2 - x1) * level;
                else {
                    if (x1 >= g->clip.x1)
                        buf[a++] += (1.0f - fraction(x1)) * level;
                    for (int x = a; x < b; x++)
                        buf[x] += level;
                    if (x2 < g->clip.x2)
                        buf[b] += fraction(x2) * level;
                }
            }
        }
        uint32_t *__restrict bmp = g->bmp + scanY * g->stride;
        minx = max(minx, 0);
        maxx = min(maxx, g->clip.x2 - 1);
        for (int i = minx; i <= maxx; i++)
            bmp[i] = fastblend(color, bmp[i], buf[i], g->toLinear, g->toGamma);
    }
    free(buf);
    free(edges);
    free(segs.data);
}
static void bmp_strokePath(Pg *g, uint32_t color, PgPath *path, float width) {
    PgMatrix otm = g->ctm;
    segs_t segs = bmp_segmentPath(path, g->ctm, false, g->flatness);
    for (seg_t *seg = segs.data; seg < segs.data + segs.n; seg++) {
        PgPath *sub = pgNewPath();
        float dx = seg->b.x - seg->a.x;
        float dy = seg->b.y - seg->a.y;
        float len = sqrt(dx*dx + dy*dy);
        float rad = atan2f(dy, dx);
        pgIdentity(g);
        pgRotate(g, rad);
        pgTranslate(g, seg->a.x, seg->a.y);
        PgPt vert[4] = {
            { -width / 2.0f, -width / 2.0f },
            { len + width / 2.0f, -width / 2.0f },
            { len + width / 2.0f, +width / 2.0f },
            { -width / 2.0f, + width / 2.0f },
        };
        pgMove(sub, vert[0]);
        pgLine(sub, vert[1]);
        pgLine(sub, vert[2]);
        pgLine(sub, vert[3]);
        pgFillPath(g, color, sub);
        pgFreePath(sub);
    }
    free(segs.data);
    g->ctm = otm;
}
static Pg *bmp_subsection(Pg *original, PgRect rect) {
    Pg *g = malloc(sizeof *g);
    *g = *original;
    rect.a = pgPt(clamp(0, rect.a.x, g->width), clamp(0, rect.a.y, g->height));
    rect.b = pgPt(clamp(rect.a.x, rect.b.x, g->width), clamp(rect.a.y, rect.b.y, g->height));
    g->clip = (PgRect) {{
        clamp(0, g->clip.a.x - rect.a.x, rect.b.x),
        clamp(0, g->clip.a.y - rect.a.y, rect.b.y),
        clamp(0, g->clip.b.x - rect.a.x, rect.b.x - rect.a.x),
        clamp(0, g->clip.b.y - rect.a.y, rect.b.y - rect.a.y)
    }};
    g->width = rect.b.x - rect.a.x;
    g->height = rect.b.y - rect.a.y;
    g->borrowed = true;
    g->bmp += (int)rect.a.x + (int)rect.a.y * original->stride;
    return g;
}
static void bmp_setGamma(Pg *pg, float gamma) {
    if (pg->gamma == gamma)
        return;
    pg->gamma = gamma;
    initGammaTable(pg->toLinear, pg->toGamma, gamma);
}
Pg *pgNewBitmapCanvas(int width, int height) {
    Pg *g = calloc(1, sizeof *g);
    g->subsamples = 3.0f;
    g->flatness = 1.001f;
    g->resize = bmp_resize;
    g->clear = bmp_clear;
    g->clearSection = bmp_clearSection;
    g->free = bmp_free;
    g->fillPath = bmp_fillPath;
    g->strokePath = bmp_strokePath;
    g->subsection = bmp_subsection;
    g->setGamma = bmp_setGamma;
    pgIdentityMatrix(&g->ctm);
    g->setGamma(g, 2.2f);
    g->resize(g, width, height);
    return g;
}
Pg *pgSubsectionCanvas(Pg *g, PgRect rect) {
    return g->subsection(g, rect);
}
void pgSetGamma(Pg *g, float gamma) {
    g->setGamma(g, gamma);
}
void pgClearCanvas(Pg *g, uint32_t color) {
    g->clear(g, color);
}
void pgClearSection(Pg *g, PgPt a, PgPt b, uint32_t color) {
    g->clearSection(g, a, b, color);
}
void pgFreeCanvas(Pg *g) {
    g->free(g);
}
void pgResizeCanvas(Pg *g, int width, int height) {
    g->resize(g, width, height);
}
void pgIdentity(Pg *g) { pgIdentityMatrix(&g->ctm); }
void pgTranslate(Pg *g, float x, float y) { pgTranslateMatrix(&g->ctm, x, y); }
void pgScale(Pg *g, float x, float y) { pgScaleMatrix(&g->ctm, x, y); }
void pgShear(Pg *g, float x, float y) { pgShearMatrix(&g->ctm, x, y); }
void pgRotate(Pg *g, float rad) { pgRotateMatrix(&g->ctm, rad); }
void pgMultiply(Pg *g, const PgMatrix * __restrict b) { pgMultiplyMatrix(&g->ctm, b); }

void pgIdentityMatrix(PgMatrix *mat) {
    mat->a = 1;
    mat->b = 0;
    mat->c = 0;
    mat->d = 1;
    mat->e = 0;
    mat->f = 0;
}
void pgTranslateMatrix(PgMatrix *mat, float x, float y) {
    mat->e += x;
    mat->f += y;
}
void pgScaleMatrix(PgMatrix *mat, float x, float y) {
    mat->a *= x;
    mat->c *= x;
    mat->e *= x;
    mat->b *= y;
    mat->d *= y;
    mat->f *= y;
}
void pgShearMatrix(PgMatrix *mat, float x, float y) {
    mat->a = mat->a + mat->b * y;
    mat->c = mat->c + mat->d * y;
    mat->e = mat->e + mat->f * y;
    mat->b = mat->a * x + mat->b;
    mat->d = mat->c * x + mat->d;
    mat->f = mat->e * x + mat->f;
}
void pgRotateMatrix(PgMatrix *mat, float rad) {
    PgMatrix old = *mat;
    float m = cosf(rad);
    float n = sinf(rad);
    mat->a = old.a * m - old.b * n;
    mat->b = old.a * n + old.b * m;
    mat->c = old.c * m - old.d * n;
    mat->d = old.c * n + old.d * m;
    mat->e = old.e * m - old.f * n;
    mat->f = old.e * n + old.f * m;
}
void pgMultiplyMatrix(PgMatrix * __restrict a, const PgMatrix * __restrict b) {
    PgMatrix old = *a;

    a->a = old.a * b->a + old.b * b->c;
    a->c = old.c * b->a + old.d * b->c;
    a->e = old.e * b->a + old.f * b->c + b->e;

    a->b = old.a * b->b + old.b * b->d;
    a->d = old.c * b->b + old.d * b->d;
    a->f = old.e * b->b + old.f * b->d + b->f;
}
PgPt pgTransformPoint(PgMatrix ctm, PgPt p) {
    return (PgPt) {
        ctm.a * p.x + ctm.c * p.y + ctm.e,
        ctm.b * p.x + ctm.d * p.y + ctm.f
    };
}
PgPt *pgTransformPoints(PgMatrix ctm, PgPt *v, int n) {
    for (int i = 0; i < n; i++)
        v[i] = pgTransformPoint(ctm, v[i]);
    return v;
}
static void addPathPart(PgPath *path, int type, ...) {
    if (type == PG_MOVE) {
        if (path->nsubs >= path->subCap)
            path->subCap = max(path->subCap * 2, 4),
            path->subs = realloc(path->subs, path->subCap * sizeof(int));
        path->subs[path->nsubs++] = 0;
    } else {
        if (path->ntypes >= path->typeCap)
            path->typeCap = max(path->typeCap * 2, 8),
            path->types = realloc(path->types, path->typeCap * sizeof(int));
        path->types[path->ntypes++] = type;
    }

    if (path->npoints + max(type, 1) >= path->pointCap)
        path->pointCap = max(path->pointCap * 2, 32),
        path->data = realloc(path->data, path->pointCap * sizeof(PgPt));

    va_list ap;
    va_start(ap, type);
    for (int i = 0; i < max(type, 1); i++) {
        path->data[path->npoints++] = va_arg(ap, PgPt);
        path->subs[path->nsubs - 1]++;
    }
    va_end(ap);
}
PgPath *pgNewPath() {
    PgPath *path = calloc(1, sizeof *path);
    return path;
}
void pgClearPath(PgPath *path) {
    path->npoints = 0;
    path->nsubs = 0;
    path->ntypes = 0;
}
void pgFreePath(PgPath *path) {
    free(path->subs);
    free(path->types);
    free(path->data);
    free(path);
}
void pgMove(PgPath *path, PgPt a) {
    addPathPart(path, PG_MOVE, a);
}
void pgLine(PgPath *path, PgPt b) {
    addPathPart(path, PG_LINE, b);
}
void pgQuad(PgPath *path, PgPt b, PgPt c) {
    addPathPart(path, PG_QUAD, b, c);
}
void pgCubic(PgPath *path, PgPt b, PgPt c, PgPt d) {
    addPathPart(path, PG_CUBIC, b, c, d);
}
void pgClosePath(PgPath *path) {
    if (!path->nsubs) return;
    addPathPart(path, PG_LINE, path->data[path->npoints - path->subs[path->nsubs - 1]]);
}
void pgFillPath(Pg *g, uint32_t color, PgPath *path) {
    g->fillPath(g, color, path);
}
void pgStrokePath(Pg *g, uint32_t color, PgPath *path, float width) {
    g->strokePath(g, color, path, width);
}
PgRect pgGetPathBindingBox(PgPath *path, PgMatrix ctm) {
    if (path->npoints == 0) return (PgRect) {{ 0.0f, 0.0f, 0.0f, 0.0f }};
    PgRect r = {{ INFINITY, INFINITY, -INFINITY, -INFINITY }};
    for (PgPt *i = path->data; i < path->data + path->npoints; i++) {
        PgPt p = pgTransformPoint(ctm, *i);
        if (p.x < r.a.x) r.a.x = p.x;
        if (p.x > r.b.x) r.b.x = p.x;
        if (p.y < r.a.y) r.a.y = p.y;
        if (p.y > r.b.y) r.b.y = p.y;
    }
    return r;
}
PgPathStepData pgNewPathStepData(PgPath *path) {
    return (PgPathStepData){.path=path,};
}
PgPathStep *pgNextPathStep(PgPathStepData *data) {
    const PgPath *path = data->path;
    if (data->point >= path->npoints) return NULL;
    PgPt *points = &path->data[data->point];
    PgPathStepType type =   data->point == 0 or data->point == path->subs[data->sub] ?
                                data->sub++,
                                PG_MOVE :
                            path->types[data->type++];
    data->point += type ? type : 1;
    data->step = (PgPathStep){.type=type, points=points};
    return &data->step;
}
PgPath *pgTransformPath(PgPath *path, PgMatrix ctm) {
    pgTransformPoints(ctm, path->data, path->npoints);
    return path;
}


PgStringBuffer *pgPathAsSvgPath(PgStringBuffer *buffer, PgPath *path) {
    if (not buffer)
        buffer = pgNewStringBuffer();
    PgPt *p;
    for (PgPathStepData i = pgNewPathStepData(path); pgNextPathStep(&i) and (p = i.step.points); )
        switch (i.step.type) {
        case PG_MOVE:   pgBufferFormat(buffer, "M%g,%g", p[0].x, p[0].y); break;
        case PG_LINE:   pgBufferFormat(buffer, "L%g,%g", p[0].x, p[0].y); break;
        case PG_QUAD:   pgBufferFormat(buffer, "L%g,%g, %g,%g", p[0].x, p[0].y, p[1].x, p[1].y); break;
        case PG_CUBIC:  pgBufferFormat(buffer, "L%g,%g, %g,%g %g,%g", p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y); break;
        }
    return buffer;
}
int pgGetGlyphNoSubstitute(PgFont *font, int c) {
    return font->getGlyph(font, c);
}
int pgGetGlyph(PgFont *font, int c) {
    int g = pgGetGlyphNoSubstitute(font, c);
    for (int i = 0; i < font->nsubs; i++)
        if (font->subs[i].in == g) {
            g = font->subs[i].out;
            i = -1; // keep substituting
        }
    return g;
}
PgFontFamily *pgScanFonts(void) {
    return _pgScanFonts();
}
PgFont *pgLoadFontHeader(const void *file, int fontIndex) {
    return (PgFont*)pgLoadOpenTypeFontHeader(file, fontIndex);
}
PgFont *pgLoadFontFromMemory(const void *file, int fontIndex) {
    return (PgFont*)pgLoadOpenTypeFont(file, fontIndex);
}
PgFont *pgLoadFontFromFile(const wchar_t *filename, int index) {
    if (!filename)
        return NULL;

    void *host;
    void *data = _pgMapFile(&host, filename);
    if (!data)
        return NULL;
    PgFont *font = (PgFont*)pgLoadFontFromMemory(data, index);
    if (!font) {
        _pgFreeFileMap(host);
        return NULL;
    }
    font->host = host;
    font->freeHost = _pgFreeFileMap;
    return font;
}

// This searches for and loads a font from the system font list.
// The family name is case-insensitive.
// If weight is not found, it will attempt to select the next higher
// then the next lower weight available.
// If the italic style is requested and not available, the roman form
// is looked up with the same rules as above.
PgFont *pgOpenFont(const wchar_t *family, int weight, bool italic) {
    weight /= 100;
    if (weight == 0)
        weight = 4;
    else if (weight >= 10)
        return NULL;
    pgScanFonts();

    PgFontFamily *fam = NULL;
    for (int i = 0; i < PgNFontFamilies; i++)
        if (!wcsicmp(family, PgFontFamilies[i].name)) {
            fam = PgFontFamilies + i;
            break;
        }
    if (!fam)
        return NULL;

    PgFont *font = NULL;
    struct pgFontDesc *i = (italic? fam->italic: fam->roman) + weight;
    struct pgFontDesc *r = fam->roman + weight;
    return  (font = pgLoadFontFromFile(i[0].path, i[0].index)) ||
            (font = pgLoadFontFromFile(i[1].path, i[1].index)) ||
            (font = pgLoadFontFromFile(i[-1].path, i[-1].index)) ||
            (font = pgLoadFontFromFile(r[0].path, r[0].index)) ||
            (font = pgLoadFontFromFile(r[1].path, r[1].index)) ||
            (font = pgLoadFontFromFile(r[-1].path, r[-1].index))
        ? font
        : NULL;
}
void pgFreeFont(PgFont *font) {
    free(font->subs);
    free((void*) font->styleName);
    free((void*) font->familyName);
    free((void*) font->name);
    if (font->freeHost)
        font->freeHost(font->host);
    if (font->free)
        font->free(font);
    free(font);
}
uint32_t *pgGetFontFeatures(PgFont *font) {
    return font->getFeatures(font);
}
void pgSetFontFeatures(PgFont *font, const uint32_t *tags) {
    font->setFeatures(font, tags);
}
void pgScaleFont(PgFont *font, float x, float y) {
    if (!x) x = y;
    if (!y) y = x;
    font->ctm.a = x / font->em;
    font->ctm.d = y / font->em;
}
PgPath *pgGetGlyphPath(PgFont *font, PgPath *path, int glyph) {
    return font->getGlyphPath(font, path, glyph);
}
PgPath *pgGetCharPath(PgFont *font, PgPath *path, int c) {
    return pgGetGlyphPath(font, path, pgGetGlyph(font, c));
}
float pgGetGlyphWidth(PgFont *font, int glyph) {
    return font->getGlyphWidth(font, glyph);
}
float pgGetCharWidth(PgFont *font, int c) {
    return pgGetGlyphWidth(font, pgGetGlyph(font, c));
}
float pgGetStringWidth(PgFont *font, const wchar_t *text, int len) {
    if (len < 0) len = wcslen(text);
    float x = 0;
    for (int i = 0; i < len; i++)
        x += pgGetCharWidth(font, text[i]);
    return x;
}
float pgGetFontEm(PgFont *font) {
    return font->ctm.a * font->em;
}
float pgGetFontHeight(PgFont *font) {
    return pgGetFontAscender(font) - pgGetFontDescender(font);
}
float pgGetFontXHeight(PgFont *font) {
    return font->ctm.d * font->xHeight;
}
float pgGetFontCapHeight(PgFont *font) {
    return font->ctm.d * font->capHeight;
}
float pgGetFontAscender(PgFont *font) {
    return font->ctm.d * font->ascender;
}
float pgGetFontDescender(PgFont *font) {
    return font->ctm.d * font->descender;
}
float pgGetFontLineGap(PgFont *font) {
    return font->ctm.d * font->lineGap;
}
int pgGetFontWeight(PgFont *font) {
    return font->weight;
}
bool pgIsFontItalic(PgFont *font) {
    return font->isItalic;
}
bool pgIsFontFixedPitched(PgFont *font) {
    return font->isFixedPitched;
}
const wchar_t *pgGetFontName(PgFont *font) {
    return font->name;
}
const wchar_t *pgGetFontFamilyName(PgFont *font) {
    return font->familyName;
}
const wchar_t *pgGetFontStyleName(PgFont *font) {
    return font->styleName;
}
void pgSubstituteGlyph(PgFont *font, uint16_t in, uint16_t out) {
    font->subs = realloc(font->subs, (font->nsubs + 1) * sizeof *font->subs);
    font->subs[font->nsubs].in = in;
    font->subs[font->nsubs].out = out;
    font->nsubs++;
}
float pgFillGlyph(Pg *g, PgFont *font, uint32_t color, float x, float y, int glyph) {
    PgPath *path = pgGetGlyphPath(font, NULL, glyph);
    if (path) {
        for (int i = 0; i < path->npoints; i++)
            path->data[i].x += x,
            path->data[i].y += y;
        pgFillPath(g, color, path);
        pgFreePath(path);
    }
    return x + pgGetGlyphWidth(font, glyph);
}
float pgFillChar(Pg *g, PgFont *font, uint32_t color, float x, float y, int c) {
    return pgFillGlyph(g, font, color,  x, y, pgGetGlyph(font, c));
}
float pgFillString(Pg *g, PgFont *font, uint32_t color, float x, float y, const wchar_t *text, int len) {
    if (len < 0) len = wcslen(text);
    for (int i = 0; i < len; i++)
        x = pgFillChar(g, font, color, x, y, text[i]);
    return x;
}
float pgFillUtf8(Pg *g, PgFont *font, uint32_t color, float x, float y, const char *text, int len) {
    if (len < 0) len = strlen(text);
    const char *end = text + len;
    const char *p = text;
    while (p < end)
        x = pgFillChar(g, font, color, x, y, pgStepUtf8(&p));
    return x;
}
float pgPrintf(Pg *g, PgFont *font, uint32_t color, float x, float y, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int len = vsnprintf(NULL, 0, format, ap);
    va_start(ap, format);
    char *buffer = malloc(len + 1);
    vsprintf(buffer, format, ap);
    x = pgFillUtf8(g, font, color, x, y, buffer, len);
    free(buffer);
    va_end(ap);
    return x;
}
void pgFillRect(Pg *g, uint32_t color, PgPt a, PgPt b) {
    PgPath *path = pgNewPath();
    pgMove(path, a);
    pgLine(path, pgPt(b.x, a.y));
    pgLine(path, b);
    pgLine(path, pgPt(a.x, b.y));
    pgFillPath(g, color, path);
    pgFreePath(path);
}
void pgStrokeRect(Pg *g, uint32_t color, PgPt a, PgPt b, float width) {
    PgPath *path = pgNewPath();
    pgMove(path, a);
    pgLine(path, pgPt(b.x, a.y));
    pgLine(path, b);
    pgLine(path, pgPt(a.x, b.y));
    pgClosePath(path);
    pgStrokePath(g, color, path, width);
    pgFreePath(path);
}
void pgStrokeLine(Pg *g, uint32_t color, PgPt a, PgPt b, float width) {
    PgPath *path = pgNewPath();
    pgMove(path, a);
    pgLine(path, b);
    pgStrokePath(g, color, path, width);
    pgFreePath(path);
}
void pgStrokeHLine(Pg *g, uint32_t color, PgPt a, float x2, float width) {
    pgStrokeLine(g, color, a, pgPt(x2, a.y), width);
}
void pgStrokeVLine(Pg *g, uint32_t color, PgPt a, float y2, float width) {
    pgStrokeLine(g, color, a, pgPt(a.x, y2), width);
}
PgPath *pgInterpretSvgPath(PgPath *path, const char *svg) {
    PgPt    start = {0.0f, 0.0f};
    PgPt    a = start;
    PgPt    b = start;
    PgPt    c = start;
    float   args[6];
    char    cmd = 0;
    int     argsNeeded = 0;

    if (not path)
        path = pgNewPath();

    while (true) {
        while (*svg and (isspace(*svg) or *svg==',')) svg++;

        switch (tolower(*svg)) {
        case 'c': argsNeeded = 6; cmd = *svg++; break;
        case 'h': argsNeeded = 1; cmd = *svg++; break;
        case 'l': argsNeeded = 2; cmd = *svg++; break;
        case 'm': argsNeeded = 2; cmd = *svg++; break;
        case 'q': argsNeeded = 4; cmd = *svg++; break;
        case 's': argsNeeded = 4; cmd = *svg++; break;
        case 't': argsNeeded = 2; cmd = *svg++; break;
        case 'v': argsNeeded = 1; cmd = *svg++; break;
        case 'z': argsNeeded = 0; cmd = *svg++; break;
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
        case '-': case '+': case '.':
            if (argsNeeded == 0) return path;
            break;
        default: return path; // invalid command
        }

        const char *before = svg;
        for (int i = 0; i < argsNeeded; i++) {
            while (*svg and (isspace(*svg) or *svg==',')) svg++;
            const char *before = svg;
            args[i] = strtod(svg, (char**)&svg);
            if (before == svg) svg++; // strtod will not accept -, +, or . without a digit after it
        }

//        if (path->npoints == 0) puts("\n");
//        putchar(cmd);
//        for (int i = 0; i < argsNeeded; i++) printf(" %g", args[i]);
//        putchar('\n');

        switch (cmd) {
        case 'M':
            start = a = pgPt(args[0], args[1]);
            pgMove(path, a);
            break;
        case 'm':
            start = a = pgAddPt(a, pgPt(args[0], args[1]));
            pgMove(path, a);
            break;
        case 'L':
            a = pgPt(args[0], args[1]);
            pgLine(path, a);
            break;
        case 'l':
            a = pgAddPt(a, pgPt(args[0], args[1]));
            pgLine(path, a);
            break;
        case 'H':
            a = pgPt(args[0], a.y);
            pgLine(path, a);
            break;
        case 'h':
            a = pgAddPt(a, pgPt(args[0], 0.0f));
            pgLine(path, a);
            break;
        case 'V':
            a = pgPt(a.x, args[0]);
            pgLine(path, a);
            break;
        case 'v':
            a = pgAddPt(a, pgPt(0, args[0]));
            pgLine(path, a);
            break;
        case 'C':
            b = pgPt(args[0], args[1]);
            c = pgPt(args[2], args[3]);
            a = pgPt(args[4], args[5]);
            pgCubic(path, b, c, a);
            break;
        case 'c':
            b = pgAddPt(a, pgPt(args[0], args[1]));
            c = pgAddPt(a, pgPt(args[2], args[3]));
            a = pgAddPt(a, pgPt(args[4], args[5]));
            pgCubic(path, b, c, a);
            break;
        case 'S':
            b = pgAddPt(a, pgSubtractPt(a, c));
            c = pgPt(args[0], args[1]);
            a = pgPt(args[2], args[3]);
            pgCubic(path, b, c, a);
            break;
        case 's':
            b = pgAddPt(a, pgSubtractPt(a, c));
            c = pgAddPt(a, pgPt(args[0], args[1]));
            a = pgAddPt(a, pgPt(args[2], args[3]));
            pgCubic(path, b, c, a);
            break;
        case 'Q':
            b = pgPt(args[0], args[1]);
            a = pgPt(args[2], args[3]);
            pgQuad(path, b, a);
            break;
        case 'q':
            b = pgAddPt(a, pgPt(args[0], args[1]));
            a = pgAddPt(a, pgPt(args[2], args[3]));
            pgQuad(path, b, a);
            break;
        case 'T':
            b = pgAddPt(a, pgSubtractPt(a, b));
            a = pgPt(args[0], args[1]);
            pgQuad(path, b, a);
            break;
        case 't':
            b = pgAddPt(a, pgSubtractPt(a, b));
            a = pgAddPt(a, pgPt(args[0], args[1]));
            pgQuad(path, b, a);
            break;
        case 'Z':
        case 'z':
            pgClosePath(path);
            a = start;
            break;
        }
    }
}
