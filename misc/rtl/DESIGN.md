# RTL & Arabic text shaping in Dear ImGui — design notes & the full journey

This document explains, in as much detail as I can, *why* every piece of the RTL
add-on in this directory exists, what problem it solves, and the order in which the
whole thing had to be built. It is written for the next person (probably me) who has
to touch this code, and for anyone trying to bolt complex-script / bidi text onto a
text engine that was never designed for it.

The code under discussion:

- `misc/rtl/imgui_rtl.h` / `imgui_rtl.cpp` — the shaper implementation.
- `imgui_draw.cpp` — the shaped text-layout/render path.
- `imgui_widgets.cpp` — InputText editing, selection, caret, arrows, alignment.
- `imgui.h` / `imgui_internal.h` — the small interface additions.
- `misc/freetype/imgui_freetype.cpp` — glyph-by-index loading for the FreeType loader.

Everything is gated so that **without `IMGUI_ENABLE_RTL` the result is byte-for-byte
stock Dear ImGui with no extra dependencies**.

---

## Table of contents

1. The problem in one paragraph
2. Why RTL text is genuinely hard (and why naive attempts fail)
3. The core decision: a *pluggable* shaper, not hard-coded Arabic
4. Foundational requirement #1: load a glyph by **glyph index**, not codepoint
5. Foundational requirement #2: the shaper's metrics must match the glyph loader's scale
6. The shaping pipeline (what happens on every `RenderText` / `CalcTextSize`)
7. Word wrap: breaking at word boundaries, not characters
8. Text editing: mapping logical bytes ↔ visual pixels
9. Arrow keys and "visual" direction
10. The shaping cache (the double-edged sword)
11. Right-alignment (block-level vs per-line)
12. Performance (the ASCII fast-path)
13. Compile-time gating and "no deps when disabled"
14. Known limitations and likely bugs
15. The journey, in the order it actually happened

---

## 1. The problem in one paragraph

Dear ImGui renders text by decoding UTF-8 into Unicode code points and, for each
code point, looking up a pre-rasterized glyph and advancing the pen horizontally.
That model is correct for left-to-right scripts (Latin, Cyrillic, …) where one
character = one glyph in reading order. It is **wrong** for Arabic/Hebrew, where:

- characters must be **reordered** (right-to-left, with LTR runs embedded),
- a character's **shape depends on its neighbors** (initial/medial/final/isolated forms),
- adjacent letters may join into a **ligature** (`ل` + `ا` → `لا`),
- diacritics must be **positioned** above/below their base letter (GPOS marks),
- and the *visual* order of glyphs is not the *logical* order of the bytes.

So the goal is: take the logical UTF-8 string, run the Unicode Bidirectional Algorithm
and OpenType shaping on it, and hand ImGui a list of **glyphs in visual order with
advances and offsets**, instead of a list of code points.

---

## 2. Why RTL text is genuinely hard

Several independent problems stack up, and they must all be solved at once or the
result is "kind of works, mostly broken":

1. **Bidi reordering** (Unicode Bidirectional Algorithm, UBA). "abc DEF 123"
   rendered RTL is not a simple reversal. SheenBidi solves this.

2. **Contextual shaping** (OpenType GSUB). Arabic letters are *joined cursive*; the
   correct glyph for a letter depends on the letters around it. HarfBuzz solves this.

3. **Mark positioning** (OpenType GPOS). Combining marks (harakat: fatha, damma,
   shadda, …) have zero advance and are offset onto the base glyph. HarfBuzz solves this.

4. **No 1:1 codepoint↔glyph mapping.** This is the one that breaks ImGui's whole
   glyph-cache model and is explained in section 4.

5. **Glyph-index vs codepoint for the rasterizer.** ImGui rasterizes glyphs *by
   codepoint*. A shaped glyph (a contextual form, a ligature, a decomposed mark) has
   **no single codepoint**. So a second, parallel lookup is required (section 4).

6. **Logical↔visual coordinate mapping for editing.** A caret sits *between two bytes*
   in the logical string, but is drawn *at a visual x position*. The two are not
   linearly related for RTL. Section 8.

7. **Direction of motion.** "Left" and "Right" arrows must move the caret visually,
   which for RTL is the *opposite* logical direction. Section 9.

Each of these is individually solvable, but the difficulty is that they all interact.
This is why the topic is hard to find documented coherently: it is not one trick, it
is seven tricks that must agree with each other.

---

## 3. The core decision: a pluggable shaper, not hard-coded Arabic

Rather than teaching ImGui about Arabic specifically, the cleanest integration is a
**text-shaping backend**, mirroring how ImGui already has a **font-loading backend**
(`ImFontLoader`, with stb_truetype and FreeType implementations).

The interface is `ImFontShaper` (`imgui_internal.h`):

```cpp
struct ImFontShaper
{
    const char* Name;
    bool  (*ShapeText)(ImFont*, ImFontBaked*, const char* text_begin, const char* text_end,
                       const ImShapedGlyph** out_glyphs, int* out_glyph_count, int* out_base_direction);
    bool  (*TextNeedsShaping)(const char* text_begin, const char* text_end);   // pre-check (section 13)
    float (*IndexToXOffset)(ImFont*, ImFontBaked*, const char*, const char*, int byte_offset, int affinity);
    int   (*XOffsetToIndex)(ImFont*, ImFontBaked*, const char*, const char*, float x_offset);
    int   (*GetSelectionSegments)(ImFont*, ImFontBaked*, const char*, const char*, int sel_begin, int sel_end,
                                  float* out_segments, int max_segments);
    int   (*DirectionAt)(ImFont*, ImFontBaked*, const char*, const char*, int byte_offset);
    int   (*MoveCaretVisual)(ImFont*, ImFontBaked*, const char*, const char*, int byte_offset, int visual_dir,
                             int affinity, int* out_affinity);
    void  (*FontDestroyed)(ImFont*);                                          // release per-font resources
};
```

- `ShapeText` turns logical UTF-8 into a visual-order glyph list. It may return `false`
  ("this text needs no shaping, use the plain codepoint path").
- `IndexToXOffset` / `XOffsetToIndex` are the *editing* helpers (section 8), with
  `affinity` disambiguating bidi dual-caret positions.
- `GetSelectionSegments`, `DirectionAt` and `MoveCaretVisual` implement bidi-aware
  selection highlighting, per-run arrow keys and visual caret stepping.
- `TextNeedsShaping` and `FontDestroyed` are optional hooks: the first lets simple LTR
  text bypass shaping entirely, the second is how a shaper releases cached per-font data
  (required if it caches anything keyed on `ImFont*`).

Every member except `ShapeText` is optional (`NULL` means "fall back to LTR behavior").

The shaped output is `ImShapedGlyph`:

```cpp
struct ImShapedGlyph
{
    unsigned int GlyphId;    // font glyph index (0 = .notdef)
    int          SourceIdx;  // which merged font source produced it (disambiguates glyph indices)
    int          Dir;        // resolved bidi direction of the run (0 = LTR, 1 = RTL)
    ImWchar      Codepoint;  // first codepoint of the cluster (used for control characters: tab etc.)
    float        XAdvance;   // pen advance
    float        YAdvance;
    float        XOffset;    // glyph offset from pen (GPOS mark positioning)
    float        YOffset;
    unsigned int Cluster;    // UTF-8 byte offset of the cluster start
};
```

Why a `Codepoint` and `Cluster` if they're "informational"? Because the *sizing* and
*wrap* logic sometimes needs to know which glyphs belong to which logical bytes
(section 7), and because it is useful for debugging.

The design keeps ImGui's own code **generic**: the shaped path checks
`font->OwnerAtlas->FontShaper != NULL` at runtime, and if no shaper is attached it
takes the original LTR path. When `IMGUI_ENABLE_RTL` is compiled in, the default
shaper is auto-attached in `ImFontAtlasBuildInit()`.

This is the single most important decision. It means:

- LTR text is never changed (there is no RTL code in the hot LTR path other than one
  pointer check).
- The shaping engine can be swapped (libraqm today, something else tomorrow).

---

## 4. Foundational requirement #1: load a glyph by glyph index, not codepoint

### The problem

ImGui's glyph cache (`ImFontBaked`) is indexed by **Unicode code point**:
`FindGlyph(codepoint)` → `ImFontGlyph`. Every glyph is keyed by the code point that
produced it, and its raster is fetched through the loader's
`FontBakedLoadGlyph(codepoint)`.

Shaping breaks that assumption. A shaped run contains glyphs like:

- a **contextual form** (e.g. U+0647 HEH has four forms),
- a **ligature** (`ل`+`ا` → the lam-alef ligature, one glyph for two code points),
- a **decomposed mark** (one code point may produce several glyphs).

None of these has a code point that uniquely identifies it in the glyph cache.

### The fix

Add a parallel, glyph-index-keyed lookup:

```cpp
struct ImFontGlyph {
    ...
    unsigned int GlyphId;   // font glyph index
    ...
};

struct ImFontBaked {
    ...
    ImVector<ImU32> GlyphIdLookup;   // sparse: (glyph index << 4 | source) -> (glyph_idx + 1)
    ImFontGlyph* FindGlyphByIndex(unsigned int glyph_index, int source_idx);
};
```

`GlyphIdLookup` is the analogue of the existing `IndexLookup` (codepoint → glyph), but
keyed by the font's internal glyph index. `FindGlyphByIndex` returns `NULL` (not a
fallback glyph) for a missing index, because a shaping backend should be able to
decide on its own what to do with a missing glyph.

A glyph index is only unique **within a font file**. A merged font (e.g. a Latin base with
an Arabic font merged in) can reuse the same glyph index in two sources, so the lookup key
folds the 4-bit `ImFontGlyph::SourceIdx` into the index (`glyph_id << 4 | source_idx`) and
`FindGlyphByIndex` takes the source. The shaper records which source produced each glyph
(`ImShapedGlyph::SourceIdx`, derived from `raqm_glyph_t.ftface`), so the renderer resolves
each shaped glyph against the correct source.

Both loaders must be able to **rasterize by glyph index**:

- `ImFontLoader::FontBakedLoadGlyphByIndex(...)` is the new loader hook.
- `imgui_draw.cpp` implements it for **stb_truetype** (`ImGui_ImplStbTrueType_FontBakedLoadGlyphByIndex`).
- `misc/freetype/imgui_freetype.cpp` implements it for **FreeType** (`ImGui_ImplFreeType_FontBakedLoadGlyphByIndex`).

Without this, the shaped glyphs could never be pulled into the atlas, and RTL
rendering would be impossible at all. This is the very first thing that had to exist.

---

## 5. Foundational requirement #2: the shaper's metrics must match the glyph loader's scale

### The problem (the "Noto Naskh Arabic has gaps" bug)

The shaper computes **advances and offsets** (how wide each glyph is, where a mark
sits). The glyph loader rasterizes **bitmaps**. For the two to agree, the FreeType
size the shaper uses must be *exactly* the same FreeType size the loader uses.

ImGui's FreeType loader requests a size with:

```c
FT_Request_Size(face, { FT_SIZE_REQUEST_TYPE_REAL_DIM, 0, size * 64 * density, 0, 0 });
```

(`REAL_DIM` is important: it sizes by the real ascender+descender, not the em square.
For a font like Noto Naskh Arabic, whose real height differs from its em size, using
the wrong request type makes the shaper's advances disagree with the bitmaps — and
letters draw with visible gaps.)

The shaper must therefore do the *identical* request (`ImGuiRTL_EnsureFacesSized`),
including the `density` factor (`baked->RasterizerDensity * src->RasterizerDensity`),
otherwise every glyph is subtly the wrong scale.

### The Y-direction sign flip

HarfBuzz/FreeType use a Y-**up** coordinate system; ImGui uses Y-**down**. So the
shaper negates `y_advance` and `y_offset` when converting to `ImShapedGlyph`:

```cpp
out.YAdvance = -(float)raqm_glyphs[i].y_advance / 64.0f / density;
out.YOffset  = -(float)raqm_glyphs[i].y_offset  / 64.0f / density;
```

(`/64` converts FreeType's 26.6 fixed point to pixels.)

These two details — matching `FT_Request_Size` exactly, and the Y flip — are the kind
of thing that costs hours of "why is my Arabic slightly wrong?" debugging. They are
documented here so the next person doesn't re-derive them.

### A second, subtler cause of gaps: hinting breaks cursive joins

Even with the size matched exactly, Arabic can still draw with *tiny* gaps between
letters. That residual gap is not a scale bug — it is FreeType hinting.

ImGui's FreeType loader rasterizes bitmaps with **full hinting** by default
(`FT_LOAD_TARGET_NORMAL`). Full hinting grid-fits each glyph's horizontal edges
independently, which breaks the horizontal connections that cursive Arabic depends on.
The shaper, by contrast, reads metrics through HarfBuzz from the *unhinted* outline
(hb-ft loads glyphs with `FT_LOAD_NO_HINTING` by default). So the shaper's advances
describe the unhinted outline while the bitmaps show the hinted outline — they disagree
by a fraction of a pixel, and that disagreement appears as tiny gaps (or overlaps).

The fix is to rasterize Arabic with **light hinting**
(`ImFontConfig::FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting`), which snaps
glyphs only vertically. Horizontal metrics stay unhinted, the advances agree with the
bitmaps, and the joins stay closed. The demo loads Noto Naskh Arabic this way.

---

## 6. The shaping pipeline

The actual shaper is `imgui_rtl.cpp`, built on **libraqm**, which combines:

- **HarfBuzz** — OpenType shaping (contextual forms, ligatures, mark positioning),
- **SheenBidi** — the Unicode Bidirectional Algorithm (logical → visual order),
- **FreeType** — font data access.

`ImGuiRTL_ShapeText`:

1. **ASCII fast-path** — if every byte is `< 0x80`, return `false` immediately
   ("this text needs no shaping"). The caller falls back to the LTR codepoint path.
   Pure-Latin text therefore costs nothing.

2. Prepares a reusable `raqm_t` object (`raqm_clear_contents` + set text + set face +
   set paragraph direction + `raqm_layout`).

3. Extracts `raqm_get_glyphs` — glyphs in **visual order**, each with `index`,
   `x_advance`, `x_offset`, `y_offset`, `cluster`.

4. Converts to `ImShapedGlyph` and returns.

The paragraph direction comes from `ImGuiRTL::SetDirection()` (default: auto-detect
via UBA first-strong). It is also exposed through `ImGuiRTL::IsRtl(text)`.

### The render/size side

`ImFont::RenderText()` and `ImFontCalcTextSizeEx()` both check the shaper at the top:

```cpp
const ImFontShaper* shaper = OwnerAtlas->FontShaper;
if (shaper != NULL && !ImFontTextIsPureAscii(text_begin, text_end))
{
    ImFontRenderTextShaped(...);   // or ImFontCalcTextSizeShaped(...)
    return;
}
// ... original LTR path ...
```

The `!ImFontTextIsPureAscii(...)` guard is a later optimization: it routes pure-ASCII
text to the *original* LTR path even when a shaper is attached, so English is
byte-for-byte stock behavior and pays no shaper overhead (section 12).

The shaped render path (`ImFontRenderTextShaped`) walks the text:

- split into logical lines at `\n`,
- (if wrapping) split each logical line into visual lines at word boundaries
  (`ImFontShapedWrapLine`, section 7),
- render each visual line (`ImFontShapedRenderLine`).

`ImFontShapedRenderLine` shapes the visual line and draws each glyph using
`baked->FindGlyphByIndex(sg.GlyphId)` plus its `XOffset`/`YOffset`:

```cpp
float px = x + sg.XOffset * scale;
float py = y + sg.YOffset * scale;
PrimRectUV(ImVec2(px + glyph->X0*scale, py + glyph->Y0*scale), ...);
x += sg.XAdvance * scale;
```

Crucially, the glyphs are already in **visual order**, so drawing them left-to-right
from the pen position is exactly correct for RTL too — the shaper did the reordering,
the renderer stays dumb.

### Merged fonts (multiple sources)

A font can be built from several sources (e.g. a Latin font with an Arabic font merged in).
The shaper opens **one `FT_Face` per source** (cached in a flat pool so the entry stays a POD —
same reason as the shaping cache in §10), sizes each exactly like the loader (per-source
`ExtraSizeScale`, `RasterizerDensity`, and the merge `SizePixels` ratio), then assigns every
character to the first source whose face actually contains it (`FT_Get_Char_Index`), honoring
`GlyphExcludeRanges`, via `raqm_set_freetype_face_range()` (UTF-8 byte offsets).
`raqm_glyph_t.ftface` identifies which face produced each glyph, so advances are converted with
the correct per-source density. This is what lets an Arabic word inside an otherwise-Latin font
fall back to the merged source and still shape correctly.

---

## 7. Word wrap: breaking at word boundaries, not characters

ImGui's LTR word-wrap (`ImFontCalcWordWrapPositionEx`) measures *code point* advances
and breaks at spaces/punctuation. For Arabic this is wrong on two counts:

- Arabic advances are only correct **after shaping** (a joined word is narrower than
  the sum of isolated letters),
- and you must never break a word mid-letter-join.

So the shaped path uses its own wrap (`ImFontShapedWrapLine`):

1. Shape the remaining logical text.
2. Compute the width of logical prefixes using the `Cluster` field
   (a glyph belongs to the prefix `[p, p+n)` iff `glyph.Cluster < n`).
3. Find the largest word boundary (after a space) whose prefix fits `wrap_width`.
4. If no boundary fits, hard-break inside the word at a glyph/cluster boundary.
5. Skip trailing spaces and emit the next visual line start.

It falls back to the LTR wrap when `ShapeText` returns `false` (pure ASCII).

The same wrap is used by:

- `ImFontCalcTextSizeShaped` (sizing), and
- `InputTextLineIndexBuild` (the line index the caret/selection use).

That last one matters: the caret and selection operate on a **line index** built from
the same wrapping as the display, otherwise they would land on lines that don't match
what is on screen. This is the subtle part of "word wrap works for editing".

### The `get_line_end` "-1" convention

`ImGuiTextIndex::get_line_end(n)` returns `Offsets[n+1] - 1` (the char *before* the
next line start). This exists so a hard-newline line excludes its trailing `\n`. For
wrapped lines it means the line's content may include trailing spaces in some
edge cases — a pre-existing ImGui quirk, not something the RTL code introduced, but
worth knowing when a caret/selection looks off by one space at a wrap point.

---

## 8. Text editing: mapping logical bytes ↔ visual pixels

Rendering is only half the story. `InputText` needs to know, for a given **logical
byte offset** (where the caret is), the **visual x position** (where to draw it), and
vice-versa for mouse clicks.

These are the shaper's optional `IndexToXOffset` / `XOffsetToIndex`:

- `IndexToXOffset(byte_offset)` → x.
- `XOffsetToIndex(x)` → byte offset.

Both are implemented on top of a single shared **caret-stop** model (see below), not by two
independent raqm calls. That is the key to keeping caret drawing, click hit-testing, and
arrow-key movement all mutually consistent.

`IndexToXOffset` is called once per frame while the caret blinks, so it must be cheap. The
caret stops are rebuilt per call, but they come from the **cached** shaped glyphs (a cache
hit is a few microseconds, see §10), and the stop list itself is a single O(glyphs) walk —
no HarfBuzz re-layout. `XOffsetToIndex` previously called `raqm_position_to_index`, which
re-ran `raqm_layout()` on every call (mouse click / drag); moving it onto the cached
caret-stop list removed that per-frame re-shape entirely.

They are used in `imgui_widgets.cpp` for:

- **Caret position** — `InputTextLineIndexGetPosOffset` maps the caret's byte offset to
  x within its visual line.
- **Mouse click/drag** — `InputTextShapedClick` maps mouse x to a byte index (building a
  shaper-aware line index for multi-line).
- **Selection highlight** — maps the selected byte range to a visual x range per line.

### The unified caret model (and the dual-caret problem)

The caret logic went through three stages, and the final one is what's in the code now:

1. **Independent mappings.** `IndexToXOffset` used a prefix-width sum for single-direction
   runs and `raqm_index_to_position` for bidi (which returns the caret's *trailing* position,
   i.e. "after the previous character"); `XOffsetToIndex` used `raqm_position_to_index`;
   `MoveCaretVisual` walked the shaped glyphs. Three different models that *disagreed* in
   mixed text, so the drawn caret x didn't match the last arrow-key step.

2. **One caret-stop list (leading edges only).** `IndexToXOffset`, `XOffsetToIndex` and
   `MoveCaretVisual` were unified on a single list of caret stops — one per distinct glyph
   cluster, placed at the character's *leading* edge (left edge for LTR, right edge for
   RTL), plus the text end at the paragraph's trailing edge. This made everything mutually
   consistent, but it was **wrong at LTR→RTL run boundaries**: stepping right from an
   English word into an Arabic word landed on the *second* Arabic glyph (it skipped the
   first visual glyph of the Arabic word).

3. **Dual carets + affinity.** A single logical byte offset at a run boundary legitimately
   maps to *two* visual positions (a "dual caret"). The stop list now emits **both edges** of
   every distinct cluster, not just the leading edge:

   - LEADING  (`"before" the character`): left edge for LTR, right edge for RTL. Byte = cluster.
   - TRAILING (`"after" the character`): right edge for LTR, left edge for RTL. Byte = next cluster.

   Within a same-direction run, one character's leading edge and its logical neighbour's
   trailing edge coincide (same byte, same x), so they merge into one unambiguous stop. At a
   run boundary they do NOT coincide, so both survive as the dual caret. The paragraph's two
   edge stops (start/end) are emitted in visual order: the left-edge stop first, the right-edge
   stop last, so the rightward walk starts at the visual left edge and click hit-testing at the
   trailing edge resolves to the paragraph end.

   Each stop is tagged with an **affinity** (0 = trailing, 1 = leading, -1 = unambiguous), and
   `IndexToXOffset` / `MoveCaretVisual` take an `affinity` argument to pick the right occurrence
   (exact match first, then the unambiguous stop, then any). `ImGuiInputTextState::StbCaretAffinity`
   remembers the current side, updated on every arrow-key step, so stepping into and back out of
   a dual-caret region stays consistent.

   Emitting both edges is what makes the caret walk **reversible**: stepping right through a
   mixed line and then stepping left produces the exact reverse sequence, so neither LTR→RTL nor
   RTL→LTR direction skips a glyph. The self-test asserts this reversibility explicitly.

Three concrete consequences this fixes:

- **No more "skip the first Arabic glyph"** when stepping right from English into Arabic, and
  symmetrically no more "skip the first Latin word" when stepping left from Arabic into English.
- **The caret stops at the visual edge.** Previously, reaching the end of RTL text and pressing
  the arrow that should do nothing fell back to stb's *logical* Left/Right, which moved the
  caret back the other way. The movement handler now treats "at the edge, no movement" as
  handled, so it does not fall back.
- **Click hit-testing at the trailing edge** returns the paragraph end, not the "before the last
  run" position, because the paragraph-end stop is the trailing-most stop at that x.

A combining mark still shares its base's cluster and is therefore not a separate caret stop
(`IndexToXOffset` on a mark byte snaps to the next logical boundary), and a partially-selected
ligature remains the same accepted approximation.


### Caret movement by grapheme cluster

Arabic harakat (fatha, kasra, damma, shadda, sukun, tanween…) are **combining marks**:
they attach to the preceding base letter and have no independent visual existence. The
stock caret steps one *codepoint* at a time, so pressing Left/Right would stop between a
letter and its mark — a caret position that means nothing visually.

The shaped path therefore steps the caret by **caret stop** rather than by codepoint.
`ImGuiRTL_BuildCaretStops()` (in `imgui_rtl.cpp`) walks the visual-order glyph list and emits
one stop per distinct *cluster* (a base letter and its harakat share a cluster), plus the
paragraph edges, so stepping never lands between a letter and its mark and never
oscillates at a bidi run boundary. `InputTextMoveCursorVisual()` (in `imgui_widgets.cpp`)
drives it for Left/Right (and Shift+Left/Right); mouse clicks go through
`InputTextShapedClick()` → `ImGuiRTL_XOffsetToIndex()`, which resolves the nearest caret
stop. There is no Unicode-range table involved: marks are skipped because the shaper gives
them their base's cluster.

This is *not* scoped to RTL only: any text that the shaper accepts (i.e. anything needing
bidi/complex/mark handling) uses this caret model, while pure-ASCII text and simple LTR
scripts are routed to the stock codepoint path (see section 13) and keep the stock
behavior byte-for-byte.

### Selection highlight is not a single span

The first version of the selection highlight mapped the two endpoints of the selection
with `IndexToXOffset` and drew one rectangle from `min` to `max`. That is correct for a
single-direction run but **wrong for mixed bidi text**: the two logical endpoints do not
bound the visual selection.

Example (LTR paragraph, `"English عربي 1234"`): selecting the logical range `"عربي 1234"`
[8, 21) selects the Arabic word *and* the trailing digits. Visually the digits (`EN`,
European numbers) are reordered to the *left* of the Arabic run, so the selected glyphs
span `["1234", "عربي"]`. But `IndexToXOffset(8)` returns a caret *inside* that span (at a
run boundary), so `min(endpoint1, endpoint2)`..`max(..)` covers only part of the selection
and highlights text that was never selected.

The fix is `ImFontShaper::GetSelectionSegments`: walk the shaped glyphs (already in
visual order) and emit one `[x0, x1]` interval per contiguous run of glyphs whose `Cluster`
lies inside `[sel_begin, sel_end)`. The highlight is then drawn as one rectangle per
interval. This is correct by construction: it highlights exactly the selected glyphs,
however the bidi algorithm reordered them.

`Cluster` is a UTF-8 byte offset (raqm converts HarfBuzz's UTF-32 clusters back to UTF-8
in `raqm_get_glyphs`), so the membership test `sel_begin <= cluster < sel_end` is a direct
byte-range comparison. The only remaining approximation is a partially-selected ligature
(several characters collapse to one cluster) — the same caret-snapping limitation above.

---

## 9. Arrow keys and "visual" direction

ImGui's stock arrows are logical: `Left` = previous byte, `Right` = next byte. For RTL
that is backwards — pressing `Left` should move the caret *visually left*, which is
the **next** logical character.

The fix (in `InputTextEx`) detects `ImGuiRTL::IsRtl` and swaps the **relative, visual** keys, but leaves the **absolute** ones alone:

| key            | LTR behavior              | RTL behavior              |
|----------------|---------------------------|---------------------------|
| Left           | prev char (`K_LEFT`)      | next char (`K_RIGHT`)     |
| Right          | next char (`K_RIGHT`)     | prev char (`K_LEFT`)      |
| Ctrl+Left/Right| prev/next word            | swapped                   |
| Home/End       | line start/end            | **not swapped**           |
| Ctrl+Home/End  | text start/end            | **not swapped**           |

Why the split:

- `Left`/`Right` (and word variants) are **relative and visual**: "move left" means
  "move in the visual-left direction", and for RTL the visual-left direction is the
  *next* logical character — so they must be swapped.
- `Home`/`End` and `Ctrl+Home`/`Ctrl+End` are **absolute byte positions** (first/last
  byte of the line, first/last byte of the buffer). Those are not directional: for RTL,
  byte 0 *already* lands at the visual right edge (the caret is drawn there by the
  shaper's own `IndexToXOffset`), so no swap is needed — swapping them is exactly the
  bug where `Home` jumps to the left instead of the right.

Direction is detected **per logical line** for multi-line inputs (the `\n`-delimited line
containing the caret), not per whole buffer. This matters for a buffer that mixes scripts:
e.g. an English first line followed by Arabic lines must give the English line LTR arrows
and the Arabic lines RTL arrows. Detecting from the whole buffer's first strong character
would apply the first line's direction to every line.

Within a single line, **character** movement does not use a direction decision at all — it
steps in *visual* order via `ImFontShaper::MoveCaretVisual`. The shaped glyphs are already in
visual order, so the sequence of distinct cluster starts (plus the text end) is exactly the list
of visual caret positions; stepping along it is bidi-correct by construction and cannot oscillate
at run boundaries (the naive "swap Left/Right by the local run direction" approach dead-locks:
at an LTR/RTL boundary the direction flips and the caret bounces back).

### The end-of-text caret is direction-dependent (a deceptively simple bug)

The visual caret sequence is built as **distinct cluster starts in glyph order, plus `text_len`**
(the caret after the last logical character). The trap is *where* `text_len` goes:

- For **LTR**, `text_len` is the **rightmost** caret (x = total width) → it is **appended**.
- For **RTL**, `text_len` is the **leftmost** caret (x = 0) → it must be **prepended**.

The first version always appended `text_len`. That is correct for LTR but wrong for RTL: at the
end of an Arabic line, "move right" computed `pos + 1` past the end of the sequence and returned
"no movement", so the caret could not move back toward the start — a deadlock that only appears
at the end of a pure-RTL string (clicking at the end, then trying to move right). The fix picks
the insertion side from `base_dir`: prepend for RTL, append for LTR. This is the kind of bug that
survives every bidi case except the last character of a right-to-left line.

**Word** movement (`Ctrl+Left/Right`) still uses a direction decision:
`ImFontShaper::DirectionAt` (`raqm_get_direction_at_index`) gives the run direction at the caret,
with the per-line result as the fallback for when the shaper can't answer (e.g. pure ASCII).

---

## 10. The shaping cache (the double-edged sword)

### Why a cache at all

Shaping is expensive (HarfBuzz bidi + shaping ≈ tens of microseconds per run). The
same text range gets shaped multiple times: once while **wrapping**, once while
**measuring**, once while **rendering**, and then again across frames for static
labels. A cache deduplicates this.

### The design (and its evolution)

The cache lives in `imgui_rtl.cpp`, keyed on:

```
(BakedId, TextBegin, TextLen, TextHash, Direction)
```

- **`BakedId`** (`ImFontBaked::BakedId`) is a stable hash of (font, size, density).
  Keying on the `baked` *pointer* would be subtly wrong: an atlas rebuild can reuse a
  heap address for a new baked object, producing a stale hit. `BakedId` avoids that.
- **`TextBegin` + `TextLen` + `TextHash`** identify the text content. `TextHash` is an
  FNV-1a hash of the bytes and is the **authority**: if a buffer is edited in place
  (same pointer, same length, new content), the hash changes → miss. This is the
  content-keying idea (borrowed from `rtl_text.hpp`, a simple reshaping with no postioning code which uses SheenBidi only).
- **`Direction`** is the paragraph direction, because `SetDirection()` changes the
  shaping result.

Safety properties:

- **Persistent** (not frame-scoped) so static labels aren't re-shaped every frame.
- **Content-verified** (the FNV hash) so in-place edits can't go stale.
- **Bounded** (`IMGUI_RTL_SHAPE_CACHE_SIZE = 2048`), cleared wholesale on overflow —
  no LRU/eviction subtleties, no unbounded growth. See the sizing note in `imgui_rtl.cpp`; beyond ~2048 a hash map/LRU would beat a larger array.
- **Owned flat glyph pool**: glyphs live in a single `ImVector<ImShapedGlyph>`
  (`GShapeCacheGlyphs`); each entry is a POD referencing `[GlyphOffset, GlyphCount)`.

### The bug this design exists to prevent

The first version stored an `ImVector<ImShapedGlyph>` **inside** each cache entry.
That crashed the self-test with a segfault.

Why: ImGui's `ImVector` grows by `memcpy`-ing its backing array, which is only valid
for **trivially-copyable** types. A struct containing a vector is not trivially
copyable — the inner vector's `Data` pointer gets duplicated, and when the old and new
copies are both freed you get a double-free.

The flat pool fixes it: entries are plain POD (memcpy-safe), and glyphs live in a
separate, flat, append-only pool. This is the single most important "gotcha" in the
whole project, and it is the kind of bug that only shows up under load.

`ImGuiRTL::ClearShapeCache()` exists for testing and to reclaim memory.

---

## 11. Right-alignment (block-level vs per-line)

### The idea

Detecting "is this RTL?" is already done (the shaper resolves the base direction via
UBA first-strong; `ImGuiRTL::IsRtl`). What remains is positioning: RTL text should be
**flush-right** within its container, not left-anchored.

The mechanism is a one-liner given the container width and text width:

```cpp
if (base_direction == RTL && container_width > text_width)
    x += container_width - text_width;
```

wrapped in `ImGuiRTL::AlignTextRight(pos_x, max_x, text, text_end, text_width)`.

### Two levels of alignment

- **Block-level** (single-line): shift the whole block so its right edge is at the
  container's right edge. Applied to `Text()` (unwrapped), `Selectable()`, and
  single-line `InputText`.

- **Per-line flush-right** (wrapped): each visual RTL line is shifted independently
  inside the shaped render path (`ImFontShapedRenderLine`), so the *short last line* of
  a wrapped paragraph also sits at the right edge, not the left. This is the
  difference between "correct" and "text indented toward the right but left-aligned".

### The ordering bug (and why order matters)

Right-aligning `InputText` shifts `draw_pos.x`. The caret, selection rect, and text are
all drawn at `draw_pos.x + ...`. So the shift must be applied **before** any of them
are drawn; if it is applied after the selection rect is computed, the selection stays
at the old left position (the exact bug that was reported and fixed).

Separately, mouse hit-testing uses a *different* coordinate (`mouse_x` relative to the
frame origin), so it must be **un-shifted** by the same amount, otherwise you have to
click the empty left side to select the text that is drawn on the right.

For wrapped text, the per-line shift is different for each line, so mouse/selection/caret
all consult the same `InputTextRtlLineShift()` helper to stay consistent.

### Remaining gap

**Non-wrapped** `InputTextMultiline` (no `ImGuiInputTextFlags_WordWrap`) is still
block-aligned, not per-line, because the render path has no container width to flush
against when `wrap_width == 0`. Fixing that needs plumbing an explicit alignment width
through `RenderText`.

---

## 12. Performance (the ASCII fast-path)

Shaping must not slow down the overwhelmingly common case (Latin text). Two layers:

1. `ShapeText` itself returns `false` for pure-ASCII (no raqm work at all).
2. `ImFont::RenderText` / `ImFontCalcTextSizeEx` skip the shaped path entirely for
   pure ASCII via `ImFontTextIsPureAscii`, so even the shaped-path function-call and
   fallback-loop overhead is avoided.

The result (measured by `examples/rtl_demo/benchmark.cpp`): English ≈ 1.0x vs stock;
Arabic ≈ 100–300x (the inherent HarfBuzz cost). The cache-hit cost is ~200–500x
cheaper than a shaping miss.

---

## 13. Compile-time gating and "no deps when disabled"

The goal: without `IMGUI_ENABLE_RTL`, the build is stock ImGui with **no** libraqm /
HarfBuzz / SheenBidi / FreeType dependency.

How it's achieved:

- The `#include "misc/rtl/imgui_rtl.h"` and the `ImGuiRTL::GetShaper()` auto-attach are
  both inside `#ifdef IMGUI_ENABLE_RTL`.
- The interface types (`ImFontShaper`, `ImShapedGlyph`, `GlyphId`, `GlyphIdLookup`,
  `FindGlyphByIndex`, `FontBakedLoadGlyphByIndex`) are **always compiled** but are pure
  C++ definitions with zero external deps — they reference nothing from libraqm etc.
- The shaped render/size path is always compiled, but is inert when
  `FontShaper == NULL` (which is always the case when the flag is off).
- The LTR path is unchanged except for a single `if (shaper != NULL)` pointer check, plus
  the always-compiled glyph-id bookkeeping (`ImFontGlyph::GlyphId`,
  `ImFontBaked::GlyphIdLookup`, `ImFontAtlasBakedUpdateGlyphIdLookup()`), which costs one
  sparse-vector store per loaded glyph and changes no output. With a shaper attached,
  non-ASCII text still takes the LTR path whenever `TextNeedsShaping()` says it needs no
  shaping (pure ASCII, precomposed Latin/Greek/Cyrillic, CJK, Kana, Hangul syllables, …),
  so e.g. accented Latin text has no measurable overhead.

Verified by compiling `imgui.cpp + imgui_draw.cpp + imgui_widgets.cpp + imgui_tables.cpp`
with no flags and the bundled stb_truetype loader — it builds, links, and runs with no
RTL deps.

The one flag to keep in mind: `IMGUI_ENABLE_FREETYPE` gates the FreeType glyph loader
(independently of RTL). The RTL shaper *itself* needs FreeType (for font data), but that
is a dependency of the add-on, not of the core.

---

## 14. Known limitations and likely bugs

Be honest about what is not perfect:

- **Merged fonts / fallback shaping** — supported: one `FT_Face` per source, characters
  assigned to the first source whose face contains the glyph via
  `raqm_set_freetype_face_range()`, and per-glyph advance conversion using
  `raqm_glyph_t.ftface`. The caret/selection math reads the already per-source
  density-corrected glyph advances, so merged sources with differing `RasterizerDensity`
  are handled correctly (no first-source density assumption remains).
- **Double/triple-click word/line selection** still uses stb's logical-order word
  boundaries, not shaped-aware ones. Word boundaries are space-based so it is usually
  acceptable, but not perfect.
- **Non-wrapped multi-line per-line flush-right** — see section 11.
- **Caret inside ligatures** — no visual position exists inside `لا`; the caret snaps
  to one side. Inherent.
- **Dual-caret "invisible" arrow step** — at a run boundary (e.g. the far-right edge of
  `"English عربي"`), two *different* logical positions share one visual x: "before ع" (insert
  before the last character) and "end of text" (append). With visual arrow movement the caret
  steps through both, so pressing Right one extra time at the visual end appears not to move
  (the logical position changes, the drawn x doesn't). This is correct bidi dual-caret behavior,
  not a bug: both insertion points are real and reachable, and the walk stays reversible. A
  naive attempt to collapse the two into one makes the *other* position unreachable (or skips a
  glyph), which is worse.
- **`draw_scroll.y` in InputText** — the vertical scroll is folded into `draw_pos.y`,
  and `draw_scroll.y` appears uninitialized in some code paths. Pre-existing stock
  behavior, not introduced here, but worth investigating if caret/selection y ever
  looks wrong in the inactive multiline case.
- **The FNV-1a hash and `BakedId` are 32/64-bit** — collision probability is
  negligible, and the consequence would be a visual glitch, never a crash.
- **Thread-safety** — the add-on uses global state (single-threaded, like ImGui
  itself). The cache is not thread-local.
- **Page Up/Down** still route through stb's LTR column mapping.

The author's honest assessment: the code works for the tested cases, but complex text
is a deep rabbit hole and there are almost certainly latent bugs in the edges above.

---

## 15. The journey, in the order it actually happened

For anyone retracing this, the order mattered because each step unlocked the next:

1. **The interface** (`ImFontShaper`, `ImShapedGlyph`, `SetFontShaper`) — the
   skeleton everything else hangs off.

2. **Glyph-index loading** (`GlyphId`, `FindGlyphByIndex`, `FontBakedLoadGlyphByIndex`
   in both loaders) — without this, no shaped glyph could enter the atlas.

3. **The shaper itself** (`imgui_rtl.cpp`, libraqm) — first correct shaping output.

4. **FreeType size matching** — fixed the "Noto Naskh Arabic gaps" (the metrics/bitmap
   scale mismatch) and the Y sign flip.

5. **Render + size paths** (`ImFontRenderTextShaped`, `ImFontCalcTextSizeShaped`,
   `ImFontShapedMeasureLine`, `ImFontShapedRenderLine`).

6. **Word wrap** (`ImFontShapedWrapLine`) and the shaper-aware line index — this is
   what made wrapped text and later editing line up.

7. **Editing** — `IndexToXOffset`/`XOffsetToIndex`, caret position, mouse click/drag,
   selection highlight, then multi-line and Up/Down movement.

8. **The ASCII fast-path** (`ImFontTextIsPureAscii`) — made Latin zero-cost and
   byte-identical to stock.

9. **The cache** — dedup repeated shaping; hit and fixed the nested-`ImVector`
   double-free by moving to the flat glyph pool; later made it persistent and
   content-keyed (borrowing the content-keyed idea from `rtl_text.hpp`).

10. **Right-alignment** — `AlignTextRight` for single-line, then per-line flush-right
    for wrapped text, then fixing the mouse/selection/caret to track the shift.

11. **Arrow-key direction** — swapping Left/Right (and word variants) for RTL, while Home/End stay absolute.

12. **Mixed script selecting and highlight** — a logical selection maps to several
    visual runs, so the highlight is drawn per visual segment (`GetSelectionSegments`),
    not as a single `min..max` span.

The lesson of the ordering: you cannot bolt alignment or editing or caching onto a
shaper until the *glyph-index* and *metric-scale* foundations are solid, and each
editing feature (caret, selection, mouse, arrows) must use the **same** coordinate
mapping or they drift apart.

---

## 15. Post-review fixes

A follow-up review with targeted probes found and fixed the following. Kept here so the
reasoning behind each guard is not lost:

- **Password fields**: the shaped path looked glyphs up by index and never consulted the
  `FallbackGlyphIndex`/`IndexLookup` substitution that `PushPasswordFont()` installs, so a
  non-ASCII password rendered its real glyphs (or nothing). The shaped measure/render paths
  now substitute the fallback glyph *and* its advance while `ImFontFlags_NoLoadGlyphs` is
  set (`ImFontShapedIsMasked()`/`ImFontShapedGetGlyph()` in `imgui_draw.cpp`).
- **Glyph-index lookup**: `FindGlyphByIndex()` cached `NOT_FOUND` even when the failure was
  transient (locked atlas, or `NoLoadGlyphs`), permanently losing those glyphs; and it used
  `glyph_index << 4` as a sparse key with no bound, so a bogus index could request a
  multi-GB allocation. It now validates the index (`<= 0xFFFF`, source `<= 15`) and only
  caches definitive misses.
- **Font lifetime**: the FT_Face cache was keyed on `ImFont*` and never invalidated, so
  removing a font and adding another one that reused the address crashed inside
  `FT_Get_Char_Index()`. `ImFontShaper::FontDestroyed()` is now called both from `~ImFont()`
  and from `ImFontAtlas::RemoveFont()` (which clears `OwnerAtlas` before deleting the font),
  the entry is validated against `FontId`/atlas/source data, and the shape cache is flushed
  as well (its `BakedId` keys are only unique per atlas).
- **Malformed UTF-8**: HarfBuzz can report cluster offsets past the end of the text for lone
  continuation bytes; clusters are now clamped to the text range and the caret-stop builder
  rejects malformed clusters, so caret/selection positions can never leave the buffer.
- **Bidi class-B characters**: libraqm/SheenBidi refuses to lay out text containing `'\r'`,
  U+0085 (NEL) or U+2029 (PS) — `raqm_layout()` failed and returned no glyphs, so a CRLF line
  containing Arabic silently rendered unshaped. The shaper now shapes a copy with those
  characters removed and remaps the clusters back to the original byte offsets (the core splits
  lines on `'\n'` before shaping, and a direct multi-line call is truncated at the first `'\n'`).
- **Password geometry**: masking substituted the fallback glyph/advance only in the core, so the
  shaper's caret stops, selection segments and line metrics still used the real advances and the
  caret drifted off the asterisks. The advance rule is shared through
  `ImFontShapedGetGlyphAdvance()`.
- **Bidi caret affinity** (`ImGuiInputTextState::StbCaretAffinity`) is reset when the text is
  inserted or deleted, so a stale dual-caret side can't be reused after an edit.
- **Word wrap**: a line whose first cluster was wider than the wrap width was cut at byte 1
  (mid-UTF-8). The hard break is now always at a complete character, and the wrap scan uses
  per-byte cumulative widths (O(1) prefix queries, no sorting) instead of re-summing glyphs
  per candidate.
- **Wrap flags**: `ImFontShapedWrapLine()` takes `ImDrawTextFlags` and its LTR fallback passes
  them to `ImFontCalcWordWrapPositionEx()`/`ImTextCalcWordWrapNextLineStart()`. Without this the
  line index (and therefore wrapped height, clicks, caret and Home/End) used different break
  points than the renderer, which uses `WrapKeepBlanks` for InputText. A wrap point followed only
  by blanks no longer emits a phantom empty visual line, and `'\r'` costs no width.
- **Home/End**: `STB_TEXTEDIT_MOVELINESTART/END` used the stock LTR wrap, which does not
  match the shaped visual lines. They now go through `InputTextCalcWordWrapPosition()`.
- **Control characters**: `	` (and other C0 controls) reached the shaper and got the
  `.notdef` advance; they are now measured/advanced exactly like the codepoint path.
- **Selection highlight**: the shaped branch silently dropped the "selected newline" bar and
  could fall back to a bogus logical span when the shaper returned no segments; segments are
  now capped with an explicit `-1` ("use the endpoint span") signal.
- **`Text()` right-alignment** no longer applies to inline text after `SameLine()`, and
  `Selectable` respects an explicit `SelectableTextAlign.x = -1`.
- **`SetFontShaper(NULL)`** is now sticky across atlas rebuilds (`ImFontAtlas::FontShaperExplicit`).

### Verification

`examples/rtl_demo/selftest.cpp` is the regression suite: it dumps the shaped output and runs
28 checks covering all of the above (glyph-index bounds and caching, password masking and its
caret geometry, transient failures, wrap boundaries/flags/phantom lines, class-B characters,
Home/End/Up/Down on shaped visual lines, `FontDestroyed` notifications, font removal + re-add,
plus a malformed/unusual-input sweep at 3 sizes x 4 wrap widths). It returns a non-zero exit code
when a check fails:

```sh
cmake -S examples/rtl_demo -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/rtl_selftest        # exit code 0 == all checks passed
./build/rtl_benchmark       # RTL vs LTR cost per case
```
