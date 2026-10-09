// Compare the FreeType sizing requests that matter for the shaper.
//
// The shaper/loader use FT_Request_Size with FT_SIZE_REQUEST_TYPE_REAL_DIM (see
// ImGuiRTL_EnsureFacesSized() in imgui_rtl.cpp, and the matching loader comment in
// misc/freetype/imgui_freetype.cpp). REAL_DIM makes ascender/descender agree with the glyph
// loader's scale; FT_Set_Char_Size (NOMINAL) does not, which is what caused the original
// "Noto Naskh Arabic has gaps" bug. This tool exists to show that difference per font.
#include <stdio.h>
#include <ft2build.h>
#include FT_FREETYPE_H

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s font.ttf [size_px]\n", argv[0]);
        return 1;
    }
    const float size = (argc > 2) ? (float)atof(argv[2]) : 26.0f;

    FT_Library lib;
    if (FT_Init_FreeType(&lib) != 0) { fprintf(stderr, "FT_Init_FreeType failed\n"); return 1; }
    FT_Face face;
    if (FT_New_Face(lib, argv[1], 0, &face) != 0) { fprintf(stderr, "FT_New_Face failed: %s\n", argv[1]); FT_Done_FreeType(lib); return 1; }

    // NOMINAL (FT_Set_Char_Size): what the shaper used before the fix.
    FT_Set_Char_Size(face, 0, (FT_F26Dot6)(size * 64.0f), 0, 72);
    printf("NOMINAL   (FT_Set_Char_Size %gpx): ppem x=%d y=%d, scale x=%ld y=%ld, ascender=%ld descender=%ld upem=%d\n",
           size, face->size->metrics.x_ppem, face->size->metrics.y_ppem,
           (long)face->size->metrics.x_scale, (long)face->size->metrics.y_scale,
           (long)face->size->metrics.ascender, (long)face->size->metrics.descender, face->units_per_EM);

    // REAL_DIM (FT_Request_Size): what the shaper and the FreeType loader use now.
    FT_Size_RequestRec req;
    req.type = FT_SIZE_REQUEST_TYPE_REAL_DIM;
    req.width = 0;
    req.height = (FT_UInt)(size * 64.0f);
    req.horiResolution = 0;
    req.vertResolution = 0;
    FT_Request_Size(face, &req);
    printf("REAL_DIM  (FT_Request_Size %gpx): ppem x=%d y=%d, scale x=%ld y=%ld, ascender=%ld descender=%ld\n",
           size, face->size->metrics.x_ppem, face->size->metrics.y_ppem,
           (long)face->size->metrics.x_scale, (long)face->size->metrics.y_scale,
           (long)face->size->metrics.ascender, (long)face->size->metrics.descender);

    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return 0;
}
