// Dump the exact glyph draw positions produced by the RTL shaped render path.
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_rtl.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s font.ttf [size_px]\n", argv[0]);
        return 1;
    }
    const char* font_path = argv[1];
    float font_size = (argc > 2) ? (float)atof(argv[2]) : 26.0f;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    ImFont* font = io.Fonts->AddFontFromFileTTF(font_path, font_size, NULL);
    if (!font) { fprintf(stderr, "font load failed: %s\n", font_path); return 1; }
    io.Fonts->Build();

    ImFontBaked* baked = font->GetFontBaked(font_size);
    printf("baked: size=%.1f ascent=%.2f descent=%.2f\n", baked->Size, baked->Ascent, baked->Descent);

    // No shaper means there is no shaped render path to dump (this tool builds with
    // IMGUI_ENABLE_RTL, so a NULL shaper indicates the backend failed to attach).
    const ImFontShaper* shaper = io.Fonts->FontShaper;
    printf("shaper: %s\n", shaper ? shaper->Name : "(none)");
    if (shaper == NULL || shaper->ShapeText == NULL)
    {
        fprintf(stderr, "no shaper attached: nothing to dump (is this built with IMGUI_ENABLE_RTL?)\n");
        ImGui::DestroyContext();
        return 1;
    }

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
        const bool ok = shaper->ShapeText(font, baked, text, end, &g, &n, &dir);
        printf("\n=== '%s' dir=%s n=%d%s ===\n", samples[s], dir ? "RTL" : "LTR", n, ok ? "" : " SHAPE-FAILED");

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
