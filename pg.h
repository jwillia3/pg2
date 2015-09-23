typedef struct { float x, y; } PgPt;
typedef union {
    struct { float x1, y1, x2, y2; };
    struct { PgPt a, b; };
} PgRect;
typedef struct { float a, b, c, d, e, f; } PgMatrix;
typedef struct {
    int npoints, pointCap;
    int nsubs, subCap;
    int ntypes, typeCap;
    int *subs;
    enum { PG_MOVE, PG_LINE, PG_QUAD, PG_CUBIC } *types;
    PgPt *data;
} PgPath;
typedef struct Pg Pg;
struct Pg {
    int width;
    int height;
    PgMatrix ctm;
    PgRect clip;
    void (*resize)(Pg *pg, int width, int height);
    void (*clear)(Pg *pg, uint32_t color);
    void (*free)(Pg *pg);
    void (*fillPath)(Pg *g, PgPath *path, uint32_t color);
    void (*strokePath)(Pg *g, PgPath *path, float width, uint32_t color);
    Pg *(*subsection)(Pg *pg, PgRect rect);
    uint32_t *bmp;
    int stride;
    bool borrowed;
};
typedef struct PgFont PgFont;
struct PgFont {
    const void *file;
    void *host;
    PgMatrix ctm;
    float em, xHeight, capHeight, ascender, descender, lineGap;
    uint32_t panose[10];
    int weight;
    bool isItalic;
    bool isFixedPitched;
    const wchar_t *familyName, *name, *styleName;
    int nfonts;
    struct { uint16_t in, out; } *subs;
    int nsubs;
    
    void (*free)(PgFont *font);
    void (*freeHost)(void *host);
    PgPath *(*getGlyphPath)(PgFont *font, PgPath *path, int glyph);
    int (*getGlyph)(PgFont *font, int glyph);
    float (*getGlyphWidth)(PgFont *font, int glyph);
    uint32_t *(*getFeatures)(PgFont *font);
    void (*setFeatures)(PgFont *font, const uint32_t *tags);
};
typedef struct {
    PgFont _;
    const int16_t *hmtx;
    const void *glyf;
    const void *loca;
    const void *gsub;
    uint16_t *cmap;
    bool longLoca;
    int nhmtx;
    int nglyphs;
    uint32_t lang;
    uint32_t script;
} PgOpenTypeFont;
typedef struct {
    const wchar_t *name;
    const wchar_t *roman[10];
    const wchar_t *italic[10];
    int romanIndex[10];
    int italicIndex[10];
} PgFontFamily;

PgFontFamily    *PgFontFamilies;
int             PgNFontFamilies;


static PgPt pgPt(float x, float y) { return (PgPt){x,y}; }
static PgRect pgRect(PgPt a, PgPt b) { return (PgRect){ .a = a, .b = b }; }
unsigned pgStepUtf8(const uint8_t **input);
uint32_t pgBlend(uint32_t fg, uint32_t bg, uint32_t a);

// CANVAS MANAGEMENT
    Pg *pgNewBitmapCanvas(int width, int height);
    Pg *pgSubsectionCanvas(Pg *g, PgRect rect);
    void pgClearCanvas(Pg *g, uint32_t color);
    void pgFreeCanvas(Pg *g);
    void pgResizeCanvas(Pg *g, int width, int height);
    // CANVAS MATRIX MANAGEMENT
    void pgIdentity(Pg *g);
    void pgTranslate(Pg *g, float x, float y);
    void pgScale(Pg *g, float x, float y);
    void pgShear(Pg *g, float x, float y);
    void pgRotate(Pg *g, float rad);
    void pgMultiply(Pg *g, const PgMatrix * __restrict b);
    
// MATRIX MANAGEMENT
    static PgMatrix PgIdentity = { 1, 0, 0, 1, 0, 0 };
    void pgIdentityMatrix(PgMatrix *mat);
    void pgTranslateMatrix(PgMatrix *mat, float x, float y);
    void pgScaleMatrix(PgMatrix *mat, float x, float y);
    void pgShearMatrix(PgMatrix *mat, float x, float y);
    void pgRotateMatrix(PgMatrix *mat, float rad);
    void pgMultiplyMatrix(PgMatrix * __restrict a, const PgMatrix * __restrict b);
    PgPt pgTransformPoint(PgMatrix ctm, PgPt p);
    PgPt *pgTransformPoints(PgMatrix ctm, PgPt *v, int n);
// Path management
    PgPath *pgNewPath();
    void pgFreePath(PgPath *path);
    void pgClearPath(PgPath *path);
    void pgMove(PgPath *path, PgPt a);
    void pgLine(PgPath *path, PgPt b);
    void pgQuad(PgPath *path, PgPt b, PgPt c);
    void pgCubic(PgPath *path, PgPt b, PgPt c, PgPt d);
    void pgClosePath(PgPath *path);
    void pgFillPath(Pg *g, PgPath *path, uint32_t color);
    void pgStrokePath(Pg *g, PgPath *path, float width, uint32_t color);
// Fonts
    PgFontFamily *pgScanFonts();
    PgFont *pgLoadFontHeader(const void *file, int fontIndex);
    PgFont *pgLoadFont(const void *file, int fontIndex);
    PgFont *pgOpenFont(const wchar_t *family, int weight, bool italic);
    PgOpenTypeFont *pgLoadOpenTypeFontHeader(const void *file, int fontIndex);
    PgOpenTypeFont *pgLoadOpenTypeFont(const void *file, int fontIndex);
    
    float pgGetFontEm(PgFont *font);
    float pgGetFontHeight(PgFont *font);
    float pgGetFontBaseline(PgFont *font);
    float pgGetFontXHeight(PgFont *font);
    float pgGetFontCapHeight(PgFont *font);
    float pgGetFontAscender(PgFont *font);
    float pgGetFontDescender(PgFont *font);
    float pgGetFontLineGap(PgFont *font);
    int pgGetFontWeight(PgFont *font);
    bool pgIsFontItalic(PgFont *font);
    bool pgIsFontFixedPitched(PgFont *font);
    const wchar_t *pgGetFontName(PgFont *font);
    const wchar_t *pgGetFontFamilyName(PgFont *font);
    const wchar_t *pgGetFontStyleName(PgFont *font);
    
    
    PgFont *pgLoadFontFromFile(const wchar_t *filename, int index);
    uint32_t *pgGetFontFeatures(PgFont *font);
    void pgSetFontFeatures(PgFont *font, const uint32_t *tags);
    int pgGetGlyph(PgFont *font, int c);
    int pgGetGlyphNoSubstitute(PgFont *font, int c);
    void pgSubstituteGlyph(PgFont *font, uint16_t in, uint16_t out);
    void pgFreeFont(PgFont *font);
    void pgScaleFont(PgFont *font, float x, float y);
    PgPath *pgGetGlyphPath(PgFont *font, PgPath *path, int glyph);
    PgPath *pgGetCharPath(PgFont *font, PgPath *path, int c);
    float pgFillGlyph(Pg *g, PgFont *font, float x, float y, int glyph, uint32_t color);
    float pgFillChar(Pg *g, PgFont *font, float x, float y, int c, uint32_t color);
    float pgFillString(Pg *g, PgFont *font, float x, float y, const wchar_t *text, int len, uint32_t color);
    float pgFillUtf8(Pg *g, PgFont *font, float x, float y, const char *text, int len, uint32_t color);
    float pgGetCharWidth(PgFont *font, int c);
    float pgGetGlyphWidth(PgFont *font, int c);
    float pgGetStringWidth(PgFont *font, const wchar_t *text, int len);
    void pgFillRect(Pg *g, PgPt a, PgPt b, uint32_t color);
    void pgStrokeRect(Pg *g, PgPt a, PgPt b, float width, uint32_t color);
    void pgStrokeLine(Pg *g, PgPt a, PgPt b, float width, uint32_t color);
    void pgStrokeHLine(Pg *g, PgPt a, float x2, float width, uint32_t color);
    void pgStrokeVLine(Pg *g, PgPt a, float y2, float width, uint32_t color);