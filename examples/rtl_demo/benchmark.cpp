// Headless benchmark: RTL shaping enabled vs disabled (i.e. stock LTR path).
//
// Measures the text-sizing and text-rendering cost for several text cases with the
// shaper attached (RTL mode) vs. detached (FontShaper = NULL, which is byte-for-byte the
// same LTR path used when IMGUI_ENABLE_RTL is not defined at compile time).
//
// Usage: rtl_benchmark [font.ttf] [font_size] [iterations]

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_rtl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>

// Default font location: baked in by the CMake build, so the binaries work when run from
// the build directory. Falls back to the sibling "fonts/" folder.
#ifndef RTL_DEMO_FONT_DIR
#define RTL_DEMO_FONT_DIR "../fonts"
#endif
#define RTL_DEMO_FONT_ARABIC RTL_DEMO_FONT_DIR "/NotoNaskhArabic.ttf"

typedef std::chrono::high_resolution_clock Clock;

struct BenchCase
{
    const char* name;
    const char* text;
    float       wrap_width; // 0 = single line
};

int main(int argc, char** argv)
{
    const char* font_path = (argc > 1) ? argv[1] : RTL_DEMO_FONT_ARABIC;
    const float font_size = (argc > 2) ? (float)atof(argv[2]) : 20.0f;
    const int iterations = (argc > 3) ? atoi(argv[3]) : 20000;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);

    ImFontConfig cfg;
    cfg.OversampleH = cfg.OversampleV = 1;
    ImFont* font = io.Fonts->AddFontFromFileTTF(font_path, font_size, &cfg);
    if (font == NULL)
    {
        fprintf(stderr, "Failed to load font: %s\n", font_path);
        return 1;
    }
    io.Fonts->Build();

    const ImFontShaper* shaper = ImGuiRTL::GetShaper();

    // --- Test cases (logical UTF-8; Arabic rendered disconnected/LTR when shaping is off) ---
    const char* en = "The quick brown fox jumps over the lazy dog. 0123456789";
    const char* ar = "\xD8\xA7\xD9\x84\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xD8\xB9\xD9\x84\xD9\x8A\xD9\x83\xD9\x85"; // "السلام عليكم"
    const char* ar_dia = "\xD8\xA8\xD9\x90\xD8\xB3\xD9\x92\xD9\x85\xD9\x90 \xD8\xA7\xD9\x84\xD9\x84\xD9\x91\xD9\x8E\xD9\x87\xD9\x90 \xD8\xA7\xD9\x84\xD8\xB1\xD9\x91\xD9\x8E\xD8\xAD\xD9\x92\xD9\x85\xD9\x8E\xD9\x86\xD9\x90"; // Bismillah with diacritics
    const char* mixed = "English \xD9\x86\xD8\xB5 \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A and 1234"; // "English نص عربي and 1234"
    const char* long_en = "This is a long English sentence that should wrap across multiple lines at the given width to exercise the word-wrap path.";
    const char* long_ar = "\xD9\x87\xD8\xB0\xD8\xA7 \xD9\x86\xD8\xB5 \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A \xD8\xB7\xD9\x88\xD9\x8A\xD9\x84 \xD8\xAC\xD8\xAF\xD9\x91\xD8\xA7 \xD9\x84\xD8\xA7\xD8\xAE\xD8\xAA\xD8\xA8\xD8\xA7\xD8\xB1 \xD8\xA7\xD9\x84\xD8\xAA\xD9\x81\xD8\xA7\xD9\x81 \xD8\xA7\xD9\x84\xD8\xA3\xD8\xB3\xD8\xB7\xD8\xB1"; // long Arabic

    const BenchCase cases[] =
    {
        { "English",            en,      0.0f   },
        { "Arabic",             ar,      0.0f   },
        { "Arabic+diacritics",  ar_dia,  0.0f   },
        { "Mixed",              mixed,   0.0f   },
        { "English (wrap)",     long_en, 220.0f },
        { "Arabic (wrap)",      long_ar, 220.0f },
    };
    const int case_count = (int)(sizeof(cases) / sizeof(cases[0]));

    // Shared render target.
    ImDrawList draw_list(ImGui::GetDrawListSharedData());
    const ImVec4 clip_rect(0.0f, 0.0f, 100000.0f, 100000.0f);
    const ImVec2 pos(0.0f, 0.0f);
    const ImU32 col = 0xFFFFFFFF;

    printf("=== RTL text-shaping benchmark ===\n");
    printf("font: %s   size: %.0fpx   iterations: %d\n\n", font_path, font_size, iterations);
    printf("  \"RTL\" = shaping ON  (libraqm: correct bidi + Arabic shaping)\n");
    printf("  \"LTR\" = shaping OFF (stock ImGui; Arabic shows disconnected glyphs)\n\n");
    printf("  measure = ImFont::CalcTextSizeA (layout / sizing)\n");
    printf("  render  = ImFont::RenderText    (drawing to a draw-list)\n");
    printf("  cache   = sizing the SAME text a 2nd time in one frame (shaper cache hit)\n\n");
    printf("  All values are microseconds (us) per call (lower is faster).\n\n");
    printf("%-22s | %12s | %12s | %10s | %10s | %10s | %10s\n",
           "case", "measure RTL", "measure LTR", "render RTL", "render LTR", "cache RTL", "cache LTR");
    printf("%s\n", "--------------------------------------------------------------------------------------------------------");

    volatile float sink = 0.0f;

    // Clear the shaping cache between measured iterations so each call is a cache MISS
    // (simulates fresh/independent text). Without this, repeated same-text calls would hit the
    // cache and report unrealistically low numbers.

    for (int ci = 0; ci < case_count; ci++)
    {
        const BenchCase& c = cases[ci];
        const char* text_end = c.text + strlen(c.text);
        double size_us[2] = { 0.0, 0.0 };
        double render_us[2] = { 0.0, 0.0 };
        double cache_hit_us[2] = { 0.0, 0.0 };

        for (int mode = 0; mode < 2; mode++)
        {
            const bool enabled = (mode == 0);
            io.Fonts->FontShaper = enabled ? shaper : NULL;

            // Warm-up (also preloads glyphs); clear the cache so it doesn't leave a populated cache.
            for (int i = 0; i < 200; i++)
            {
                ImGuiRTL::ClearShapeCache();
                ImVec2 s = font->CalcTextSizeA(font_size, FLT_MAX, c.wrap_width, c.text);
                sink += s.x;
                draw_list._ResetForNewFrame();
                font->RenderText(&draw_list, font_size, pos, col, clip_rect, c.text, text_end, c.wrap_width, 0);
            }
            draw_list._ResetForNewFrame();

            // Sizing (cache flushed each iteration).
            {
                const double t0 = (double)Clock::now().time_since_epoch().count();
                for (int i = 0; i < iterations; i++)
                {
                    ImGuiRTL::ClearShapeCache();
                    ImVec2 s = font->CalcTextSizeA(font_size, FLT_MAX, c.wrap_width, c.text);
                    sink += s.x;
                }
                const double t1 = (double)Clock::now().time_since_epoch().count();
                size_us[mode] = (t1 - t0) / (double)iterations * 1e-3; // ns -> us
            }

            // Rendering (cache flushed each iteration).
            {
                const double t0 = (double)Clock::now().time_since_epoch().count();
                for (int i = 0; i < iterations; i++)
                {
                    ImGuiRTL::ClearShapeCache();
                    draw_list._ResetForNewFrame();
                    font->RenderText(&draw_list, font_size, pos, col, clip_rect, c.text, text_end, c.wrap_width, 0);
                }
                const double t1 = (double)Clock::now().time_since_epoch().count();
                render_us[mode] = (t1 - t0) / (double)iterations * 1e-3;
            }

            // Cache hit: populate the cache once, then size the same text repeatedly. All
            // subsequent calls hit the cache (this isolates the shaper's cache-hit cost).
            {
                for (int i = 0; i < 100; i++) { ImGuiRTL::ClearShapeCache(); ImVec2 s = font->CalcTextSizeA(font_size, FLT_MAX, c.wrap_width, c.text); sink += s.x; }
                const double t0 = (double)Clock::now().time_since_epoch().count();
                for (int i = 0; i < iterations; i++)
                {
                    ImVec2 s = font->CalcTextSizeA(font_size, FLT_MAX, c.wrap_width, c.text);
                    sink += s.x;
                }
                const double t1 = (double)Clock::now().time_since_epoch().count();
                cache_hit_us[mode] = (t1 - t0) / (double)iterations * 1e-3;
            }
        }

        printf("%-22s | %12.2f | %12.2f | %10.2f | %10.2f | %10.2f | %10.2f\n",
               c.name,
               size_us[0], size_us[1], render_us[0], render_us[1],
               cache_hit_us[0], cache_hit_us[1]);
    }

    printf("\nHow to read it:\n");
    printf("  - For Latin/English text, RTL and LTR are ~equal: there is no shaping overhead\n");
    printf("    because pure-ASCII text is routed straight to the stock LTR path.\n");
    printf("  - For Arabic/mixed text, RTL 'measure'/'render' are slower than LTR because\n");
    printf("    libraqm must run HarfBuzz bidi+shaping on every (uncached) call. That is the\n");
    printf("    inherent cost of correct Arabic shaping, not an overhead bug.\n");
    printf("  - 'cache RTL' is the cost of a cache HIT (the 2nd+ call to the same text in one\n");
    printf("    frame). Compare it to 'measure RTL' to see how much the shaping cache saves\n");
    printf("    for repeated labels.\n");
    (void)sink;

    ImGui::DestroyContext();
    return 0;
}
