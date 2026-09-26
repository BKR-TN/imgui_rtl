#include <stdio.h>
#include <string.h>
#include <raqm.h>
#include <ft2build.h>
#include FT_FREETYPE_H

static void dump(const char* label, raqm_t* rq, FT_Face face, const char* utf8)
{
    raqm_set_text_utf8(rq, utf8, strlen(utf8));
    raqm_set_freetype_face(rq, face);
    raqm_set_par_direction(rq, RAQM_DIRECTION_RTL);
    raqm_layout(rq);
    size_t n = 0;
    raqm_glyph_t* g = raqm_get_glyphs(rq, &n);
    printf("=== %s ===\n", label);
    for (size_t i = 0; i < n; i++)
    {
        FT_Load_Glyph(face, g[i].index, FT_LOAD_DEFAULT);
        FT_GlyphSlot slot = face->glyph;
        printf("  gid=%u  hb: xadv=%d(%.2fpx) xoff=%d(%.2f) yoff=%d(%.2f) | ft: bearingX=%d bearingY=%d w=%d h=%d\n",
               g[i].index,
               g[i].x_advance, g[i].x_advance / 64.0f,
               g[i].x_offset, g[i].x_offset / 64.0f,
               g[i].y_offset, g[i].y_offset / 64.0f,
               slot->bitmap_left, slot->bitmap_top,
               slot->bitmap.width, slot->bitmap.rows);
    }
    raqm_clear_contents(rq);
}

int main(int argc, char** argv)
{
    FT_Library lib; FT_Init_FreeType(&lib);
    FT_Face face; FT_New_Face(lib, argv[1], 0, &face);
    FT_Set_Char_Size(face, 0, 26 * 64, 0, 72);
    raqm_t* rq = raqm_create();
    dump("ra+shadda+fatha (رَّ)", rq, face, "\xD8\xB1\xD9\x91\xD9\x8E");
    dump("meem+fatha (مَ)", rq, face, "\xD9\x85\xD9\x8E");
    dump("lam+alef+lam (لال)", rq, face, "\xD9\x84\xD8\xA7\xD9\x84");
    raqm_destroy(rq); FT_Done_Face(face); FT_Done_FreeType(lib);
    return 0;
}
