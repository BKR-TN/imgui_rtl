// Probe of libraqm's own cursor-mapping APIs (raqm_index_to_position / raqm_position_to_index).
//
// NOTE: the shaper does NOT use these. It was tried and abandoned: the three raqm-based mappings
// disagreed with each other in mixed bidi text, and XOffsetToIndex ended up re-running raqm_layout()
// per call. The shaper instead builds a cached caret-stop list from the shaped glyphs
// (ImGuiRTL_BuildCaretStops() in imgui_rtl.cpp); see misc/rtl/DESIGN.md section 8.
//
// Kept as a standalone probe of upstream raqm behaviour when investigating caret issues.
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <raqm.h>
#include <ft2build.h>
#include FT_FREETYPE_H

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
    ft_set_size_real_dim(face, 22.0f);

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
