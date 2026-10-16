// fonttest: sceLibFont through the runtime's free-font implementation (docs/05-runtime-hle.md, fonts).
// Our own code; GPL-2.0-only like the rest of the repository.
#include <pspkernel.h>
#include <pspfont.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PSP_MODULE_INFO("fonttest", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

static int passed, failed;

static void check(const char *name, int ok) {
    if (ok) { ++passed; } else { ++failed; printf("FAIL %s\n", name); }
}

static void *alloc_fn(void *data, unsigned int size) { return malloc(size); }
static void free_fn(void *data, void *p) { free(p); }

static unsigned char image8[32 * 32];
static unsigned char image4[16 * 32 + 16]; // 32x32 at 4 bpp, plus a guard area

int main(void) {
    SceFontLibData lib_data;
    memset(&lib_data, 0, sizeof(lib_data));
    lib_data.maxFonts = 4;
    lib_data.alloc = alloc_fn;
    lib_data.free = free_fn;
    int error = -1;
    void *lib = sceFontNewLib(&lib_data, &error);
    check("newlib", lib != 0 && error == 0);
    check("numfonts", sceFontGetNumFontList(lib, &error) >= 1);

    SceFontStyle style;
    memset(&style, 0, sizeof(style));
    style.family = SCE_FONT_FAMILY_SANS_SERIF;
    style.style = SCE_FONT_STYLE_REGULAR;
    style.language = SCE_FONT_LANGUAGE_ENGLISH;
    int index = sceFontFindOptimumFont(lib, &style, &error);
    check("optimum", index >= 0 && error == 0);
    void *font = sceFontOpen(lib, index, 0, &error);
    check("open", font != 0 && error == 0);

    SceFontInfo info;
    memset(&info, 0, sizeof(info));
    check("fontinfo", sceFontGetFontInfo(font, &info) == 0);
    check("fontinfo depth", info.depth == 4);
    check("fontinfo height", info.maxHeight >= 12 && info.maxHeight <= 32);
    check("fontinfo fixed/float", info.maxFixed.height == (int)(info.maxFloat.height * 64.0f + 0.5f));

    SceFontCharacterInfo a, i;
    check("charinfo A", sceFontGetCharInfo(font, 'A', &a) == 0);
    check("A size", a.width > 4 && a.height > 8 && a.width < 32 && a.height < 32);
    check("A top", a.y > 8 && a.y < 20);
    check("A advance", a.data.hShift > 4 * 64 && a.data.hShift < 24 * 64);
    sceFontGetCharInfo(font, 'i', &i);
    check("i narrower than A", i.data.hShift < a.data.hShift);

    SceFontImageBuffer buffer;
    memset(image8, 0, sizeof(image8));
    buffer.pixelFormat = SCE_FONT_PIXEL_FORMAT_8;
    buffer.x = 2 * 64;
    buffer.y = 2 * 64;
    buffer.size.width = 32;
    buffer.size.height = 32;
    buffer.bytesPerLine = 32;
    buffer.data = image8;
    check("glyph 8bpp", sceFontGetCharGlyphImage(font, 'A', &buffer) == 0);
    int lit = 0, outside = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            if (!image8[y * 32 + x]) continue;
            ++lit;
            if (x < 2 || y < 2 || x >= 2 + (int)a.width + 1 || y >= 2 + (int)a.height + 1) ++outside;
        }
    check("glyph pixels", lit > 20);
    check("glyph inside its box", outside == 0);

    memset(image4, 0xEE, sizeof(image4));
    memset(image4, 0, 16 * 32);
    buffer.pixelFormat = SCE_FONT_PIXEL_FORMAT_L4;
    buffer.x = 30 * 64; // mostly off the right edge: must be clipped
    buffer.bytesPerLine = 16;
    buffer.data = image4;
    check("glyph 4bpp", sceFontGetCharGlyphImage(font, 'W', &buffer) == 0);
    int guard = 1;
    for (int k = 16 * 32; k < (int)sizeof(image4); ++k) guard &= image4[k] == 0xEE;
    check("clipped at buffer end", guard);

    check("close", sceFontClose(font) == 0);
    check("donelib", sceFontDoneLib(lib) == 0);

    printf("fonttest: %d passed, %d failed\n", passed, failed);
    sceKernelExitGame();
    return 0;
}
