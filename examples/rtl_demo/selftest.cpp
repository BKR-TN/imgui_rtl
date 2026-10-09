// Headless self-test for the RTL text-shaping integration.
// No window/backend required: loads a font, exercises glyph-index lookup and text
// measurement, and dumps the shaped glyph sequence produced by the shaper.

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_rtl.h"

#include <stdio.h>
#include <string.h>

// Default font location: baked in by the CMake build, so the binaries work when run from
// the build directory. Falls back to the sibling "fonts/" folder.
#ifndef RTL_DEMO_FONT_DIR
#define RTL_DEMO_FONT_DIR "../fonts"
#endif
#define RTL_DEMO_FONT_ARABIC RTL_DEMO_FONT_DIR "/NotoNaskhArabic.ttf"
#define RTL_DEMO_FONT_LATIN  RTL_DEMO_FONT_DIR "/Roboto-Medium.ttf"

static void PrintUtf8Hex(const char* s)
{
    for (const unsigned char* p = (const unsigned char*)s; *p; p++)
        printf("%02X ", *p);
}

// --- Minimal test harness -------------------------------------------------------------------
static int GTestFailures = 0;
static int GTestCount = 0;
static void Check(bool ok, const char* name, const char* detail = NULL)
{
    GTestCount++;
    if (!ok)
        GTestFailures++;
    if (detail != NULL)
        printf("  [%s] %s (%s)\n", ok ? "OK" : "FAILED", name, detail);
    else
        printf("  [%s] %s\n", ok ? "OK" : "FAILED", name);
}

static void DumpShaped(const ImFontShaper* shaper, ImFont* font, ImFontBaked* baked, const char* label, const char* text)
{
    const char* end = text + strlen(text);
    const ImShapedGlyph* g = NULL;
    int n = 0, dir = 0;
    bool ok = shaper->ShapeText(font, baked, text, end, &g, &n, &dir);
    printf("  %-16s dir=%s ok=%d n=%d\n", label, dir ? "RTL" : "LTR", (int)ok, n);
    if (ok)
        for (int i = 0; i < n; i++)
            printf("      [%2d] gid=%6u  adv=%7.2f  xoff=%6.2f  yoff=%6.2f  cluster=%u\n",
                   i, g[i].GlyphId, g[i].XAdvance, g[i].XOffset, g[i].YOffset, g[i].Cluster);
}

int main(int argc, char** argv)
{
    const char* font_path = (argc > 1) ? argv[1] : RTL_DEMO_FONT_ARABIC;
    // Merged-font check: defaults to the bundled fonts so it runs in a plain `./rtl_selftest`.
    // Pass "-" to disable it, or override the two paths explicitly.
    const char* merge_base_path = (argc > 2) ? argv[2] : RTL_DEMO_FONT_LATIN;
    const char* merge_arabic_path = (argc > 3) ? argv[3] : RTL_DEMO_FONT_ARABIC;
    if (merge_base_path[0] == '-' && merge_base_path[1] == 0) merge_base_path = NULL;
    if (merge_arabic_path != NULL && merge_arabic_path[0] == '-' && merge_arabic_path[1] == 0) merge_arabic_path = NULL;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);

    ImFontConfig cfg;
    cfg.OversampleH = cfg.OversampleV = 1;
    ImFont* font = io.Fonts->AddFontFromFileTTF(font_path, 22.0f, &cfg);
    if (font == NULL)
    {
        fprintf(stderr, "Failed to load font: %s\n", font_path);
        return 1;
    }

    // Optional merged-font check: a Latin-only base with an Arabic source merged in, which
    // exercises the shaper's per-source face fallback (Arabic glyphs come from the merge).
    ImFont* merged_font = NULL;
    if (merge_base_path != NULL && merge_arabic_path != NULL)
    {
        static const ImWchar kArabicRanges[] = { 0x0600, 0x06FF, 0x0750, 0x077F, 0x08A0, 0x08FF, 0xFB50, 0xFDFF, 0xFE70, 0xFEFF, 0 };
        merged_font = io.Fonts->AddFontFromFileTTF(merge_base_path, 22.0f, &cfg, io.Fonts->GetGlyphRangesDefault());
        ImFontConfig merge_cfg;
        merge_cfg.OversampleH = merge_cfg.OversampleV = 1;
        merge_cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(merge_arabic_path, 22.0f, &merge_cfg, kArabicRanges);
    }

    // Force atlas build (preloads glyphs at the font's size and builds the texture).
    io.Fonts->Build();

    ImFontBaked* baked = font->GetFontBaked(22.0f);
    printf("Font loaded: %s (ascent %.1f, descent %.1f, size %.1f)\n",
           font->GetDebugName(), baked->Ascent, baked->Descent, baked->Size);

    // --- Glyph-index lookup sanity check ---
    printf("\nGlyph-index lookup (indices 1..8):\n");
    for (unsigned int i = 1; i <= 8; i++)
    {
        ImFontGlyph* g = baked->FindGlyphByIndex(i, 0);
        printf("  glyph %u: %s (advance=%.2f visible=%d)\n", i, g ? "found" : "missing",
               g ? g->AdvanceX : 0.0f, g ? (int)g->Visible : 0);
    }

    // --- Text measurement ---
    const char* ltr = "Hello, world!";
    const char* ar  = "\xD8\xA7\xD9\x84\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xD8\xB9\xD9\x84\xD9\x8A\xD9\x83\xD9\x85"; // "السلام عليكم"
    const char* mixed = "English \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A 1234";
    const char* mixed_nodigits = "English \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A"; // "English عربي" (no trailing digits).
    const char* rtl_ltr = "\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A English"; // "عربي English" (Arabic then English, RTL base).
    const char* en_ar_en = "English \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A English"; // "English عربي English" (RTL run between LTR runs).
    const char* ar_dia = "\xD8\xA7\xD9\x84\xD8\xB1\xD9\x91\xD9\x8E\xD8\xAD\xD9\x92\xD9\x85\xD9\x8E\xD9\x86\xD9\x8F"; // "الرَّحْمَنُ"

    printf("\nText measurement:\n");
    ImVec2 sz_ltr = font->CalcTextSizeA(22.0f, FLT_MAX, 0.0f, ltr);
    ImVec2 sz_ar  = font->CalcTextSizeA(22.0f, FLT_MAX, 0.0f, ar);
    ImVec2 sz_mix = font->CalcTextSizeA(22.0f, FLT_MAX, 0.0f, mixed);
    printf("  LTR   \"%s\"  -> %.2f x %.2f\n", ltr, sz_ltr.x, sz_ltr.y);
    printf("  AR    \""); PrintUtf8Hex(ar); printf("\" -> %.2f x %.2f\n", sz_ar.x, sz_ar.y);
    printf("  MIXED \"%s\" -> %.2f x %.2f\n", mixed, sz_mix.x, sz_mix.y);

    // Word wrap (shaped path should wrap RTL text into multiple lines).
    const char* long_ar = "\xD9\x87\xD8\xB0\xD8\xA7 \xD9\x86\xD8\xB5 \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A \xD8\xB7\xD9\x88\xD9\x8A\xD9\x84 \xD8\xAC\xD8\xAF\xD9\x91\xD8\xA7 \xD9\x84\xD8\xA7\xD8\xAE\xD8\xAA\xD8\xA8\xD8\xA7\xD8\xB1 \xD8\xA7\xD9\x84\xD8\xAA\xD9\x91\xD9\x81\xD8\xA7\xD9\x81 \xD8\xA7\xD9\x84\xD8\xA3\xD8\xB3\xD8\xB7\xD8\xB1";
    ImVec2 sz_wrap = font->CalcTextSizeA(22.0f, FLT_MAX, 120.0f, long_ar);
    printf("  WRAP  (120px) -> %.2f x %.2f  (~%d lines)\n", sz_wrap.x, sz_wrap.y, (int)(sz_wrap.y / 22.0f + 0.5f));

    const char* long_ltr = "This is a long English sentence that should wrap across multiple lines at the given width.";
    ImVec2 sz_wrap_ltr = font->CalcTextSizeA(22.0f, FLT_MAX, 120.0f, long_ltr);
    printf("  WRAP  LTR (120px) -> %.2f x %.2f  (~%d lines)\n", sz_wrap_ltr.x, sz_wrap_ltr.y, (int)(sz_wrap_ltr.y / 22.0f + 0.5f));

    // --- Shaping dump ---
    const ImFontShaper* shaper = io.Fonts->FontShaper;
    printf("\nShaper: %s\n", shaper ? shaper->Name : "(none)");
    Check(shaper != NULL, "a shaper is attached to the atlas (IMGUI_ENABLE_RTL build)");
    if (shaper == NULL)
    {
        printf("\nFATAL: no shaper attached; the rest of the suite would silently pass nothing.\n");
        printf("=== Regression tests: %d/%d passed ===\n", GTestCount - GTestFailures, GTestCount);
        return 1;
    }
    if (shaper != NULL)
    {
        DumpShaped(shaper, font, baked, "AR", ar);
        DumpShaped(shaper, font, baked, "AR-diacritics", ar_dia);
        DumpShaped(shaper, font, baked, "MIXED", mixed);
        DumpShaped(shaper, font, baked, "RTL-LTR", rtl_ltr);
        DumpShaped(shaper, font, baked, "EN-AR-EN", en_ar_en);
        DumpShaped(shaper, font, baked, "LTR-ascii", ltr);

        // Bidi caret-walk reversibility: stepping right through a mixed line must be the exact
        // reverse of stepping left. This catches both the LTR->RTL "skip the first Arabic glyph"
        // bug and its symmetric RTL->LTR counterpart.
        if (shaper->MoveCaretVisual != NULL)
        {
            printf("\nCaret walk reversibility (RTL<->LTR):\n");
            const char* rev_cases[] = { rtl_ltr, en_ar_en };
            for (int rc = 0; rc < 2; rc++)
            {
                const char* t = rev_cases[rc];
                const char* te = t + strlen(t);
                const ImShapedGlyph* g = NULL; int n = 0, d = 0;
                shaper->ShapeText(font, baked, t, te, &g, &n, &d);

                int seq[64]; int seq_n = 0;
                const int visual_start = (d == 1) ? (int)strlen(t) : 0;
                const int visual_end   = (d == 1) ? 0 : (int)strlen(t);
                int cur = visual_start, aff = -1;
                seq[seq_n++] = cur;
                for (int s = 0; s < 63; s++)
                {
                    int oaff = -1;
                    const int nxt = shaper->MoveCaretVisual(font, baked, t, te, cur, +1, aff, &oaff);
                    aff = oaff;
                    if (nxt == cur) break;
                    seq[seq_n++] = nxt;
                    cur = nxt;
                }

                // Walk left from the visual end and compare against the reverse of seq[].
                cur = visual_end; aff = -1;
                int rev_ok = 1, rev_idx = seq_n - 1;
                if (seq[rev_idx] != cur) rev_ok = 0;
                for (int s = 0; s < 63 && rev_ok; s++)
                {
                    int oaff = -1;
                    const int nxt = shaper->MoveCaretVisual(font, baked, t, te, cur, -1, aff, &oaff);
                    aff = oaff;
                    if (nxt == cur) break;
                    cur = nxt;
                    rev_idx--;
                    if (rev_idx < 0 || seq[rev_idx] != cur) rev_ok = 0;
                }
                printf("  %-22s -> %s\n", rc == 0 ? "عربي English" : "English عربي English", rev_ok ? "OK" : "FAILED");
                Check(rev_ok != 0, rc == 0 ? "visual caret walk is reversible (Arabic then English)"
                                          : "visual caret walk is reversible (English, Arabic, English)");
            }
        }
    }

    // Merged-font shaping: the Arabic glyphs must come from the merged source (not .notdef).
    // Guarded on the shaper too: this block dereferences shaper->ShapeText / FindGlyphByIndex.
    if (merged_font != NULL && shaper != NULL)
    {
        ImFontBaked* merged_baked = merged_font->GetFontBaked(22.0f);
        printf("\nMerged-font shaping (Latin base + Arabic merge):\n");
        DumpShaped(shaper, merged_font, merged_baked, "AR", ar);
        DumpShaped(shaper, merged_font, merged_baked, "AR-diacritics", ar_dia);
        DumpShaped(shaper, merged_font, merged_baked, "MIXED", mixed);

        // Verify the source-aware glyph lookup resolves every shaped glyph to the right source
        // (glyph indices collide across merged sources, so source_idx must be part of the key).
        const ImShapedGlyph* g = NULL; int n = 0, d = 0;
        if (shaper->ShapeText(merged_font, merged_baked, ar, ar + strlen(ar), &g, &n, &d))
        {
            int mismatches = 0;
            for (int i = 0; i < n; i++)
            {
                ImFontGlyph* bg = merged_baked->FindGlyphByIndex(g[i].GlyphId, g[i].SourceIdx);
                if (bg == NULL || bg->GlyphId != g[i].GlyphId || bg->SourceIdx != g[i].SourceIdx)
                    mismatches++;
            }
            Check(mismatches == 0 && n > 0, "merged-font glyphs resolve to their own source (glyph-index + source key)",
                  mismatches == 0 ? NULL : "some shaped glyphs resolved to the wrong source");
        }
    }

    // --- Cursor mapping round-trip (IndexToXOffset / XOffsetToIndex) ---
    if (shaper != NULL && shaper->IndexToXOffset != NULL && shaper->XOffsetToIndex != NULL)
    {
        const struct { const char* label; const char* text; } map_cases[] = {
            { "AR", ar }, { "AR+marks", ar_dia }, { "MIXED", mixed },
        };
        bool map_roundtrip_ok = true;
        for (int mc = 0; mc < (int)(sizeof(map_cases) / sizeof(map_cases[0])); mc++)
        {
            const char* map_text = map_cases[mc].text;
            const char* map_end = map_text + strlen(map_text);
            printf("\nCursor mapping (%s):\n", map_cases[mc].label);
            for (const char* p = map_text; ; )
            {
                const int i = (int)(p - map_text);
                const float x = shaper->IndexToXOffset(font, baked, map_text, map_end, i, -1);
                const int idx = shaper->XOffsetToIndex(font, baked, map_text, map_end, x);
                const char* tag = "OK";
                if (idx != i)
                {
                    // A non-identical round-trip is fine only if it lands on another caret
                    // stop at the *same* x: a combining mark snapping to its base, or a bidi
                    // run boundary where two logical positions share one visual x.
                    const float x2 = shaper->IndexToXOffset(font, baked, map_text, map_end, idx, -1);
                    tag = (x2 >= 0.0f && x2 - x > -0.001f && x2 - x < 0.001f) ? "same-x" : "WRONG";
                }
                printf("  byte %2d -> x=%6.2f -> byte %2d %s\n", i, x, idx, tag);
                if (strcmp(tag, "WRONG") == 0)
                    map_roundtrip_ok = false;
                if (p >= map_end)
                    break;
                unsigned int c = (unsigned char)*p;
                p += (c < 0x80) ? 1 : ImTextCharFromUtf8(&c, p, map_end);
            }
        }
        Check(map_roundtrip_ok, "caret Index<->X round-trip lands on the same visual x");
    }

    // --- Shaping cache correctness ---
    if (shaper != NULL)
    {
        // Shaping the same text twice must yield identical glyphs (2nd call is a cache hit).
        const ImShapedGlyph* g1 = NULL, * g2 = NULL;
        int n1 = 0, n2 = 0, d1 = 0, d2 = 0;
        const bool ok1 = shaper->ShapeText(font, baked, ar, ar + strlen(ar), &g1, &n1, &d1);
        const bool ok2 = shaper->ShapeText(font, baked, ar, ar + strlen(ar), &g2, &n2, &d2); // cache hit
        bool identical = (ok1 == ok2 && n1 == n2 && d1 == d2);
        if (identical)
            for (int i = 0; i < n1; i++)
                if (g1[i].GlyphId != g2[i].GlyphId || g1[i].XAdvance != g2[i].XAdvance || g1[i].XOffset != g2[i].XOffset || g1[i].YOffset != g2[i].YOffset || g1[i].Cluster != g2[i].Cluster)
                {
                    identical = false;
                    break;
                }
        Check(identical && n1 > 0, "shaping cache hit returns identical glyphs to the miss");

        // Direction keying: changing the base direction must not return a stale cached result.
        ImGuiRTL::SetDirection(ImGuiRTL::Direction_RTL);
        shaper->ShapeText(font, baked, mixed, mixed + strlen(mixed), &g1, &n1, &d1);
        ImGuiRTL::SetDirection(ImGuiRTL::Direction_LTR);
        shaper->ShapeText(font, baked, mixed, mixed + strlen(mixed), &g2, &n2, &d2);
        ImGuiRTL::SetDirection(ImGuiRTL::Direction_Default);
        Check(d1 == 1 && d2 == 0, "shaping cache is keyed on base direction (forced RTL -> 1, forced LTR -> 0)");

        // Per-run direction (used for arrow-key movement inside mixed bidi text).
        if (shaper->DirectionAt != NULL)
        {
            printf("\nDirection at index (mixed 'English عربي 1234'):\n");
            const int offs[] = { 0, 4, 8, 10, 14, 17, 20 };
            for (int oi = 0; oi < (int)(sizeof(offs) / sizeof(offs[0])); oi++)
            {
                const int o = offs[oi];
                const int dir = shaper->DirectionAt(font, baked, mixed, mixed + strlen(mixed), o);
                printf("  byte %2d -> %s\n", o, dir == 1 ? "RTL" : dir == 0 ? "LTR" : "unknown");
            }
        }

        // Visual caret traversal (MoveCaretVisual): stepping right must be a monotonic visual
        // walk with no oscillation at bidi run boundaries.
        if (shaper->MoveCaretVisual != NULL)
        {
            printf("\nVisual caret traversal (mixed, from start to end):\n");
            const char* me = mixed + strlen(mixed);
            int cur = 0;
            int aff = -1;
            for (int step = 0; step < 40; step++)
            {
                int out_aff = -1;
                const int nxt = shaper->MoveCaretVisual(font, baked, mixed, me, cur, +1, aff, &out_aff);
                aff = out_aff;
                if (nxt == cur)
                {
                    printf("  (end at byte %d)\n", cur);
                    break;
                }
                printf("  byte %2d -> %2d\n", cur, nxt);
                cur = nxt;
            }

            // Regression: "English عربي" (no trailing digits) — stepping right from the English
            // space must land "after the English" (byte 8), NOT skip the first Arabic glyph ي.
            printf("\nVisual caret traversal (English عربي, no skip at run boundary):\n");
            {
                const char* me2 = mixed_nodigits + strlen(mixed_nodigits);
                int oaff = -1;
                const int after_space = shaper->MoveCaretVisual(font, baked, mixed_nodigits, me2, 7, +1, 0, &oaff);
                Check(after_space == 8, "visual caret step does not skip the first Arabic glyph at a run boundary");
            }

            // Pure RTL: from the end (leftmost) stepping right must reach the start without dead-lock.
            printf("\nVisual caret traversal (AR, from end toward start):\n");
            const char* ae = ar + strlen(ar);
            cur = (int)strlen(ar); // end = leftmost for RTL
            aff = -1;
            for (int step = 0; step < 40; step++)
            {
                int out_aff = -1;
                const int nxt = shaper->MoveCaretVisual(font, baked, ar, ae, cur, +1, aff, &out_aff);
                aff = out_aff;
                if (nxt == cur)
                {
                    printf("  (end at byte %d)\n", cur);
                    break;
                }
                printf("  byte %2d -> %2d\n", cur, nxt);
                cur = nxt;
            }
        }

        // Sanity: the visual caret walk must be x-monotonic (left-to-right), and each step's
        // x must agree with IndexToXOffset at that byte — the whole point of the unified model.
        if (shaper->MoveCaretVisual != NULL && shaper->IndexToXOffset != NULL)
        {
            const char* me = mixed + strlen(mixed);
            int cur = 0;
            int aff = -1;
            float prev_x = -FLT_MAX;
            bool monotonic = true;
            for (int step = 0; step < 64; step++)
            {
                const float x = shaper->IndexToXOffset(font, baked, mixed, me, cur, aff);
                if (x < prev_x - 0.001f) { monotonic = false; break; }
                prev_x = x;
                int out_aff = -1;
                const int nxt = shaper->MoveCaretVisual(font, baked, mixed, me, cur, +1, aff, &out_aff);
                aff = out_aff;
                if (nxt == cur) break;
                cur = nxt;
            }
            Check(monotonic, "visual caret walk is x-monotonic and agrees with IndexToXOffset");
        }
    }

    // --- Right-alignment helper ---
    {
        const float ar_w = font->CalcTextSizeA(22.0f, FLT_MAX, 0.0f, ar).x;
        const float x_ar = ImGuiRTL::AlignTextRight(0.0f, 200.0f, ar, ar + strlen(ar), ar_w);
        const float x_ltr = ImGuiRTL::AlignTextRight(0.0f, 200.0f, ltr, ltr + strlen(ltr), 100.0f);
        Check(ImFabs(x_ar - (200.0f - ar_w)) < 0.01f && x_ltr == 0.0f,
              "AlignTextRight() right-aligns RTL text and leaves LTR text at pos_x");
    }

    // --- Selection segments (bidi-correct highlight) ---
    if (shaper != NULL && shaper->GetSelectionSegments != NULL)
    {
        printf("\nSelection segments (mixed text 'English عربي 1234'):\n");
        const struct { const char* name; const char* t; int b; int e; } sel[] = {
            { "AR whole",        ar,      0,  (int)strlen(ar) },
            { "AR partial",      ar,      4,  12 },
            { "MIXED arabic",    mixed,   8,  16 },
            { "MIXED ar+digits", mixed,   8,  (int)strlen(mixed) },
        };
        for (int si = 0; si < (int)(sizeof(sel) / sizeof(sel[0])); si++)
        {
            float segs[64];
            const int n = shaper->GetSelectionSegments(font, baked, sel[si].t, sel[si].t + strlen(sel[si].t), sel[si].b, sel[si].e, segs, 32);
            printf("  %-18s [%d,%d) -> %d segment(s):", sel[si].name, sel[si].b, sel[si].e, n);
            for (int i = 0; i < n; i++)
                printf(" [%.2f,%.2f)", segs[i * 2 + 0], segs[i * 2 + 1]);
            printf("\n");
        }
    }

    // =========================================================================================
    // Regression tests for the issues found in review.
    // =========================================================================================
    printf("\n=== Regression tests ===\n");

    // (1) Word-wrap must never split a UTF-8 character and must always make progress.
    if (shaper != NULL)
    {
        const char* wt = long_ar;
        const char* wt_end = wt + strlen(wt);
        bool wrap_ok = true;
        int wrap_lines_at_120 = 0;
        for (float wr = 1.0f; wr <= 240.0f; wr += 1.0f)
        {
            ImVector<const char*> starts;
            ImFontShapedWrapLine(font, baked, shaper, 22.0f, wt, wt_end, wr, &starts, ImDrawTextFlags_WrapKeepBlanks);
            if (starts.Size < 1 || starts[0] != wt)
                wrap_ok = false;
            const char* prev = NULL;
            for (int i = 0; i < starts.Size && wrap_ok; i++)
            {
                const char* st = starts[i];
                if (st < wt || st > wt_end) wrap_ok = false;                  // in range
                if (((unsigned char)*st & 0xC0) == 0x80) wrap_ok = false;     // not a continuation byte
                if (prev != NULL && st <= prev) wrap_ok = false;              // strictly increasing
                prev = st;
            }
            if (wr == 120.0f)
                wrap_lines_at_120 = starts.Size;
        }
        Check(wrap_ok, "word-wrap keeps UTF-8 boundaries and always makes progress (widths 1..240)");
        Check(wrap_lines_at_120 > 1, "word-wrap still wraps long RTL text at 120px");

        // ASCII (shaper declines): the shaped wrap must produce exactly the same line starts as the
        // stock index formula, otherwise the InputText line index disagrees with the LTR renderer.
        const char* ascii = "The quick brown fox jumps over the lazy dog";
        const char* ascii_end = ascii + strlen(ascii);
        bool ascii_index_ok = true;
        for (float wr = 4.0f; wr <= 200.0f; wr += 4.0f)
        {
            ImVector<const char*> shaped_starts;
            ImFontShapedWrapLine(font, baked, shaper, 22.0f, ascii, ascii_end, wr, &shaped_starts, ImDrawTextFlags_WrapKeepBlanks);
            // Stock formula used by InputTextLineIndexBuild()/the LTR renderer.
            ImVector<const char*> stock_starts;
            const char* q = ascii;
            for (;;)
            {
                stock_starts.push_back(q);
                q = ImFontCalcWordWrapPositionEx(font, 22.0f, q, ascii_end, wr, ImDrawTextFlags_WrapKeepBlanks);
                if (q >= ascii_end)
                    break;
            }
            if (shaped_starts.Size != stock_starts.Size)
            {
                ascii_index_ok = false;
                break;
            }
            for (int i = 0; i < shaped_starts.Size; i++)
                if (shaped_starts[i] != stock_starts[i])
                {
                    ascii_index_ok = false;
                    break;
                }
        }
        Check(ascii_index_ok, "ASCII wrap matches the stock line index (no shaper/renderer mismatch)",
              ascii_index_ok ? NULL : "mismatch; see the widths swept in the loop above");

        // A wrap point whose remainder is only blanks must not create a phantom empty line.
        {
            bool no_phantom = true;
            const char* cases[] = { "AB ", "AB  ", "AB \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A", "\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A  " };
            for (int ci = 0; ci < 4 && no_phantom; ci++)
            {
                const char* t = cases[ci];
                const char* t_end = t + strlen(t);
                const float one_word = font->CalcTextSizeA(22.0f, FLT_MAX, 0.0f, t).x;
                ImVector<const char*> st;
                ImFontShapedWrapLine(font, baked, shaper, 22.0f, t, t_end, one_word + 0.5f, &st, ImDrawTextFlags_WrapKeepBlanks);
                for (int i = 0; i < st.Size; i++)
                    if (st[i] >= t_end)
                        no_phantom = false;
            }
            Check(no_phantom, "wrap never emits a line start at (or past) the end of the text");
        }

        // '\r' must not consume width in the shaped path (the codepoint path skips it).
        {
            // '\r' must cost no width AND must not break shaping (libraqm rejects bidi class-B
            // characters: the shaper strips them and remaps the clusters).
            const char* cr_cases[] = { "\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\r", "\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\r\r",
                                       "\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\r\n", "\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xE2\x80\xA9",
                                       "\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xC2\x85" };
            bool cr_ok = true;
            for (int ci = 0; ci < 5 && cr_ok; ci++)
            {
                const char* t = cr_cases[ci];
                const int n = (int)strlen(t);
                float with_break = 0.0f, without_break = 0.0f;
                int d = 0;
                const bool shaped_with = ImFontShapedCalcLineMetrics(font, baked, 22.0f, t, t + n, &with_break, &d);
                ImFontShapedCalcLineMetrics(font, baked, 22.0f, t, t + 8, &without_break, &d); // "عربي"
                // The trailing separator must be stripped for shaping (so it costs nothing) and the
                // remainder must still be shaped (not falling back to the LTR path).
                if (!shaped_with || ImFabs(with_break - without_break) > 0.001f)
                    cr_ok = false;
            }
            Check(cr_ok, "bidi class-B characters (CR/NEL/U+2029) don't break shaping or add width");
        }
    }

    // (2) Glyph-index lookup robustness: out-of-range indices must return NULL (no huge allocation).
    // Note the index guard is evaluated before the source guard, so exercising the *source* guard
    // requires a glyph index that is itself valid (hence the 1u below).
    Check(baked->FindGlyphByIndex(0u, 0) == NULL, "FindGlyphByIndex(0) returns NULL");
    Check(baked->FindGlyphByIndex(0x10000u, 0) == NULL, "FindGlyphByIndex(> 0xFFFF) returns NULL");
    Check(baked->FindGlyphByIndex(1u, 99) == NULL, "FindGlyphByIndex(bad source) returns NULL");

    // (3) A transient load failure (ImFontFlags_NoLoadGlyphs, as set by PushPasswordFont()) must not
    //     be cached as NOT_FOUND: the glyph must load once loading is allowed again.
    if (shaper != NULL)
    {
        // Find a glyph index that is used by the text but not loaded in the atlas yet.
        unsigned int unloaded_gid = 0;
        const char* rare = "\xD9\x83\xD8\xB4\xD9\x85\xD8\xB4"; // "كشمش"
        const ImShapedGlyph* rg = NULL; int rn = 0, rd = 0;
        if (shaper->ShapeText(font, baked, rare, rare + strlen(rare), &rg, &rn, &rd))
            for (int i = 0; i < rn; i++)
            {
                // Mirrors the core's ImFontBakedGlyphIdLookupKey() packing (glyph_id << 4 | source);
                // IM_FONTGLYPH_ID_INDEX_UNUSED is 0. If that packing ever changes, this probe stops
                // finding a candidate -- which is why the else branch below fails loudly instead of skipping.
                const unsigned int key = (rg[i].GlyphId << 4) | (unsigned int)(rg[i].SourceIdx & 0xF);
                if (key < (unsigned int)baked->GlyphIdLookup.Size && baked->GlyphIdLookup[key] == 0 && rg[i].GlyphId != 0)
                {
                    unloaded_gid = rg[i].GlyphId;
                    break;
                }
            }
        if (unloaded_gid != 0)
        {
            font->Flags |= ImFontFlags_NoLoadGlyphs;
            ImFontGlyph* g_blocked = baked->FindGlyphByIndex(unloaded_gid, 0);
            font->Flags &= ~ImFontFlags_NoLoadGlyphs;
            ImFontGlyph* g_retry = baked->FindGlyphByIndex(unloaded_gid, 0);
            Check(g_blocked == NULL && g_retry != NULL, "transient load failure is not cached as NOT_FOUND");
        }
        else
        {
            Check(false, "transient load failure is not cached as NOT_FOUND",
                  "no unloaded glyph index found - the glyph-id lookup key packing may have changed");
        }
    }

    // (4) Password masking: the shaped path must substitute the fallback glyph like the codepoint
    //     path, and must not poison the glyph cache while doing so.
    if (shaper != NULL)
    {
        ImGuiContext& g = *GImGui;
        ImDrawList draw_list(ImGui::GetDrawListSharedData());
        g.Font = font;
        g.FontBaked = baked;
        const char* pt = ar;
        const char* pt_end = pt + strlen(pt);
        const ImVec4 clip(0.0f, 0.0f, 10000.0f, 10000.0f);
        int masked_distinct = 0;
        {
            ImGui::PushPasswordFont();
            draw_list._ResetForNewFrame(); // Required before using a standalone ImDrawList.
            const int v0 = draw_list.VtxBuffer.Size;
            font->RenderText(&draw_list, 22.0f, ImVec2(0.0f, 0.0f), IM_COL32_WHITE, clip, pt, pt_end, 0.0f, 0);
            const int v1 = draw_list.VtxBuffer.Size;
            for (int i = v0; i < v1; i += 4) // one ImFontGlyph quad = 4 vertices; compare top-left UVs
            {
                bool seen = false;
                for (int j = v0; j < i; j += 4)
                    if (draw_list.VtxBuffer[j].uv.x == draw_list.VtxBuffer[i].uv.x && draw_list.VtxBuffer[j].uv.y == draw_list.VtxBuffer[i].uv.y) { seen = true; break; }
                if (!seen)
                    masked_distinct++;
            }
            ImGui::PopPasswordFont();
            char detail[128];
            snprintf(detail, sizeof(detail), "fallback_idx=%d verts=%d distinct_uv=%d", baked->FallbackGlyphIndex, v1 - v0, masked_distinct);
            Check(v1 > v0 && masked_distinct == 1, "password masking applies to the shaped path (single fallback glyph)", detail);
        }
        {
            draw_list._ResetForNewFrame();
            const int v0 = draw_list.VtxBuffer.Size;
            font->RenderText(&draw_list, 22.0f, ImVec2(0.0f, 0.0f), IM_COL32_WHITE, clip, pt, pt_end, 0.0f, 0);
            const int v1 = draw_list.VtxBuffer.Size;
            int real_distinct = 0;
            for (int i = v0; i < v1; i += 4)
            {
                bool seen = false;
                for (int j = v0; j < i; j += 4)
                    if (draw_list.VtxBuffer[j].uv.x == draw_list.VtxBuffer[i].uv.x && draw_list.VtxBuffer[j].uv.y == draw_list.VtxBuffer[i].uv.y) { seen = true; break; }
                if (!seen)
                    real_distinct++;
            }
            Check(v1 > v0 && real_distinct > 1, "real glyphs still render after the password font is popped (no poisoned cache)");
        }
    }

    // (5) TextNeedsShaping: simple LTR scripts must bypass the shaping backend entirely.
    if (shaper != NULL && shaper->TextNeedsShaping != NULL)
    {
        const char* cafe = "caf\xC3\xA9";
        const char* cafe_end = cafe + strlen(cafe);
        const ImShapedGlyph* sg = NULL; int sn = 0, sd = 0;
        Check(!shaper->TextNeedsShaping(cafe, cafe_end), "accented Latin does not need shaping");
        Check(!shaper->TextNeedsShaping("Hello", "Hello" + 5), "ASCII does not need shaping");
        Check(shaper->TextNeedsShaping(ar, ar + strlen(ar)), "Arabic needs shaping");
        Check(shaper->TextNeedsShaping(mixed, mixed + strlen(mixed)), "mixed bidi text needs shaping");
        Check(!shaper->ShapeText(font, baked, cafe, cafe_end, &sg, &sn, &sd), "ShapeText() declines accented Latin (LTR fast path)");
        ImGuiRTL::SetSimpleScriptFastPath(false);
        Check(shaper->TextNeedsShaping(cafe, cafe_end), "SetSimpleScriptFastPath(false) routes simple text to the shaper");
        ImGuiRTL::SetSimpleScriptFastPath(true);
        Check(!shaper->TextNeedsShaping(cafe, cafe_end), "SetSimpleScriptFastPath(true) restores the fast path");
    }

    // (6) Control characters in shaped text use the codepoint path's advances (tab glyph width).
    if (shaper != NULL)
    {
        const char* ar_short = "\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A";      // "عربي"
        const char* ar_tab = "\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\t";    // "عربي\t"
        float w_plain = 0.0f, w_tab = 0.0f;
        int dir_plain = 0, dir_tab = 0;
        const bool ok_plain = ImFontShapedCalcLineMetrics(font, baked, 22.0f, ar_short, ar_short + strlen(ar_short), &w_plain, &dir_plain);
        const bool ok_tab = ImFontShapedCalcLineMetrics(font, baked, 22.0f, ar_tab, ar_tab + strlen(ar_tab), &w_tab, &dir_tab);
        const float tab_advance = baked->GetCharAdvance((ImWchar)'\t') * (22.0f / baked->Size);
        char detail[160];
        snprintf(detail, sizeof(detail), "w_plain=%.3f w_tab=%.3f delta=%.3f tab_advance=%.3f", w_plain, w_tab, w_tab - w_plain, tab_advance);
        Check(ok_plain && ok_tab && ImFabs((w_tab - w_plain) - tab_advance) < 0.01f, "tab in shaped text uses the tab glyph advance", detail);
    }

    // (6b) Malformed UTF-8 must not produce out-of-range caret/selection positions.
    if (shaper != NULL)
    {
        const char* bad_cases[] = { "\x80\x80", "\xD8", "\xD8\xB9\xD8", "a\x80" };
        bool in_range = true;
        for (int bc = 0; bc < 4 && in_range; bc++)
        {
            const char* bt = bad_cases[bc];
            const char* bt_end = bt + strlen(bt);
            const int bt_len = (int)(bt_end - bt);
            int cur = 0, aff = -1;
            for (int s = 0; s < 64; s++)
            {
                int oa = -1;
                const int nxt = shaper->MoveCaretVisual(font, baked, bt, bt_end, cur, +1, aff, &oa);
                aff = oa;
                if (nxt < 0) break;       // shaper declined
                if (nxt == cur) break;    // visual edge
                if (nxt < 0 || nxt > bt_len) { in_range = false; break; }
                cur = nxt;
            }
            for (int off = 0; off <= bt_len; off++)
            {
                const float x = shaper->IndexToXOffset(font, baked, bt, bt_end, off, -1);
                if (x < 0.0f)
                    continue;
                const int idx = shaper->XOffsetToIndex(font, baked, bt, bt_end, x);
                if (idx < 0 || idx > bt_len) { in_range = false; break; }
            }
        }
        Check(in_range, "malformed UTF-8 keeps caret/selection positions in range");
    }

    // (7) SetFontShaper(NULL) must survive a later atlas init (no compile-time default override).
    if (shaper != NULL)
    {
        io.Fonts->SetFontShaper(NULL);
        ImFontAtlasBuildInit(io.Fonts);
        Check(io.Fonts->FontShaper == NULL, "SetFontShaper(NULL) survives an atlas init");
        io.Fonts->SetFontShaper(ImGuiRTL::GetShaper());
        Check(io.Fonts->FontShaper != NULL, "SetFontShaper(shaper) restores shaping");
    }

    // (9) Edge-case sweep: malformed/unusual input must never crash or produce out-of-range
    // positions, at several sizes and wrap widths.
    if (shaper != NULL)
    {
        const char* cases[] = {
            "", "\n", "a", "\xD8\xB9", "\xD8", "\x80\x80", "\r\n\r\n",
            "\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\t\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A",
            "caf\xC3\xA9 e\xCC\x81", "\xE4\xBD\xA0\xE5\xA5\xBD", "\xF0\x9F\x98\x80",
        };
        const int case_count = (int)(sizeof(cases) / sizeof(cases[0]));
        const float sweep_sizes[] = { 8.0f, 22.0f, 96.0f };
        // 0.0f is meaningful: it selects the "single visual line / no wrapping" branch of
        // ImFontShapedWrapLine(). Do not substitute a fallback width here.
        const float sweep_wraps[] = { 0.0f, 1.0f, 30.0f, 120.0f };
        bool sweep_ok = true;
        int sweep_ops = 0;
        for (int ci = 0; ci < case_count && sweep_ok; ci++)
        {
            const char* t = cases[ci];
            const char* t_end = t + strlen(t);
            const int t_len = (int)(t_end - t);
            for (int si = 0; si < 3 && sweep_ok; si++)
            {
                ImFontBaked* b = font->GetFontBaked(sweep_sizes[si]);
                for (int wi = 0; wi < 4 && sweep_ok; wi++)
                {
                    const float wrap = sweep_wraps[wi];
                    ImVector<const char*> st;
                    ImFontShapedWrapLine(font, b, shaper, sweep_sizes[si], t, t_end, wrap, &st, ImDrawTextFlags_WrapKeepBlanks);
                    for (int k = 0; k < st.Size; k++)
                        if (st[k] < t || st[k] > t_end || (k > 0 && st[k] <= st[k - 1]))
                            sweep_ok = false;
                    float w = 0.0f;
                    int dir = 0;
                    ImFontShapedCalcLineMetrics(font, b, sweep_sizes[si], t, t_end, &w, &dir);
                    for (int off = 0; off <= t_len && sweep_ok; off++)
                    {
                        const float x = shaper->IndexToXOffset(font, b, t, t_end, off, -1);
                        if (x < 0.0f)
                            continue;
                        const int idx = shaper->XOffsetToIndex(font, b, t, t_end, x);
                        if (idx < 0 || idx > t_len)
                            sweep_ok = false;
                    }
                    int cur = 0, aff = -1;
                    for (int step = 0; step < 64 && sweep_ok; step++)
                    {
                        int out_aff = -1;
                        const int nxt = shaper->MoveCaretVisual(font, b, t, t_end, cur, +1, aff, &out_aff);
                        aff = out_aff;
                        if (nxt < 0 || nxt == cur)
                            break;
                        if (nxt > t_len)
                            sweep_ok = false;
                        cur = nxt;
                    }
                    sweep_ops++;
                }
            }
        }
        char sweep_detail[64];
        snprintf(sweep_detail, sizeof(sweep_detail), "%d combinations", sweep_ops);
        Check(sweep_ok, "edge-case sweep keeps every position in range", sweep_detail);
    }

    // (7b) The core must notify the shaper when a font is removed (not only on context teardown).
    if (shaper != NULL)
    {
        static int test_destroyed_count = 0;
        struct Local
        {
            static bool ShapeText(ImFont* f, ImFontBaked* b, const char* tb, const char* te, const ImShapedGlyph** og, int* ogc, int* obd)
            {
                return ImGuiRTL::GetShaper()->ShapeText(f, b, tb, te, og, ogc, obd);
            }
            static void FontDestroyed(ImFont*) { test_destroyed_count++; }
        };
        ImFontShaper counting_shaper;
        counting_shaper.Name = "counting";
        counting_shaper.ShapeText = Local::ShapeText;
        counting_shaper.FontDestroyed = Local::FontDestroyed;
        ImFont* extra = io.Fonts->AddFontFromFileTTF(font_path, 21.0f);
        io.Fonts->SetFontShaper(&counting_shaper);
        io.Fonts->RemoveFont(extra);
        Check(test_destroyed_count == 1, "FontDestroyed() is called by ImFontAtlas::RemoveFont()");
        io.Fonts->SetFontShaper(ImGuiRTL::GetShaper());
    }

    // (7c) Password masking must also apply to the caret/selection geometry the shaper reports.
    if (shaper != NULL && shaper->IndexToXOffset != NULL)
    {
        ImGuiContext& g = *GImGui;
        g.Font = font;
        g.FontBaked = baked;
        // In masked mode every character is drawn with the fallback glyph/advance, so the line
        // width must be (glyph count * fallback advance) and consecutive caret stops exactly one
        // fallback advance apart (in RTL, x decreases as the logical offset increases).
        const ImShapedGlyph* mg = NULL; int mn = 0, md = 0;
        shaper->ShapeText(font, baked, ar, ar + strlen(ar), &mg, &mn, &md);
        float w_unmasked = 0.0f, w_masked = 0.0f; int wdir = 0;
        ImFontShapedCalcLineMetrics(font, baked, 22.0f, ar, ar + strlen(ar), &w_unmasked, &wdir);
        g.Font = font;
        g.FontBaked = baked;
        ImGui::PushPasswordFont();
        ImFontShapedCalcLineMetrics(font, baked, 22.0f, ar, ar + strlen(ar), &w_masked, &wdir);
        const float x2 = shaper->IndexToXOffset(font, baked, ar, ar + strlen(ar), 2, -1);
        const float x4 = shaper->IndexToXOffset(font, baked, ar, ar + strlen(ar), 4, -1);
        ImGui::PopPasswordFont();
        const float fallback_adv = baked->FallbackAdvanceX;
        const bool pw_geom_ok = mn > 0
            && ImFabs(w_masked - mn * fallback_adv) < 0.01f
            && ImFabs(ImFabs(x4 - x2) - fallback_adv) < 0.01f
            && ImFabs(w_unmasked - w_masked) > 0.01f; // masked geometry genuinely differs
        char pw_detail[192];
        snprintf(pw_detail, sizeof(pw_detail), "width %.2f -> %.2f (glyphs %d x fallback %.2f), |x(4)-x(2)|=%.2f",
                 w_unmasked, w_masked, mn, fallback_adv, ImFabs(x4 - x2));
        Check(pw_geom_ok, "password masking applies to shaper caret/selection geometry", pw_detail);
    }

    // (8) FT_Face cache lifetime: removing a font and adding another one at the same ImFont address
    //     must not reuse the old (freed) face. Regression for a use-after-free crash: we compare
    //     against a fresh context that only ever saw the second font.
    {
        const char* latin_only = (argc > 2) ? argv[2] : RTL_DEMO_FONT_LATIN;
        FILE* latin_file = fopen(latin_only, "rb");
        if (latin_file == NULL)
        {
            printf("  [SKIP] FT_Face cache lifetime (second font not found: %s)\n", latin_only);
        }
        else
        {
            fclose(latin_file);
            const char* sample = ar; // Arabic: needs shaping with any font (missing glyphs for Latin fonts)
            const int sample_len = (int)strlen(sample);
            unsigned int ref_gids[64] = { 0 };
            int ref_n = 0;

            // Reference: a fresh context with only the second font.
            ImGui::DestroyContext();
            ImGui::CreateContext();
            {
                ImGuiIO& ior = ImGui::GetIO();
                ior.DisplaySize = ImVec2(640, 480);
                ImFontConfig cfgr;
                cfgr.OversampleH = cfgr.OversampleV = 1;
                ImFont* fr = ior.Fonts->AddFontFromFileTTF(latin_only, 22.0f, &cfgr);
                ior.Fonts->Build();
                const ImShapedGlyph* g = NULL; int n = 0, d = 0;
                if (fr != NULL && ior.Fonts->FontShaper->ShapeText(fr, fr->GetFontBaked(22.0f), sample, sample + sample_len, &g, &n, &d))
                {
                    ref_n = ImMin(n, 64);
                    for (int i = 0; i < ref_n; i++)
                        ref_gids[i] = g[i].GlyphId;
                }
            }

            // Test: first font, then remove it and add the second one (likely reusing the address).
            ImGui::DestroyContext();
            ImGui::CreateContext();
            unsigned int first_font_gid = 0;
            unsigned int test_gids[64] = { 0 };
            int test_n = 0;
            bool ok_test = false;
            {
                ImGuiIO& iot = ImGui::GetIO();
                iot.DisplaySize = ImVec2(640, 480);
                ImFontConfig cfgt;
                cfgt.OversampleH = cfgt.OversampleV = 1;
                ImFont* f1 = iot.Fonts->AddFontFromFileTTF(font_path, 22.0f, &cfgt);
                iot.Fonts->Build();
                const ImShapedGlyph* g = NULL; int n = 0, d = 0;
                if (f1 != NULL && iot.Fonts->FontShaper->ShapeText(f1, f1->GetFontBaked(22.0f), sample, sample + sample_len, &g, &n, &d) && n > 0)
                    first_font_gid = g[0].GlyphId;
                iot.Fonts->RemoveFont(f1);
                ImFont* f2 = iot.Fonts->AddFontFromFileTTF(latin_only, 22.0f, &cfgt);
                iot.Fonts->Build();
                const ImShapedGlyph* g2 = NULL; int n2 = 0, d2 = 0;
                ok_test = (f2 != NULL) && iot.Fonts->FontShaper->ShapeText(f2, f2->GetFontBaked(22.0f), sample, sample + sample_len, &g2, &n2, &d2);
                if (ok_test)
                {
                    test_n = ImMin(n2, 64);
                    for (int i = 0; i < test_n; i++)
                        test_gids[i] = g2[i].GlyphId;
                }
            }
            bool same = ok_test && test_n == ref_n && test_n > 0;
            for (int i = 0; same && i < ref_n; i++)
                if (ref_gids[i] != test_gids[i])
                    same = false;
            char detail[192];
            snprintf(detail, sizeof(detail), "first-font gid=%u, n=%d/%d, gid[0]=%u/%u (reference/test)",
                     first_font_gid, test_n, ref_n, ref_gids[0], test_gids[0]);
            Check(same, "font removal + re-add does not reuse a stale FT_Face (matches fresh reference)", detail);
        }
    }

    printf("\n=== Regression tests: %d/%d passed ===\n", GTestCount - GTestFailures, GTestCount);
    ImGui::DestroyContext();
    return (GTestFailures == 0) ? 0 : 1;
}
