#include <stdio.h>
#include <ft2build.h>
#include FT_FREETYPE_H

int main(int argc, char** argv)
{
    FT_Library lib; FT_Init_FreeType(&lib);
    FT_Face face; FT_New_Face(lib, argv[1], 0, &face);

    float size = 26.0f;

    // My shaper: FT_Set_Char_Size (NOMINAL)
    FT_Set_Char_Size(face, 0, (FT_F26Dot6)(size * 64.0f), 0, 72);
    printf("NOMINAL (FT_Set_Char_Size %gpx): ppem x=%d y=%d, scale x=%ld y=%ld, ascender=%d descender=%d upem=%d\n",
           size, face->size->metrics.x_ppem, face->size->metrics.y_ppem,
           face->size->metrics.x_scale, face->size->metrics.y_scale,
           face->size->metrics.ascender, face->size->metrics.descender, face->units_per_EM);

    // Loader: FT_Request_Size REAL_DIM
    FT_Size_RequestRec req;
    req.type = FT_SIZE_REQUEST_TYPE_REAL_DIM;
    req.width = 0;
    req.height = (FT_UInt)(size * 64.0f * 1.0f);
    req.horiResolution = 0;
    req.vertResolution = 0;
    FT_Request_Size(face, &req);
    printf("REAL_DIM (FT_Request_Size %gpx): ppem x=%d y=%d, scale x=%ld y=%ld, ascender=%d descender=%d\n",
           size, face->size->metrics.x_ppem, face->size->metrics.y_ppem,
           face->size->metrics.x_scale, face->size->metrics.y_scale,
           face->size->metrics.ascender, face->size->metrics.descender);

    FT_Done_Face(face); FT_Done_FreeType(lib);
    return 0;
}
