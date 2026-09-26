#include <stdio.h>
#include <string.h>
#include <raqm.h>
#include <ft2build.h>
#include FT_FREETYPE_H

int main(int argc, char** argv)
{
    FT_Library lib; FT_Init_FreeType(&lib);
    FT_Face face; FT_New_Face(lib, argv[1], 0, &face);
    FT_Set_Char_Size(face, 0, 22 * 64, 0, 72);

    raqm_t* rq = raqm_create();
    const char* text = "\xD8\xA7\xD9\x84\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xD8\xB9\xD9\x84\xD9\x8A\xD9\x83\xD9\x85"; // السلام عليكم
    raqm_set_text_utf8(rq, text, strlen(text));
    raqm_set_freetype_face(rq, face);
    raqm_set_par_direction(rq, RAQM_DIRECTION_DEFAULT);
    raqm_layout(rq);

    printf("text bytes=%zu\n", strlen(text));
    size_t n = 0;
    raqm_glyph_t* g = raqm_get_glyphs(rq, &n);
    printf("glyphs n=%zu (visual order):\n", n);
    for (size_t i = 0; i < n; i++)
        printf("  [%zu] gid=%u cluster=%u x_adv=%d\n", i, g[i].index, g[i].cluster, g[i].x_advance);

    printf("\nraqm_index_to_position (byte -> x, 26.6):\n");
    for (size_t i = 0; i <= strlen(text); i++)
    {
        size_t idx = i;
        int x = 0, y = 0;
        bool ok = raqm_index_to_position(rq, &idx, &x, &y);
        printf("  byte %2zu -> x=%d (%.2fpx) idx_after=%zu ok=%d\n", i, x, x/64.0f, idx, ok);
    }

    printf("\nraqm_position_to_index (x -> byte):\n");
    for (int x = 0; x <= 53; x += 6)
    {
        size_t idx = 0;
        bool ok = raqm_position_to_index(rq, x, 0, &idx);
        printf("  x=%d -> byte %zu ok=%d\n", x, idx, ok);
    }

    raqm_destroy(rq); FT_Done_Face(face); FT_Done_FreeType(lib);
    return 0;
}
