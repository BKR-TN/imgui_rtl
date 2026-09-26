// Dump the exact glyph draw positions produced by the RTL shaped render path.
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_rtl.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    const char* font_path = (argc > 1) ? argv[1] : "fonts/NotoNaskhArabic.ttf";
    float font_size = (argc > 2) ? (float)atof(argv[2]) : 26.0f;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    ImFont* font = io.Fonts->AddFontFromFileTTF(font_path, font_size, NULL);
    if (!font) { fprintf(stderr, "font load failed\n"); return 1; }
    io.Fonts->Build();

    ImFontBaked* baked = font->GetFontBaked(font_size);
    printf("baked: size=%.1f ascent=%.2f descent=%.2f\n", baked->Size, baked->Ascent, baked->Descent);

    const ImFontShaper* shaper = io.Fonts->FontShaper;
    printf("shaper: %s\n", shaper ? shaper->Name : "(none)");

    const char* samples[] = {
        "بِسْمِ",                       // bismi (kasra marks)
        "رَّ",                          // ra + shadda + fatha
        "سلام",                         // seen lam alef meem (connected)
        NULL
    };

    for (int s = 0; samples[s]; s++)
    {
        const char* text = samples[s];
        const char* end = text + strlen(text);
        const ImShapedGlyph* g = NULL;
        int n = 0, dir = 0;
        bool ok = shaper->ShapeText(font, baked, text, end, &g, &n, &dir);
        printf("\n=== '%s' dir=%s n=%d ===\n", samples[s], dir ? "RTL" : "LTR", n);

        float x = 0.0f;
        for (int i = 0; i < n; i++)
        {
            ImFontGlyph* glyph = baked->FindGlyphByIndex(g[i].GlyphId, g[i].SourceIdx);
            float scale = font_size / baked->Size;
            float px = x + g[i].XOffset * scale;
            float py = 0.0f + g[i].YOffset * scale;   // y=0 is line top here
            float x1 = px + (glyph ? glyph->X0 : 0) * scale;
            float x2 = px + (glyph ? glyph->X1 : 0) * scale;
            float y1 = py + (glyph ? glyph->Y0 : 0) * scale;
            float y2 = py + (glyph ? glyph->Y1 : 0) * scale;
            printf("  [%d] gid=%u adv=%.2f xoff=%.2f yoff=%.2f | quad x=[%.2f,%.2f] y=[%.2f,%.2f] (baseline y=%.2f) %s\n",
                   i, g[i].GlyphId, g[i].XAdvance, g[i].XOffset, g[i].YOffset,
                   x1, x2, y1, y2, baked->Ascent,
                   (glyph && glyph->Visible) ? "" : "(INVISIBLE/MISSING)");
            x += g[i].XAdvance * scale;
        }
        printf("  total advance = %.2f\n", x);
    }

    ImGui::DestroyContext();
    return 0;
}
