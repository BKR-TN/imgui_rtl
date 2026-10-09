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

// Match the sizing the shaper and the FreeType loader use (REAL_DIM), so the metrics dumped
// here are on the same scale as what the pipeline actually computes.
static void ft_set_size_real_dim(FT_Face face, float size_px)
{
    FT_Size_RequestRec req;
    req.type = FT_SIZE_REQUEST_TYPE_REAL_DIM;
    req.width = 0;
    req.height = (FT_UInt)(size_px * 64.0f);
    req.horiResolution = 0;
    req.vertResolution = 0;
    FT_Request_Size(face, &req);
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s font.ttf\n", argv[0]);
        return 1;
    }

    FT_Library lib; FT_Init_FreeType(&lib);
    FT_Face face; FT_New_Face(lib, argv[1], 0, &face);
    ft_set_size_real_dim(face, 26.0f);
    raqm_t* rq = raqm_create();
    dump("ra+shadda+fatha (رَّ)", rq, face, "\xD8\xB1\xD9\x91\xD9\x8E");
    dump("meem+fatha (مَ)", rq, face, "\xD9\x85\xD9\x8E");
    dump("lam+alef+lam (لال)", rq, face, "\xD9\x84\xD8\xA7\xD9\x84");
    raqm_destroy(rq); FT_Done_Face(face); FT_Done_FreeType(lib);
    return 0;
}
