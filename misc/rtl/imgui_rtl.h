// dear imgui: RTL / complex text shaping addon (bidi + Arabic shaping etc.)
// Built on libraqm (HarfBuzz shaping + SheenBidi bidi + FreeType).
//
// Usage:
// - Link libraqm (+ harfbuzz + sheenbidi + freetype).
// - Add '#define IMGUI_ENABLE_RTL' in your imconfig to automatically enable support,
//   which is equivalent to selecting the default shaper with:
//      io.Fonts->SetFontShaper(ImGuiRTL::GetShaper())
// - The shaper is applied at draw/layout time, so it works with any font loaded by
//   imgui (the raw TTF/OTF data persists for the lifetime of the atlas).
// - Only text that needs it is shaped: pure ASCII, precomposed Latin/Greek/Cyrillic, CJK,
//   Kana and Hangul syllables take the stock codepoint path (see ImFontShaper::TextNeedsShaping),
//   while RTL scripts, Indic/SE-Asian scripts, combining marks, bidi controls and astral
//   codepoints are shaped.

#pragma once
#include "imgui.h"
#include "imgui_internal.h"

#ifndef IMGUI_DISABLE

struct ImFontShaper;

namespace ImGuiRTL
{
    // Return the text shaper. Attach with io.Fonts->SetFontShaper(ImGuiRTL::GetShaper()).
    IMGUI_API const ImFontShaper* GetShaper();

    // Paragraph base direction used when shaping.
    enum Direction_
    {
        Direction_Default = 0,  // Detect from text (UBA first-strong).
        Direction_LTR     = 1,  // Force left-to-right base direction.
        Direction_RTL     = 2,  // Force right-to-left base direction.
    };
    typedef int Direction;

    IMGUI_API void SetDirection(Direction direction);
    IMGUI_API Direction GetDirection();

    // Clear the internal shaping cache (normally self-managed; useful for testing or to
    // reclaim memory after e.g. tearing down a large UI).
    IMGUI_API void ClearShapeCache();

    // By default, text that needs no bidi reordering / contextual forms / mark positioning
    // (pure ASCII, precomposed Latin/Greek/Cyrillic, CJK, Kana, Hangul syllables, …) bypasses the
    // shaping backend entirely and uses the stock codepoint path (much faster). Disable this to
    // route *all* text through the shaper, e.g. if you want HarfBuzz kerning/ligatures for Latin
    // text as well. Default: enabled.
    IMGUI_API void SetSimpleScriptFastPath(bool enabled);
    IMGUI_API bool GetSimpleScriptFastPath();

    // Whether the resolved base direction of 'text' is RTL (uses the current SetDirection()).
    IMGUI_API bool IsRtl(const char* text, const char* text_end = NULL);

    // If 'text' resolves to an RTL base direction, return the x position that right-aligns it
    // within [pos_x, max_x]. Returns pos_x unchanged for LTR text, empty text, or when the text
    // is wider than the area. 'text_width' is the text's rendered width; pass < 0.0f to measure
    // it here at the current font size.
    IMGUI_API float AlignTextRight(float pos_x, float max_x, const char* text, const char* text_end, float text_width);
}

#endif // #ifndef IMGUI_DISABLE
