# Dear ImGui RTL / Arabic text shaping

Full bidirectional (RTL) and complex-script (Arabic, Farsi, …) text shaping for
Dear ImGui, integrated in the same style as the built-in FreeType glyph-loader
addon (`misc/freetype/`).

It makes existing `ImGui::Text()`, `Button()`, labels, etc. render RTL and
Arabic correctly — no per-call opt-in required — by shaping text at
layout/draw time.

## How it works

Text is shaped by **[libraqm](https://github.com/HOST-Oman/libraqm)**, which
combines:

- **HarfBuzz** (OpenType shaping: contextual Arabic forms, ligatures, mark positioning),
- **SheenBidi** (Unicode bidirectional algorithm: logical → visual reordering),
- **FreeType** (font data access).

The shaped result is a list of glyphs in *visual* order with advances/offsets.
ImGui's text layout and draw path consume this list instead of iterating raw
codepoints.

Only text that actually needs it is sent to the shaper. The shaper exposes a
`TextNeedsShaping()` pre-check which the core calls for non-ASCII text: pure ASCII,
precomposed Latin/Greek/Cyrillic, CJK, Kana and Hangul syllables take the stock codepoint
path, while anything that may need bidi reordering, contextual joining, mark positioning,
mirroring or composition (RTL scripts, Indic/SE-Asian scripts, combining marks, bidi
controls, astral codepoints, …) is shaped. This keeps e.g. accented Latin text at stock
speed (~0.1us instead of ~30us per unique string).

## Files

- `imgui_rtl.h` / `imgui_rtl.cpp` — the `ImFontShaper` implementation + helpers.
- `DESIGN.md` — the full design & implementation journey (the *why* behind every piece).
- `tools/` — standalone diagnostics (`render_dump`, `raqm_metrics_test`, `raqm_cursor_test`, `ft_size_test`).
- `examples/rtl_demo/CMakeLists.txt` — the build (imgui library + demo + self-test + benchmark).
- `examples/rtl_demo/benchmark.cpp` — headless RTL vs LTR text benchmark.

Core Dear ImGui changes (all gated; LTR behavior is unchanged when no shaper is set):

- `ImFontGlyph::GlyphId` + `ImFontBaked::FindGlyphByIndex()` — glyph-index lookup,
  required because shaped glyphs (contextual forms, ligatures) have no single codepoint.
- `ImFontLoader::FontBakedLoadGlyphByIndex()` — implemented for both the FreeType
  and stb_truetype loaders.
- `ImFontShaper` interface + `ImFontAtlas::SetFontShaper()`.
- Optional shaping path in `ImFontCalcTextSizeEx()` and `ImFont::RenderText()`.
  Pure-ASCII text is routed to the original LTR path even when a shaper is attached, so
  plain Latin text has no measurable overhead and behaves byte-for-byte like stock Dear ImGui.

## Build

The CMake project lives next to this demo, in `examples/rtl_demo/CMakeLists.txt`.
From the repository root (the `imgui/` directory):

```sh
cmake -S examples/rtl_demo -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

(or `cd examples/rtl_demo && cmake -B build && cmake --build build`, which keeps the build
directory inside the example folder).

HarfBuzz, SheenBidi and libraqm are used from a local checkout when present, and
otherwise fetched automatically with `FetchContent` (pinned versions:
HarfBuzz `14.4.0`, SheenBidi `v3.0.0`, libraqm `v0.11.0`). FreeType and GLFW are
system dependencies.

Options:

- `RTL_BUILD_DEMO` (default ON) — build the GLFW/OpenGL3 demo (`examples/rtl_demo/main.cpp`).
- `RTL_BUILD_SELFTEST` (default ON) — build the headless self-test (`examples/rtl_demo/selftest.cpp`).
- `RTL_BUILD_BENCHMARK` (default ON) — build the headless benchmark (`examples/rtl_demo/benchmark.cpp`).

## Usage

Option A — compile-time (like the FreeType addon):

```cpp
// imconfig.h
#define IMGUI_ENABLE_RTL 1
```

Then link `imgui_rtl.cpp` + libraqm (or just use the provided CMake). The shaper
is attached to every font atlas automatically.

Option B — runtime:

```cpp
io.Fonts->SetFontShaper(ImGuiRTL::GetShaper());
```

Helpers:

```cpp
ImGuiRTL::SetDirection(ImGuiRTL::Direction_RTL); // force RTL base direction
bool rtl = ImGuiRTL::IsRtl("سلام");
```

## Right-alignment

Text whose resolved base direction is RTL is automatically right-aligned:

- `Text()` (single-line) and `TextWrapped()`: unwrapped text right-aligns the block to the
  window's work-rect right edge; wrapped text is flushed right **per line** (so the short last
  line of a wrapped paragraph sits at the right edge).
- `Selectable()` right-aligns its label; `InputText()` (single- and multi-line) right-aligns when
  the text fits the frame, and word-wrapped multi-line text is flushed right per line.
- Mouse click/drag and selection in right-aligned RTL `InputText` are offset to match the drawn
  position.

LTR and pure-ASCII text are unaffected. Use `ImGuiRTL::AlignTextRight()` to do the same in
custom widgets.

Note: non-wrapped multi-line `InputTextMultiline` (without `ImGuiInputTextFlags_WordWrap`) is
right-aligned at the block level; full per-line flush-right there would require a wider
plumbing change.

## Benchmark

`rtl_benchmark` measures text sizing and rendering with the shaper attached (RTL)
vs. detached (FontShaper = NULL — the same LTR path used when `IMGUI_ENABLE_RTL`
is undefined at compile time):

```sh
./build/rtl_benchmark                      # uses the font path baked in by CMake
./build/rtl_benchmark path/to/font.ttf 20 20000
```

Pure-ASCII text is routed to the original LTR path (≈1.0x overhead), while Arabic
and mixed text pay the expected shaping cost (roughly 100–300x for a proper Arabic
font, inherent to HarfBuzz bidi+shaping).

## Shaping cache

The shaper keeps a small, persistent cache of shaped glyph runs, so the same text range is not
re-shaped multiple times (wrap + measure + render, measure-then-render) and static labels are
not re-shaped every frame. It is intentionally conservative:

- content-keyed: text is identified by (pointer, length, FNV-1a hash of the bytes) — the hash is
  the authority, so editing a buffer in place yields a miss (no stale glyphs);
- stable identity: font + size + density are keyed by `ImFontBaked::BakedId` (a stable hash),
  not by the baked pointer, so an atlas rebuild that reuses a heap address can't produce stale hits;
- direction-keyed: `SetDirection()` changes shaping, so the base direction is part of the key;
- bounded (2048 entries; on overflow the cache is dropped and rebuilt);
- stores an owned flat copy of the glyphs (entries are plain POD, so vector growth is safe).

Call `ImGuiRTL::ClearShapeCache()` to clear it manually. For repeated same-text calls the
cache-hit cost is ~200–500x cheaper than a full shaping miss (see `rtl_benchmark`).

## Fonts

The pipeline is font-agnostic; typography quality depends on the font. Use a proper
Arabic font with GSUB/GPOS and mark positioning (e.g. **Noto Naskh Arabic**) — it
renders fully connected cursive with correctly stacked harakat.

The two TTFs in `examples/rtl_demo/fonts/` (Noto Naskh Arabic, Roboto Medium) are included only so
the demo, self-test and benchmark run out of the box; both come from their upstream open-source
projects (see those projects for their licensing terms).

Two font-loading tips that matter for Arabic:

- **Use light hinting.** Full hinting (`FT_LOAD_TARGET_NORMAL`, the FreeType default)
  grid-fits each glyph's horizontal edges independently, which breaks the cursive joins
  and leaves tiny gaps between connected letters. Load the Arabic font with
  `ImFontConfig::FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting` (vertical-only
  hinting) to preserve the joins. The demo does this.
- **Bake the Arabic ranges.** Use a glyph-ranges builder that covers Arabic
  (0x0600–0x06FF, 0x0750–0x077F, 0x08A0–0x08FF, 0xFB50–0xFDFF, 0xFE70–0xFEFF,
  0x2000–0x206F), as `GetRtlGlyphRanges()` in the demo does.

## Behaviour notes

- **Password fields**: masked text is substituted by the fallback glyph in the shaped path
  too, exactly like the stock codepoint path, so non-ASCII passwords show `*`.
- **Control characters**: `\t` uses the tab glyph advance and other C0 controls are skipped,
  matching the codepoint path (a `\t` inside Arabic text no longer shifts by `.notdef`).
- **Word wrap** only breaks at complete UTF-8 characters, and word-wrapped RTL lines are
  flushed right per line.
- **Home/End** (and Cmd+Left/Right on macOS) use the same shaped wrap positions as the
  renderer, so they land on visual line boundaries.
- **`SetFontShaper(NULL)`** durably disables shaping; the compile-time default (when
  `IMGUI_ENABLE_RTL` is defined) is only attached if `SetFontShaper()` was never called.
- **Simple-script fast path**: text that cannot need bidi/contextual/mark processing bypasses
  the shaper. Call `ImGuiRTL::SetSimpleScriptFastPath(false)` to shape everything (e.g. to get
  HarfBuzz kerning for Latin text).
- **`ImGui::Text()` right-alignment** applies to text that starts a line; inline RTL text
  after `SameLine()` is left where it is.
- **Cursor/selection**: any text the shaper accepts uses shaper-driven caret stops, so
  arrow keys move visually and skip harakat; simple LTR text keeps the stock behaviour.

## Current limitations

- **Word wrap** (`TextWrapped` / `InputTextMultiline` with `wrap_width > 0`) is
  supported: each logical line is broken into visual lines at word boundaries and
  each visual line is shaped independently. Wrap decisions for a line are made from the
  shaping of that line's remainder, so shaping cost is proportional to the number of visual
  lines (cached across frames for text that doesn't change).
- **InputText RTL editing** (single-line and multi-line): the caret position, mouse
  click/drag, selection highlight, Up/Down caret movement, and Left/Right/Home/End arrow
  keys are all direction-aware (arrows move visually for RTL text), including word-wrapped
  multi-line inputs. Double/triple-click word/line selection still follows stb's logical-order
  behavior; text insertion/deletion are always correct since the buffer is in logical order.
- **Merged fonts**: supported. The shaper opens one FreeType face per source and assigns
  each character to the first source whose face contains it (matching the atlas merge order)
  via `raqm_set_freetype_face_range()`, converting advances with the glyph's own face density.
  Mouse hit-testing, the caret and the selection all use the same per-glyph advances as the
  renderer, so merged sources with different `RasterizerDensity` stay consistent.
- The addon caches one FreeType face per font *source* (merged fonts need one each) plus a
  persistent shaping cache. Both are released through `ImFontShaper::FontDestroyed()`, which
  the core calls when a font is destroyed (`RemoveFont()`, `ClearFonts()`, context teardown),
  so reloading fonts at runtime is safe. The shaper keeps one reusable `raqm_t` and its caches
  in globals: it is not thread-safe, so use one ImGui context per thread as usual.
