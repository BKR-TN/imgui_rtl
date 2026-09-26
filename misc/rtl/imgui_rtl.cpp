// dear imgui: RTL / complex text shaping addon (bidi + Arabic shaping etc.)
// Built on libraqm (HarfBuzz shaping + SheenBidi bidi + FreeType).

#include "imgui_rtl.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <raqm.h>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static ImGuiRTL::Direction GDirection = ImGuiRTL::Direction_Default;

// FreeType library (created lazily).
static FT_Library GFreeTypeLibrary = NULL;

// Reusable raqm_t (raqm objects are designed to be reused via raqm_clear_contents()).
static raqm_t* GReusableRaqm = NULL;

// Per-font FT_Face cache. Keyed by ImFont*; one FT_Face per font source (a font can be
// composed of several merged sources, e.g. a Latin font + an Arabic font). Faces are
// created lazily from the raw TTF/OTF data, which persists for the lifetime of the atlas.
// Faces live in a flat pool (GFontFacePool) so the cache entry stays a plain POD and
// ImVector's memcpy-based growth is safe (an ImVector inside the entry would be double-freed).
//
// Lifetime: the entry is dropped by ImGuiRTL_DestroyFontFaces(), which the core calls (through
// ImFontShaper::FontDestroyed) right before an ImFont is destroyed. The stored FontId/Atlas and the
// per-face source description are also validated on lookup, so a recycled ImFont* address, or a
// source whose font data changed, re-opens the faces instead of using stale ones.
struct ImGuiRTLFace
{
    FT_Face     Face;
    FT_UInt     Height;         // Last requested pixel height (0 == unset).
    const void* FontData;       // ImFontConfig::FontData the face was opened from.
    int         FontDataSize;
    int         FontNo;
};

struct ImGuiRTLFontFace
{
    ImFont*             Font;
    ImGuiID             FontId;         // ImFont::FontId at creation time (detects recycled ImFont* addresses).
    ImFontAtlas*        Atlas;          // Owner atlas at creation time.
    int                 FaceOffset;     // Index of the first ImGuiRTLFace in GFontFacePool.
    int                 FaceCount;      // Number of sources (faces); entries may be NULL.
};
static ImVector<ImGuiRTLFontFace> GFontFaces;
static ImVector<ImGuiRTLFace>     GFontFacePool;      // flat pool of faces, one per source.

static FT_Face ImGuiRTL_GetFace(ImGuiRTLFontFace* fe, int i)  { return GFontFacePool[fe->FaceOffset + i].Face; }
static FT_UInt& ImGuiRTL_GetFaceHeight(ImGuiRTLFontFace* fe, int i) { return GFontFacePool[fe->FaceOffset + i].Height; }

// Close and forget the cached faces of one font. Called from ImFontShaper::FontDestroyed().
static void ImGuiRTL_DestroyFontFaces(ImFont* font)
{
    for (int i = 0; i < GFontFaces.Size; i++)
    {
        ImGuiRTLFontFace& fe = GFontFaces[i];
        if (fe.Font != font)
            continue;
        for (int n = 0; n < fe.FaceCount; n++)
            if (GFontFacePool[fe.FaceOffset + n].Face != NULL)
                FT_Done_Face(GFontFacePool[fe.FaceOffset + n].Face);
        const int first = fe.FaceOffset;
        const int count = fe.FaceCount;
        GFontFacePool.erase(GFontFacePool.begin() + first, GFontFacePool.begin() + first + count);
        for (int j = 0; j < GFontFaces.Size; j++)
            if (GFontFaces[j].FaceOffset > first)
                GFontFaces[j].FaceOffset -= count;
        GFontFaces.erase(GFontFaces.begin() + i);
        return;
    }
}

// ---------------------------------------------------------------------------
// Per-frame shaping cache
// ---------------------------------------------------------------------------
// Shaping is expensive (HarfBuzz bidi + shaping), and the same text range is shaped multiple
// times (wrap pass, measure pass, render pass) — and static labels are re-shaped every frame.
// This cache deduplicates those calls, and persists across frames so static text isn't re-shaped.
//
// Safety guarantees (intentionally conservative):
// - Content-keyed: text is identified by (pointer, length, FNV-1a hash of the bytes). The hash is
//   the authority, so editing a buffer in place yields a miss (no stale glyphs).
// - Stable identity: font + size + density are keyed by ImFontBaked::BakedId (a stable hash), not
//   by the baked pointer, so an atlas rebuild that reuses a heap address can't produce stale hits.
// - Direction-keyed: ImGuiRTL::SetDirection() changes shaping, so Direction is part of the key.
// - Bounded: capped at IMGUI_RTL_SHAPE_CACHE_SIZE entries; on overflow the whole cache is cleared
//   (simple, no stale entries, no unbounded growth).
// - Owned: glyphs are stored in a flat pool (GShapeCacheGlyphs); entries reference a
//   [GlyphOffset, GlyphOffset+GlyphCount) slice of it. The entry struct is a plain POD so
//   ImVector's memcpy-based growth is safe. Callers must still not hold a result across
//   another ShapeText() call, since pool growth can reallocate the backing buffer.
//
// Sizing: the cache is a linear scan, and on overflow it is cleared wholesale (no LRU). So the
// size is a hit-rate / scan-cost tradeoff:
//   - 512  covers a typical widget UI (a few hundred distinct strings) but thrashes in
//          text-dense apps whose working set exceeds it.
//   - 2048 covers text-heavy apps (~4x the working set) while a full miss-scan is still a few
//          microseconds — far cheaper than a shaping miss (~30us+). Memory is ~0.8 MB.
//   - Beyond ~2048-4096 the linear-scan cost approaches the miss cost; at that point a
//          hash-map/LRU would be the right improvement, not a larger array.
#define IMGUI_RTL_SHAPE_CACHE_SIZE 2048

struct ImGuiRTLShapeCacheEntry
{
    ImFontAtlas*                Atlas;          // Owner atlas: BakedId (and ImFont::FontId) is only unique per atlas.
    ImGuiID                     BakedId;        // ImFontBaked::BakedId: stable identity for (font, size, density).
    const char*                 TextBegin;
    size_t                      TextLen;
    ImU64                       TextHash;
    int                         Direction;      // ImGuiRTL::GDirection at shaping time (affects base direction).
    int                         GlyphOffset;    // Start index into GShapeCacheGlyphs.
    int                         GlyphCount;
    int                         BaseDirection;
};

static ImVector<ImGuiRTLShapeCacheEntry> GShapeCache;    // Cache entries (POD, memcpy-safe).
static ImVector<ImShapedGlyph>           GShapeCacheGlyphs; // Flat pool of owned glyphs.

// ImFontShaper::FontDestroyed callback.
// Also drop the shape cache: its entries are keyed by ImFontBaked::BakedId (and atlas), which are
// only unique while the atlas/fonts are alive, so a recycled identity could otherwise return stale
// glyphs. Font destruction is rare (context teardown, RemoveFont, ClearFonts), so flushing wholesale
// is cheap and can never leave a stale entry behind.
static void ImGuiRTL_FontDestroyed(ImFont* font)
{
    ImGuiRTL_DestroyFontFaces(font);
    GShapeCache.clear();
    GShapeCacheGlyphs.clear();
}

// Shaper-owned output buffer (valid until the next ShapeText() call).
static ImVector<ImShapedGlyph> GShapedGlyphs;


static ImU64 ImGuiRTL_HashBytes(const char* data, size_t len)
{
    ImU64 h = 1469598103934665603ull; // FNV-1a offset basis.
    for (size_t i = 0; i < len; i++)
    {
        h ^= (ImU64)(unsigned char)data[i];
        h *= 1099511628211ull;         // FNV-1a prime.
    }
    return h;
}

static ImGuiRTLFontFace* ImGuiRTL_GetFaceEntry(ImFont* font)
{
    for (int i = 0; i < GFontFaces.Size; i++)
    {
        ImGuiRTLFontFace& fe = GFontFaces[i];
        if (fe.Font != font)
            continue;

        // Validate the cached entry: the ImFont pointer alone is not a stable identity (a new font
        // can be allocated at a recycled address) and a source's font data can be replaced without
        // the ImFont object changing (e.g. SetFontLoader(), re-added sources).
        bool valid = (fe.FontId == font->FontId && fe.Atlas == font->OwnerAtlas && fe.FaceCount == font->Sources.Size);
        for (int n = 0; valid && n < fe.FaceCount; n++)
        {
            const ImGuiRTLFace& f = GFontFacePool[fe.FaceOffset + n];
            const ImFontConfig* src = font->Sources[n];
            valid = (f.FontData == src->FontData && f.FontDataSize == (int)src->FontDataSize && f.FontNo == (int)src->FontNo);
        }
        if (valid)
            return &fe;

        ImGuiRTL_DestroyFontFaces(font); // Stale: close the faces and re-open below.
        break;
    }

    if (GFreeTypeLibrary == NULL)
    {
        if (FT_Init_FreeType(&GFreeTypeLibrary) != 0)
            return NULL;
    }

    if (font->Sources.Size == 0 || font->Sources.Size > 16)
        return NULL; // ImFontGlyph::SourceIdx is 4-bit: we cannot address more sources.

    // Open one FT_Face per source. A source with no font data yields a NULL face and is
    // skipped when assigning faces to characters (raqm then never references it).
    ImGuiRTLFontFace entry;
    entry.Font = font;
    entry.FontId = font->FontId;
    entry.Atlas = font->OwnerAtlas;
    entry.FaceOffset = GFontFacePool.Size;
    entry.FaceCount = font->Sources.Size;

    for (int i = 0; i < font->Sources.Size; i++)
    {
        ImFontConfig* src = font->Sources[i];
        ImGuiRTLFace f;
        f.Face = NULL;
        f.Height = 0;
        f.FontData = src->FontData;
        f.FontDataSize = (int)src->FontDataSize;
        f.FontNo = (int)src->FontNo;
        if (src->FontData != NULL && src->FontDataSize > 0)
            FT_New_Memory_Face(GFreeTypeLibrary, (const FT_Byte*)src->FontData, (FT_ULong)src->FontDataSize, (FT_Long)src->FontNo, &f.Face);
        GFontFacePool.push_back(f);
    }

    if (GFontFacePool.Data[entry.FaceOffset].Face == NULL)
    {
        // Roll back the pushed faces on failure (first source must be usable).
        GFontFacePool.resize(entry.FaceOffset);
        return NULL;
    }

    GFontFaces.push_back(entry);
    return &GFontFaces.back();
}

// Get (or create) the reusable raqm_t.
static raqm_t* ImGuiRTL_GetRaqm()
{
    if (GReusableRaqm == NULL)
        GReusableRaqm = raqm_create();
    return GReusableRaqm;
}

// Rasterizer density for one source: the reciprocal of the baked-size scale used when
// converting raqm's 26.6 fixed-point values for that source's glyphs to baked-size pixels.
static float ImGuiRTL_GetSourceDensity(ImFont* font, ImFontBaked* baked, int src_idx)
{
    ImFontConfig* src = (src_idx < font->Sources.Size) ? font->Sources[src_idx] : NULL;
    float density = baked->RasterizerDensity;
    if (src != NULL)
        density *= src->RasterizerDensity;
    return density;
}

// Pixel size used to rasterize one source, matching the FreeType glyph loader exactly:
// baked size, scaled by the merge SizePixels ratio and the source's ExtraSizeScale.
static float ImGuiRTL_GetSourceSize(ImFont* font, ImFontBaked* baked, int src_idx)
{
    ImFontConfig* src = font->Sources[src_idx];
    float size = baked->Size;
    if (src->MergeMode && src->SizePixels != 0.0f)
    {
        const float ref_size = (font->Sources.Size > 0) ? font->Sources[0]->SizePixels : 0.0f;
        if (ref_size != 0.0f)
            size *= src->SizePixels / ref_size;
    }
    size *= src->ExtraSizeScale;
    return size;
}

// Density of the first source (used as the uniform scale for the single-scale APIs, e.g.
// XOffsetToIndex; merged sources are expected to share a density in practice).
static float ImGuiRTL_GetDensity(ImFont* font, ImFontBaked* baked)
{
    return ImGuiRTL_GetSourceDensity(font, baked, 0);
}

// First source whose face actually contains the codepoint, honoring GlyphExcludeRanges.
// This mirrors the atlas's merge order (first source that can provide the glyph wins).
static int ImGuiRTL_FindSourceForCodepoint(ImGuiRTLFontFace* fe, ImFont* font, unsigned int codepoint)
{
    for (int i = 0; i < fe->FaceCount; i++)
    {
        FT_Face face = ImGuiRTL_GetFace(fe, i);
        ImFontConfig* src = (i < font->Sources.Size) ? font->Sources[i] : NULL;
        if (face == NULL || src == NULL)
            continue;

        if (const ImWchar* ex = src->GlyphExcludeRanges)
        {
            bool excluded = false;
            for (; ex[0] != 0; ex += 2)
                if (codepoint >= ex[0] && codepoint <= ex[1]) { excluded = true; break; }
            if (excluded)
                continue;
        }

        if (FT_Get_Char_Index(face, codepoint) != 0)
            return i;
    }
    return 0; // fall back to the first source
}

// Set the FreeType size of every face to match the imgui glyph loader (REAL_DIM + density),
// unless already set. Returns false on error.
static bool ImGuiRTL_EnsureFacesSized(ImGuiRTLFontFace* fe, ImFont* font, ImFontBaked* baked)
{
    for (int i = 0; i < fe->FaceCount; i++)
    {
        FT_Face face = ImGuiRTL_GetFace(fe, i);
        if (face == NULL)
            continue;

        const float size = ImGuiRTL_GetSourceSize(font, baked, i);
        const float density = ImGuiRTL_GetSourceDensity(font, baked, i);
        const FT_UInt height = (FT_UInt)(size * 64.0f * density);
        if (ImGuiRTL_GetFaceHeight(fe, i) == height)
            continue;

        // Match the FreeType glyph loader's size setup exactly (REAL_DIM + rasterizer
        // density), so the advances/offsets we compute are at the same scale as the glyph
        // bitmaps the loader rasterizes. Otherwise fonts whose real ascender+descender
        // differs from their em size (e.g. Noto Naskh Arabic) would have a scale mismatch
        // and letters would be drawn with gaps.
        FT_Size_RequestRec req;
        req.type = FT_SIZE_REQUEST_TYPE_REAL_DIM;
        req.width = 0;
        req.height = height;
        req.horiResolution = 0;
        req.vertResolution = 0;
        if (FT_Request_Size(face, &req) != 0)
            return false;
        ImGuiRTL_GetFaceHeight(fe, i) = height;
    }
    return true;
}

// Assign an FT_Face to every character via raqm_set_freetype_face_range(), grouping adjacent
// characters that resolve to the same source. Indices are UTF-8 byte offsets (raqm indexes
// UTF-8 text by byte).
static void ImGuiRTL_AssignFaces(ImGuiRTLFontFace* fe, ImFont* font, raqm_t* rq, const char* text_begin, const char* text_end)
{
    int cur_source = -1;
    const char* block_start = text_begin;
    for (const char* p = text_begin; p < text_end; )
    {
        unsigned int c = (unsigned char)*p;
        const char* next = p + ((c < 0x80) ? 1 : ImTextCharFromUtf8(&c, p, text_end));
        const int src = ImGuiRTL_FindSourceForCodepoint(fe, font, c);
        if (src != cur_source)
        {
            if (cur_source >= 0)
                raqm_set_freetype_face_range(rq, ImGuiRTL_GetFace(fe, cur_source), (size_t)(block_start - text_begin), (size_t)(p - block_start));
            cur_source = src;
            block_start = p;
        }
        p = next;
    }
    if (cur_source >= 0)
        raqm_set_freetype_face_range(rq, ImGuiRTL_GetFace(fe, cur_source), (size_t)(block_start - text_begin), (size_t)(text_end - block_start));
}

// Prepare the reusable raqm_t for the given text: size all faces, set text/faces/direction,
// and run layout. Returns the raqm_t (NULL on failure) and the uniform density used for
// single-scale pixel conversion.
static raqm_t* ImGuiRTL_Prepare(ImFont* font, ImFontBaked* baked, const char* text_begin, const char* text_end, float* out_density)
{
    ImGuiRTLFontFace* fe = ImGuiRTL_GetFaceEntry(font);
    if (fe == NULL || fe->FaceCount == 0 || ImGuiRTL_GetFace(fe, 0) == NULL)
        return NULL;
    if (!ImGuiRTL_EnsureFacesSized(fe, font, baked))
        return NULL;

    raqm_t* rq = ImGuiRTL_GetRaqm();
    if (rq == NULL)
        return NULL;
    raqm_clear_contents(rq);

    raqm_set_text_utf8(rq, text_begin, (size_t)(text_end - text_begin));
    ImGuiRTL_AssignFaces(fe, font, rq, text_begin, text_end);
    if (GDirection == ImGuiRTL::Direction_RTL)
        raqm_set_par_direction(rq, RAQM_DIRECTION_RTL);
    else if (GDirection == ImGuiRTL::Direction_LTR)
        raqm_set_par_direction(rq, RAQM_DIRECTION_LTR);
    else
        raqm_set_par_direction(rq, RAQM_DIRECTION_DEFAULT);

    if (!raqm_layout(rq))
        return NULL;

    if (out_density != NULL)
        *out_density = ImGuiRTL_GetDensity(font, baked);
    return rq;
}

// Source index of the face that produced a glyph (identified by its FT_Face).
static int ImGuiRTL_GlyphSourceIndex(ImGuiRTLFontFace* fe, FT_Face face)
{
    for (int i = 0; i < fe->FaceCount; i++)
        if (ImGuiRTL_GetFace(fe, i) == face)
            return i;
    return 0;
}

// Density of the source that produced a glyph (identified by its FT_Face), so merged sources
// with different RasterizerDensity still convert advances to the same pixel scale.
static float ImGuiRTL_GlyphDensity(ImGuiRTLFontFace* fe, ImFont* font, ImFontBaked* baked, FT_Face face)
{
    return ImGuiRTL_GetSourceDensity(font, baked, ImGuiRTL_GlyphSourceIndex(fe, face));
}

// ---------------------------------------------------------------------------
// "Do we need the shaping backend at all?" pre-check
// ---------------------------------------------------------------------------
// Shaping with libraqm is expensive (~30us per unique string), so we only route text to it when it
// may actually need bidi reordering, contextual joining, mark positioning, mirroring or
// composition. The test below is deliberately conservative: only codepoints that are unambiguously
// safe to draw with the standard left-to-right, one-codepoint-per-glyph path are considered
// "simple" (Latin/Greek/Cyrillic/Armenian/Georgian, CJK, Kana, Hangul syllables, common
// punctuation/symbols). Everything else — including unassigned codepoints, combining marks, RTL
// scripts, Indic/SE-Asian scripts and anything outside the BMP — is shaped.
static bool ImGuiRTL_CodepointIsSimple(unsigned int c)
{
    // Latin (incl. precomposed accented), IPA, spacing modifiers, Greek, Cyrillic, Armenian, Georgian.
    // Excludes the Combining Diacritical Marks block and the Cyrillic combining marks.
    if (c >= 0x00A0 && c <= 0x058F)
        return !((c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489));
    if (c >= 0x1D00 && c <= 0x1D7F) return true;    // Phonetic Extensions
    if (c >= 0x1E00 && c <= 0x1FFF) return true;    // Latin Extended Additional + Greek Extended
    if (c >= 0x2000 && c <= 0x206F)                 // General Punctuation
    {
        // Bidi controls and joiners change layout/shaping: shape them (ZWSP and word-joiner are fine).
        if (c == 0x200C || c == 0x200D || c == 0x200E || c == 0x200F) return false;
        if (c >= 0x202A && c <= 0x202E) return false;
        if (c >= 0x2066 && c <= 0x2069) return false;
        return true;
    }
    if (c >= 0x2070 && c <= 0x20FF)                 // Super/subscripts, currency, letterlike symbols
        return !(c >= 0x20D0 && c <= 0x20FF);       // (Combining Diacritical Marks for Symbols)
    if (c >= 0x2100 && c <= 0x2BFF) return true;    // Letterlike, arrows, math, symbols
    if (c >= 0x2E00 && c <= 0x2E7F) return true;    // Supplemental Punctuation
    if (c >= 0x3000 && c <= 0x30FF)                 // CJK symbols, Hiragana, Katakana
        return !((c >= 0x302A && c <= 0x302F) || (c >= 0x3099 && c <= 0x309A)); // combining tone/voicing marks
    if (c >= 0x3100 && c <= 0x312F) return true;    // Bopomofo
    if (c >= 0x3130 && c <= 0x318F) return true;    // Hangul Compatibility Jamo (display forms)
    if (c >= 0x31F0 && c <= 0x33FF) return true;    // Katakana extensions, enclosed CJK
    if (c >= 0x3400 && c <= 0x4DBF) return true;    // CJK Extension A
    if (c >= 0x4E00 && c <= 0x9FFF) return true;    // CJK Unified Ideographs
    if (c >= 0xA000 && c <= 0xA4CF) return true;    // Yi
    if (c >= 0xA720 && c <= 0xA7FF) return true;    // Latin Extended-D
    if (c >= 0xAC00 && c <= 0xD7A3) return true;    // Hangul Syllables
    if (c >= 0xF900 && c <= 0xFAFF) return true;    // CJK Compatibility Ideographs
    if (c >= 0xFE30 && c <= 0xFE4F) return true;    // CJK Compatibility Forms
    if (c >= 0xFF00 && c <= 0xFFEF)                 // Halfwidth and Fullwidth Forms
        return !(c == 0xFF9E || c == 0xFF9F);       // (halfwidth voicing marks are combining)
    return false;                                   // RTL/complex/unknown/astral: shape it.
}

// See ImGuiRTL::SetSimpleScriptFastPath(). When false, everything (except pure ASCII, which the
// core filters before calling us) is sent to the shaper.
static bool GSimpleScriptFastPath = true;

// True when 'text' may need shaping (i.e. contains at least one non-simple codepoint).
static bool ImGuiRTL_TextNeedsShaping(const char* text_begin, const char* text_end)
{
    if (!GSimpleScriptFastPath)
    {
        for (const char* p = text_begin; p < text_end; p++)
            if ((unsigned char)*p >= 0x80)
                return true;
        return false;
    }
    for (const char* p = text_begin; p < text_end; )
    {
        unsigned int c = (unsigned char)*p;
        p += (c < 0x80) ? 1 : ImTextCharFromUtf8(&c, p, text_end);
        if (c >= 0x80 && !ImGuiRTL_CodepointIsSimple(c))
            return true;
    }
    return false;
}

// ImFontShaper::TextNeedsShaping callback.
static bool ImGuiRTL_TextNeedsShapingFn(const char* text_begin, const char* text_end)
{
    return ImGuiRTL_TextNeedsShaping(text_begin, text_end);
}

// libraqm/SheenBidi rejects text containing a bidi class-B character (CR, NEL, U+2029): raqm_layout()
// fails and returns no glyphs, which would make the whole line fall back to the unshaped LTR path.
// We therefore shape a copy of the text with those characters removed and remap the glyph clusters
// back to the original byte offsets. '\r' is invisible in ImGui (the codepoint path skips it); NEL
// and U+2029 lose their advance, which is preferable to losing shaping for the whole line.
static bool ImGuiRTL_CodepointBreaksLayout(unsigned int c)
{
    return c == '\r' || c == 0x85 || c == 0x2029;
}

static bool ImGuiRTL_TextHasLayoutBreaker(const char* text_begin, const char* text_end)
{
    for (const char* p = text_begin; p < text_end; )
    {
        unsigned int c = (unsigned char)*p;
        p += (c < 0x80) ? 1 : ImTextCharFromUtf8(&c, p, text_end);
        if (ImGuiRTL_CodepointBreaksLayout(c))
            return true;
    }
    return false;
}

// First codepoint of a cluster (used by the core to treat control characters like the codepoint path).
static unsigned int ImGuiRTL_DecodeCodepointAt(const char* text, const char* text_end, size_t byte_offset)
{
    const char* p = text + byte_offset;
    if (p >= text_end)
        return 0;
    unsigned int c = (unsigned char)*p;
    if (c >= 0x80)
        ImTextCharFromUtf8(&c, p, text_end);
    return c;
}

static bool ImGuiRTL_ShapeText(ImFont* font, ImFontBaked* baked, const char* text_begin, const char* text_end, const ImShapedGlyph** out_glyphs, int* out_glyph_count, int* out_base_direction)
{
    *out_glyphs = NULL;
    *out_glyph_count = 0;
    *out_base_direction = 0;

    if (text_begin >= text_end)
        return true;

    // Fast path: text that needs no shaping at all (pure ASCII, or simple LTR scripts only) falls
    // back to the standard codepoint path. Keeps the common case fast and byte-identical.
    if (!ImGuiRTL_TextNeedsShaping(text_begin, text_end))
        return false;

    // --- Cache lookup (content-keyed; persists across frames) ---
    const size_t text_len = (size_t)(text_end - text_begin);
    const ImU64 text_hash = ImGuiRTL_HashBytes(text_begin, text_len);
    const ImGuiID baked_id = baked->BakedId;
    ImFontAtlas* atlas = font->OwnerAtlas;
    for (int i = 0; i < GShapeCache.Size; i++)
    {
        const ImGuiRTLShapeCacheEntry& e = GShapeCache[i];
        if (e.Atlas == atlas && e.BakedId == baked_id && e.TextLen == text_len && e.TextHash == text_hash && e.TextBegin == text_begin && e.Direction == GDirection)
        {
            *out_glyphs = GShapeCacheGlyphs.Data + e.GlyphOffset;
            *out_glyph_count = e.GlyphCount;
            *out_base_direction = e.BaseDirection;
            return true;
        }
    }

    // --- Miss: shape ---
    // Optionally build a copy without bidi class-B characters (see ImGuiRTL_CodepointBreaksLayout).
    ImVector<char> sanitized;
    ImVector<int> sanitized_to_original; // sanitized byte offset -> original byte offset
    const char* shape_begin = text_begin;
    const char* shape_end = text_end;
    if (ImGuiRTL_TextHasLayoutBreaker(text_begin, text_end))
    {
        sanitized.reserve((int)text_len + 1);
        for (const char* p = text_begin; p < text_end; )
        {
            unsigned int c = (unsigned char)*p;
            const char* next = p + ((c < 0x80) ? 1 : ImTextCharFromUtf8(&c, p, text_end));
            if (ImGuiRTL_CodepointBreaksLayout(c))
            {
                p = next;
                continue;
            }
            for (const char* q = p; q < next; q++)
            {
                sanitized.push_back(*q);
                sanitized_to_original.push_back((int)(q - text_begin));
            }
            p = next;
        }
        sanitized_to_original.push_back((int)text_len); // Offset for "end of text".
        if (sanitized.Size == 0)
            return false; // Nothing left to shape.
        shape_begin = sanitized.Data;
        shape_end = sanitized.Data + sanitized.Size;
    }
    // '\n' is also a bidi class-B character, and the core always shapes one logical line at a time
    // (the shaped measure/render/wrap paths split on '\n' first). Shape only up to the first newline
    // so that a direct multi-line call degrades gracefully instead of failing the whole layout.
    if (const char* nl = (const char*)ImMemchr(shape_begin, '\n', (size_t)(shape_end - shape_begin)))
        shape_end = nl;
    if (shape_begin >= shape_end)
        return false;
    const bool remap_clusters = (sanitized_to_original.Size > 0);
    const size_t shape_len = (size_t)(shape_end - shape_begin);

    float density = 1.0f;
    raqm_t* rq = ImGuiRTL_Prepare(font, baked, shape_begin, shape_end, &density);
    if (rq == NULL)
        return false;
    ImGuiRTLFontFace* fe = ImGuiRTL_GetFaceEntry(font);

    bool ok = false;
    int base_direction = 0;
    size_t len = 0;
    raqm_glyph_t* raqm_glyphs = raqm_get_glyphs(rq, &len);
    if (raqm_glyphs != NULL && len > 0)
    {
        // Map UTF-8 byte offsets to UTF-32 codepoint indices (raqm indexes runs by codepoint,
        // but glyph clusters are UTF-8 byte offsets). Continuation bytes map to the index of the
        // codepoint they belong to. Used to resolve the per-glyph bidi direction below.
        ImVector<int> byte_to_u32;
        byte_to_u32.resize((int)shape_len + 1);
        {
            int u32 = 0;
            for (const char* p = shape_begin; p < shape_end; )
            {
                unsigned int c = (unsigned char)*p;
                const char* next = p + ((c < 0x80) ? 1 : ImTextCharFromUtf8(&c, p, shape_end));
                for (const char* q = p; q < next; q++)
                    byte_to_u32[(int)(q - shape_begin)] = u32;
                p = next;
                u32++;
            }
            byte_to_u32[(int)shape_len] = u32;
        }

        GShapedGlyphs.resize((int)len);
        int prev_cluster = -1;
        int prev_dir = 0;
        for (size_t i = 0; i < len; i++)
        {
            ImShapedGlyph& out = GShapedGlyphs[(int)i];
            const float g_density = (fe != NULL) ? ImGuiRTL_GlyphDensity(fe, font, baked, raqm_glyphs[i].ftface) : density;
            // Map the cluster back to the original text when the shaped copy skipped characters.
            const size_t shape_cluster = ImMin((size_t)raqm_glyphs[i].cluster, shape_len);
            const size_t cluster = remap_clusters ? (size_t)sanitized_to_original[(int)shape_cluster] : shape_cluster;

            out.GlyphId   = raqm_glyphs[i].index;
            out.SourceIdx = (fe != NULL) ? ImGuiRTL_GlyphSourceIndex(fe, raqm_glyphs[i].ftface) : 0;
            out.Codepoint = (ImWchar)ImGuiRTL_DecodeCodepointAt(text_begin, text_end, cluster);
            out.XAdvance  = (float)raqm_glyphs[i].x_advance / 64.0f / g_density;
            out.YAdvance  = -(float)raqm_glyphs[i].y_advance / 64.0f / g_density; // HarfBuzz is Y-up, ImGui is Y-down.
            out.XOffset   = (float)raqm_glyphs[i].x_offset / 64.0f / g_density;
            out.YOffset   = -(float)raqm_glyphs[i].y_offset / 64.0f / g_density;   // HarfBuzz is Y-up, ImGui is Y-down.
            // Clamp the cluster offset to the text range: HarfBuzz can report offsets past the end
            // for malformed UTF-8 (e.g. lone continuation bytes), which would otherwise produce
            // caret/selection positions outside the buffer.
            out.Cluster   = (unsigned int)ImMin(cluster, text_len);

            // Resolved bidi direction of the run this glyph belongs to (all glyphs of a cluster
            // share it). Memoized per distinct cluster; glyphs of a cluster are contiguous in
            // visual order, and a re-query on any non-contiguous repeat is still correct.
            // Note: raqm indexes runs by *shaped* (sanitized) offsets, hence shape_cluster here.
            const int cl = (int)out.Cluster;
            if (cl != prev_cluster)
            {
                const raqm_direction_t d = raqm_get_direction_at_index(rq, (size_t)byte_to_u32[(int)shape_cluster]);
                prev_dir = (d == RAQM_DIRECTION_RTL) ? 1 : 0;
                prev_cluster = cl;
            }
            out.Dir = prev_dir;
        }
        ok = true;
    }

    raqm_direction_t dir = raqm_get_par_resolved_direction(rq);
    base_direction = (dir == RAQM_DIRECTION_RTL) ? 1 : 0;

    // Populate the cache with the successful (non-empty) result.
    if (ok)
    {
        if (GShapeCache.Size >= IMGUI_RTL_SHAPE_CACHE_SIZE)
        {
            // Bounded: drop everything rather than evict arbitrarily (no stale entries).
            GShapeCache.clear();
            GShapeCacheGlyphs.clear();
        }

        const int glyph_offset = GShapeCacheGlyphs.Size;
        GShapeCacheGlyphs.resize(glyph_offset + (int)len);
        for (int i = 0; i < (int)len; i++)
            GShapeCacheGlyphs[glyph_offset + i] = GShapedGlyphs[i];

        ImGuiRTLShapeCacheEntry entry;
        entry.Atlas = atlas;
        entry.BakedId = baked->BakedId;
        entry.TextBegin = text_begin;
        entry.TextLen = text_len;
        entry.TextHash = text_hash;
        entry.Direction = GDirection;
        entry.GlyphOffset = glyph_offset;
        entry.GlyphCount = (int)len;
        entry.BaseDirection = base_direction;
        GShapeCache.push_back(entry);

        // Return the cache-owned slice (stable after push_back; glyph pool is untouched).
        *out_glyphs = GShapeCacheGlyphs.Data + glyph_offset;
        *out_glyph_count = (int)len;
    }

    *out_base_direction = base_direction;
    return ok;
}

// ---------------------------------------------------------------------------
// Unified caret-position model
// ---------------------------------------------------------------------------
// A caret can only sit at a grapheme boundary (a cluster start, or the text end). The shaped
// glyphs are in visual order, so walking them and recording one "caret stop" per distinct
// cluster (plus the text end) yields the list of visual caret positions. Each stop records:
//   - ByteOffset: the logical UTF-8 byte offset to store in the cursor.
//   - X:          the visual x (pixels, relative to text_begin) of that caret.
//
// The x of a stop is the character's *leading* visual edge for its run direction: the left
// edge for an LTR run, the right edge for an RTL run. This is what makes the caret appear on
// the correct side of each character and keeps IndexToXOffset, XOffsetToIndex and
// MoveCaretVisual all consistent with one another (previously the mixed-text path in
// IndexToXOffset used a separate, inconsistent raqm fallback).
//
// The text end is the one stop that is not a cluster: it sits at the paragraph's trailing edge
// (left edge for RTL, right edge for LTR). This can produce two stops at the same x at a bidi
// run boundary (the "before run A" and "before run B" logical positions coincide visually);
// that is correct and mirrors how browsers step through both logical positions with arrow keys.
struct ImGuiRTLCaretStop
{
    int     ByteOffset;     // logical UTF-8 byte offset (cluster start, or text end).
    float   X;              // visual x (pixels, relative to text_begin).
    int     Affinity;       // 0 = trailing (after char), 1 = leading (before char), -1 = unambiguous.
};

// Small helpers over a sorted int array (distinct cluster byte offsets).
static int ImGuiRTL_LowerBoundCluster(const int* clusters, int n, int cl)
{
    int lo = 0, hi = n;
    while (lo < hi)
    {
        const int mid = (lo + hi) / 2;
        if (clusters[mid] <= cl) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
static int ImGuiRTL_NextCluster(const int* clusters, int n, int cl, int text_len)
{
    const int i = ImGuiRTL_LowerBoundCluster(clusters, n, cl);
    return (i < n) ? clusters[i] : text_len;
}

// Build the caret-stop list in visual (left-to-right) order. Returns false for pure ASCII
// (caller falls back to the LTR codepoint path).
//
// Each distinct cluster emits TWO stops:
//   - LEADING  ("before" the character): left edge for LTR, right edge for RTL. Byte = cluster.
//   - TRAILING ("after" the character): right edge for LTR, left edge for RTL. Byte = next cluster.
//
// Within a same-direction run, one character's leading edge and its logical neighbour's trailing
// edge coincide (same byte, same x) and are merged into one unambiguous stop. At a run boundary
// they do NOT coincide, so both survive as a bidi dual-caret: the same visual x has two logical
// offsets (e.g. "before ع" and "end of text" both sit at the far-right edge). Arrow keys step
// through both logical positions at that x; the caller disambiguates which one it is on via
// 'affinity'. This is the correct, reversible model (the self-test asserts reversibility).
static bool ImGuiRTL_BuildCaretStops(ImFont* font, ImFontBaked* baked, const char* text_begin, const char* text_end, ImVector<ImGuiRTLCaretStop>& stops, int* out_base_dir)
{
    stops.clear();
    const ImShapedGlyph* glyphs = NULL;
    int glyph_count = 0;
    int base_dir = 0;
    if (!ImGuiRTL_ShapeText(font, baked, text_begin, text_end, &glyphs, &glyph_count, &base_dir))
        return false;

    if (out_base_dir != NULL)
        *out_base_dir = base_dir;
    const int text_len = (int)(text_end - text_begin);

    // Distinct cluster byte offsets, sorted ascending (visual order -> sort).
    ImVector<int> clusters;
    for (int i = 0; i < glyph_count; i++)
    {
        const int cl = (int)glyphs[i].Cluster;
        if (clusters.Size == 0 || clusters[clusters.Size - 1] != cl)
            clusters.push_back(cl);
    }
    for (int i = 1; i < clusters.Size; i++)
    {
        const int key = clusters[i];
        int j = i - 1;
        while (j >= 0 && clusters[j] > key) { clusters[j + 1] = clusters[j]; j--; }
        clusters[j + 1] = key;
    }

    float total = 0.0f;
    for (int i = 0; i < glyph_count; i++)
        total += ImFontShapedGetGlyphAdvance(baked, glyphs[i]);

    // Emit the paragraph's two edge stops in visual order: the LEFT edge first (so it becomes
    // the first stop at x=0 for the rightward walk) and the RIGHT edge last (so it wins click
    // hit-testing at the trailing edge, which XOffsetToIndex resolves to the trailing-most stop).
    ImGuiRTLCaretStop edge_left, edge_right;
    edge_left.Affinity = edge_right.Affinity = -1;
    if (base_dir == 1)
    {
        edge_left.ByteOffset  = text_len; edge_left.X  = 0.0f;  // end at the left edge.
        edge_right.ByteOffset = 0;        edge_right.X = total; // start at the right edge.
    }
    else
    {
        edge_left.ByteOffset  = 0;        edge_left.X  = 0.0f;  // start at the left edge.
        edge_right.ByteOffset = text_len; edge_right.X = total; // end at the right edge.
    }
    stops.push_back(edge_left);

    // Walk glyphs in visual order, one distinct cluster at a time, emitting both edges.
    float pen = 0.0f;
    int i = 0;
    while (i < glyph_count)
    {
        const int cl = (int)glyphs[i].Cluster;
        if (cl < 0 || cl > text_len)
        {
            i++; // Defensive: ignore a malformed cluster reported by a shaper.
            continue;
        }
        const int dir = glyphs[i].Dir;
        const float start_x = pen;
        while (i < glyph_count && (int)glyphs[i].Cluster == cl)
        {
            pen += ImFontShapedGetGlyphAdvance(baked, glyphs[i]); // Same advances as the renderer (masking/controls included).
            i++;
        }
        const float end_x = pen;
        const int next_cl = ImGuiRTL_NextCluster(clusters.Data, clusters.Size, cl, text_len);

        // Leading stop ("before" the character).
        ImGuiRTLCaretStop lead;
        lead.ByteOffset = cl;
        lead.X = (dir == 1) ? end_x : start_x;
        lead.Affinity = 1;
        stops.push_back(lead);

        // Trailing stop ("after" the character).
        ImGuiRTLCaretStop trail;
        trail.ByteOffset = next_cl;
        trail.X = (dir == 1) ? start_x : end_x;
        trail.Affinity = 0;
        stops.push_back(trail);
    }

    stops.push_back(edge_right);

    // Stable sort by X (visual order). Insertion sort is stable and cheap for the stop count.
    for (int a = 1; a < stops.Size; a++)
    {
        const ImGuiRTLCaretStop key = stops[a];
        int b = a - 1;
        while (b >= 0 && stops[b].X > key.X) { stops[b + 1] = stops[b]; b--; }
        stops[b + 1] = key;
    }

    // Merge adjacent stops that are the SAME logical byte at the SAME x (a leading edge that
    // coincides with the previous character's trailing edge in a same-direction run) into a
    // single unambiguous stop. Run-boundary duals have different bytes or different x and are
    // left as-is.
    for (int a = 1; a < stops.Size; )
    {
        ImGuiRTLCaretStop& prev = stops[a - 1];
        const ImGuiRTLCaretStop& cur = stops[a];
        if (prev.ByteOffset == cur.ByteOffset && prev.X == cur.X)
        {
            prev.Affinity = -1; // now reachable from either side.
            stops.erase(stops.Data + a);
        }
        else
        {
            a++;
        }
    }
    return true;
}

// Find the caret stop for 'byte_offset', disambiguating dual-caret positions by affinity.
// Preference order: exact affinity match, then an unambiguous (-1) stop, then any occurrence.
static int ImGuiRTL_FindCaretStop(const ImVector<ImGuiRTLCaretStop>& stops, int byte_offset, int affinity)
{
    for (int i = 0; i < stops.Size; i++)
        if (stops[i].ByteOffset == byte_offset && stops[i].Affinity == affinity)
            return i;
    for (int i = 0; i < stops.Size; i++)
        if (stops[i].ByteOffset == byte_offset && stops[i].Affinity == -1)
            return i;
    for (int i = 0; i < stops.Size; i++)
        if (stops[i].ByteOffset == byte_offset)
            return i;
    return -1;
}

// Map a logical byte offset to its visual x offset (pixels, relative to text_begin).
// Returns -1.0f on failure (caller falls back to the LTR behavior). A byte offset inside a
// grapheme (e.g. a combining mark) snaps to the next logical caret stop, mirroring how the
// caret actually moves (there is no caret mid-grapheme). 'affinity' disambiguates dual-caret
// positions (0 = trailing, 1 = leading, -1 = primary/leftmost).
static float ImGuiRTL_IndexToXOffset(ImFont* font, ImFontBaked* baked, const char* text_begin, const char* text_end, int byte_offset, int affinity)
{
    ImVector<ImGuiRTLCaretStop> stops;
    if (!ImGuiRTL_BuildCaretStops(font, baked, text_begin, text_end, stops, NULL))
        return -1.0f;

    // Snap to the nearest logical caret stop at or after byte_offset.
    int target_byte = -1;
    for (int i = 0; i < stops.Size; i++)
        if (stops[i].ByteOffset >= byte_offset && (target_byte < 0 || stops[i].ByteOffset < target_byte))
            target_byte = stops[i].ByteOffset;
    if (target_byte < 0)
        target_byte = stops[stops.Size - 1].ByteOffset; // beyond text end: clamp.

    const int best = ImGuiRTL_FindCaretStop(stops, target_byte, affinity);
    return (best >= 0) ? stops[best].X : -1.0f;
}

// Map a visual x offset (pixels, relative to text_begin) to a logical byte offset.
// Returns -1 on failure. Picks the nearest caret stop; ties resolve to the left stop so an
// exact round-trip of IndexToXOffset() returns the same byte (except at a bidi run boundary,
// where two logical positions legitimately share one x and the paragraph-trailing stop wins).
static int ImGuiRTL_XOffsetToIndex(ImFont* font, ImFontBaked* baked, const char* text_begin, const char* text_end, float x_offset)
{
    ImVector<ImGuiRTLCaretStop> stops;
    if (!ImGuiRTL_BuildCaretStops(font, baked, text_begin, text_end, stops, NULL))
        return -1;

    if (stops.Size == 0)
        return -1;
    if (x_offset <= stops[0].X)
        return stops[0].ByteOffset;
    if (x_offset >= stops[stops.Size - 1].X)
        return stops[stops.Size - 1].ByteOffset;

    // First stop with X >= x_offset (x_offset lies in [stops[i-1].X, stops[i].X]).
    int i = 1;
    while (i < stops.Size && stops[i].X < x_offset)
        i++;
    const float dl = x_offset - stops[i - 1].X;
    const float dr = stops[i].X - x_offset;
    return (dr < dl) ? stops[i].ByteOffset : stops[i - 1].ByteOffset;
}

// Fill visual highlight segments for the logical byte range [sel_begin, sel_end) within
// [text_begin, text_end). Glyphs are in visual order; a contiguous run of glyphs whose
// cluster start lies inside the range is one horizontal segment. This is what makes bidi
// selection highlights correct: a single min..max span would cover reordered text that is
// NOT selected (and miss text that is). Returns the segment count, or -1 to fall back to
// the endpoint span.
static int ImGuiRTL_GetSelectionSegments(ImFont* font, ImFontBaked* baked, const char* text_begin, const char* text_end, int sel_begin, int sel_end, float* out_segments, int max_segments)
{
    if (text_begin >= text_end || sel_begin >= sel_end || max_segments <= 0)
        return 0;

    const ImShapedGlyph* glyphs = NULL;
    int glyph_count = 0;
    int base_dir = 0;
    if (!ImGuiRTL_ShapeText(font, baked, text_begin, text_end, &glyphs, &glyph_count, &base_dir))
        return -1; // pure ASCII: caller uses the LTR path

    int seg_count = 0;
    float pen_x = 0.0f;
    float seg_x0 = 0.0f;
    bool in_seg = false;
    bool overflow = false;

    for (int i = 0; i < glyph_count; i++)
    {
        const ImShapedGlyph& g = glyphs[i];
        // A glyph is selected when the character at its cluster start is inside the logical
        // range. (Ligatures collapse several chars onto one cluster; a partially-selected
        // ligature is an accepted limitation, mirroring caret snapping.)
        const bool selected = ((int)g.Cluster >= sel_begin && (int)g.Cluster < sel_end);

        if (selected && !in_seg)
        {
            seg_x0 = pen_x;
            in_seg = true;
        }
        else if (!selected && in_seg)
        {
            if (seg_count < max_segments)
            {
                out_segments[seg_count * 2 + 0] = seg_x0;
                out_segments[seg_count * 2 + 1] = pen_x;
                seg_count++;
            }
            else
            {
                overflow = true;
            }
            in_seg = false;
        }

        pen_x += ImFontShapedGetGlyphAdvance(baked, g);
    }

    if (in_seg)
    {
        if (seg_count < max_segments)
        {
            out_segments[seg_count * 2 + 0] = seg_x0;
            out_segments[seg_count * 2 + 1] = pen_x;
            seg_count++;
        }
        else
        {
            overflow = true;
        }
    }

    // More disjoint segments than the caller can display: ask it to use its (visually coarser)
    // endpoint-span fallback rather than showing a truncated highlight.
    return overflow ? -1 : seg_count;
}

// Resolved bidi direction of the run containing the character at 'byte_offset' (relative to
// text_begin). Returns 1 (RTL), 0 (LTR), or -1 (unknown). Used for per-run arrow-key movement
// in mixed bidi text (a single logical line can contain both LTR and RTL runs).
static int ImGuiRTL_DirectionAt(ImFont* font, ImFontBaked* baked, const char* text_begin, const char* text_end, int byte_offset)
{
    if (text_begin >= text_end)
        return -1;

    const int text_len = (int)(text_end - text_begin);
    if (byte_offset >= text_len)
        byte_offset = text_len - 1; // clamp to the last character
    if (byte_offset < 0)
        byte_offset = 0;

    // Convert the UTF-8 byte offset to a UTF-32 codepoint index (raqm indexes runs by codepoint).
    int u32_index = 0;
    for (const char* p = text_begin; p < text_begin + byte_offset; )
    {
        unsigned int c = (unsigned char)*p;
        p += (c < 0x80) ? 1 : ImTextCharFromUtf8(&c, p, text_end);
        u32_index++;
    }

    float density = 1.0f;
    raqm_t* rq = ImGuiRTL_Prepare(font, baked, text_begin, text_end, &density);
    if (rq == NULL)
        return -1;

    const raqm_direction_t dir = raqm_get_direction_at_index(rq, (size_t)u32_index);
    if (dir == RAQM_DIRECTION_RTL)
        return 1;
    if (dir == RAQM_DIRECTION_LTR)
        return 0;
    return -1;
}

// Move the caret one *visual* step. The caret stops are in visual order, so stepping through
// them is the bidi-correct way to move between characters (and it skips combining marks, since
// they share their base's cluster). 'affinity' disambiguates which dual-caret position the caret
// currently occupies (0 = trailing, 1 = leading, -1 = unknown); *out_affinity receives the
// affinity of the new position. Uses the same stop list as IndexToXOffset / XOffsetToIndex, so
// the caret's x always matches its logical position.
static int ImGuiRTL_MoveCaretVisual(ImFont* font, ImFontBaked* baked, const char* text_begin, const char* text_end, int byte_offset, int visual_dir, int affinity, int* out_affinity)
{
    if (out_affinity != NULL)
        *out_affinity = -1;

    ImVector<ImGuiRTLCaretStop> stops;
    if (!ImGuiRTL_BuildCaretStops(font, baked, text_begin, text_end, stops, NULL))
        return -1; // pure ASCII: caller falls back to the LTR char step

    // Locate the current caret stop, disambiguating dual-caret positions by affinity.
    const int pos = ImGuiRTL_FindCaretStop(stops, byte_offset, affinity);
    if (pos < 0)
        return -1; // not a caret stop (shouldn't happen for a cluster boundary)

    const int new_pos = pos + visual_dir;
    if (new_pos < 0 || new_pos >= stops.Size)
    {
        // At the visual edge: no movement (keep the current affinity).
        if (out_affinity != NULL)
            *out_affinity = stops[pos].Affinity;
        return byte_offset;
    }
    if (out_affinity != NULL)
        *out_affinity = stops[new_pos].Affinity;
    return stops[new_pos].ByteOffset;
}

const ImFontShaper* ImGuiRTL::GetShaper()
{
    static ImFontShaper shaper;
    shaper.Name = "libraqm";
    shaper.ShapeText = ImGuiRTL_ShapeText;
    shaper.TextNeedsShaping = ImGuiRTL_TextNeedsShapingFn;
    shaper.IndexToXOffset = ImGuiRTL_IndexToXOffset;
    shaper.XOffsetToIndex = ImGuiRTL_XOffsetToIndex;
    shaper.GetSelectionSegments = ImGuiRTL_GetSelectionSegments;
    shaper.DirectionAt = ImGuiRTL_DirectionAt;
    shaper.MoveCaretVisual = ImGuiRTL_MoveCaretVisual;
    shaper.FontDestroyed = ImGuiRTL_FontDestroyed;
    return &shaper;
}

void ImGuiRTL::SetDirection(ImGuiRTL::Direction direction)
{
    GDirection = direction;
}

ImGuiRTL::Direction ImGuiRTL::GetDirection()
{
    return GDirection;
}

void ImGuiRTL::ClearShapeCache()
{
    GShapeCache.clear();
    GShapeCacheGlyphs.clear();
}

void ImGuiRTL::SetSimpleScriptFastPath(bool enabled)
{
    GSimpleScriptFastPath = enabled;
}

bool ImGuiRTL::GetSimpleScriptFastPath()
{
    return GSimpleScriptFastPath;
}

bool ImGuiRTL::IsRtl(const char* text, const char* text_end)
{
    if (GDirection == ImGuiRTL::Direction_RTL)
        return true;
    if (GDirection == ImGuiRTL::Direction_LTR)
        return false;
    if (text_end == NULL)
        text_end = text + ImStrlen(text);
    if (text >= text_end)
        return false;

    // Cheap pre-check: text that doesn't need shaping (pure ASCII, or simple LTR scripts such as
    // accented Latin) is never RTL, so skip the bidi analysis.
    if (!ImGuiRTL_TextNeedsShaping(text, text_end))
        return false;

    raqm_t* rq = ImGuiRTL_GetRaqm();
    if (rq == NULL)
        return false;
    raqm_clear_contents(rq);
    raqm_set_text_utf8(rq, text, (size_t)(text_end - text));
    raqm_direction_t dir = raqm_get_par_detected_direction(rq);
    return dir == RAQM_DIRECTION_RTL;
}

float ImGuiRTL::AlignTextRight(float pos_x, float max_x, const char* text, const char* text_end, float text_width)
{
    if (max_x <= pos_x)
        return pos_x;
    if (text_end == NULL)
        text_end = text + ImStrlen(text);
    if (text >= text_end)
        return pos_x;

    // IsRtl() already includes a cheap "does this need shaping at all?" pre-check.
    if (!IsRtl(text, text_end))
        return pos_x;

    if (text_width < 0.0f)
    {
        ImGuiContext& g = *GImGui;
        if (g.Font == NULL)
            return pos_x;
        text_width = g.Font->CalcTextSizeA(g.FontSize, FLT_MAX, 0.0f, text, text_end).x;
    }

    const float avail = max_x - pos_x;
    if (text_width >= avail)
        return pos_x; // Doesn't fit: leave as-is (caller may scroll/clip).
    return pos_x + (avail - text_width);
}
