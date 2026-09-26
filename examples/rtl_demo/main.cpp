// Demo application: Dear ImGui + RTL/Arabic text shaping.
// GLFW + OpenGL3 backend.
//
// Loads Noto Naskh Arabic (a proper Arabic font with GSUB/GPOS + mark
// positioning) and exercises the RTL text-shaping integration added to imgui.

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>

#include <GLFW/glfw3.h> // Will include system OpenGL headers

#include "imgui_rtl.h"
#include "imgui_freetype.h"

// Default font location: baked in by the CMake build, so the binaries work when run from
// the build directory. Falls back to the sibling "fonts/" folder.
#ifndef RTL_DEMO_FONT_DIR
#define RTL_DEMO_FONT_DIR "../fonts"
#endif
#define RTL_DEMO_FONT_ARABIC RTL_DEMO_FONT_DIR "/NotoNaskhArabic.ttf"
#define RTL_DEMO_FONT_LATIN  RTL_DEMO_FONT_DIR "/Roboto-Medium.ttf"

static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// Save the current framebuffer as a PPM (P6) image (for headless verification).
static bool SaveFramebufferPPM(const char* path, int w, int h)
{
    std::vector<unsigned char> pixels((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

    FILE* f = fopen(path, "wb");
    if (f == NULL)
        return false;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    // Flip vertically (OpenGL bottom-left origin).
    for (int y = h - 1; y >= 0; y--)
        fwrite(&pixels[(size_t)y * w * 3], 1, (size_t)w * 3, f);
    fclose(f);
    return true;
}

static char password[64] = "";

void DrawPassword()
{
    static bool showPassword = false;

    ImGuiInputTextFlags flags = showPassword ? 0 : ImGuiInputTextFlags_Password;
    ImGui::Checkbox("إظهار كلمة السر", &showPassword);
    ImGui::SameLine();
    ImGui::InputText("كلمة السر", password, IM_ARRAYSIZE(password), flags);

    if (ImGui::Button("Login"))
    {
        // use password
    }
}

// Draw one line of shaped text with a selection highlight over a UTF-8 byte range.
// The highlight uses the same shaper IndexToXOffset() mapping that InputText selection
// rendering uses, so this is a direct visual check of the RTL selection highlight path.
static void TextWithSelection(ImFont* font, const char* text, int sel_begin_byte, int sel_end_byte)
{
    ImGui::PushFont(font);
    const float font_size = ImGui::GetFontSize();
    ImFont* cur_font = ImGui::GetFont();
    ImFontBaked* baked = cur_font->GetFontBaked(font_size);
    const ImFontShaper* shaper = ImGui::GetIO().Fonts->FontShaper;
    const char* text_end = text + strlen(text);

    const ImVec2 pos = ImGui::GetCursorScreenPos();

    // Match the automatic RTL right-alignment applied by ImGui::Text() so the highlight
    // rectangle lands on the drawn text instead of its pre-alignment position.
    float base_x = pos.x;
#ifdef IMGUI_ENABLE_RTL
    base_x = ImGuiRTL::AlignTextRight(pos.x, ImGui::GetCurrentWindow()->WorkRect.Max.x, text, text_end, -1.0f);
#endif

    if (shaper != NULL && shaper->GetSelectionSegments != NULL && baked != NULL && sel_begin_byte < sel_end_byte)
    {
        // Draw one highlight rect per visual segment (a bidi selection can be several
        // disjoint runs). Matches how InputText now renders its selection highlight.
        enum { SEL_MAX_SEGMENTS = 32 };
        float segments[SEL_MAX_SEGMENTS * 2];
        const int n = shaper->GetSelectionSegments(cur_font, baked, text, text_end, sel_begin_byte, sel_end_byte, segments, SEL_MAX_SEGMENTS);
        if (n >= 0)
        {
            for (int s = 0; s < n; s++)
            {
                const float lo = segments[s * 2 + 0];
                const float hi = segments[s * 2 + 1];
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(base_x + lo, pos.y), ImVec2(base_x + hi, pos.y + font_size),
                    IM_COL32(58, 114, 190, 150));
            }
        }
    }
    else if (shaper != NULL && shaper->IndexToXOffset != NULL && baked != NULL && sel_begin_byte < sel_end_byte)
    {
        const float x1 = shaper->IndexToXOffset(cur_font, baked, text, text_end, sel_begin_byte, -1);
        const float x2 = shaper->IndexToXOffset(cur_font, baked, text, text_end, sel_end_byte, -1);
        if (x1 >= 0.0f && x2 >= 0.0f)
        {
            const float lo = ImMin(x1, x2);
            const float hi = ImMax(x1, x2);
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(base_x + lo, pos.y), ImVec2(base_x + hi, pos.y + font_size),
                IM_COL32(58, 114, 190, 150));
        }
    }

    ImGui::Text("%s", text);
    ImGui::PopFont();
}

// Build glyph ranges: default (Latin) + Arabic + Arabic presentation forms + punctuation.
static const ImWchar* GetRtlGlyphRanges(ImFontAtlas* atlas)
{
    static ImVector<ImWchar> ranges;
    if (ranges.empty())
    {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(atlas->GetGlyphRangesDefault());
        builder.AddRanges((ImWchar[]){
            0x0600, 0x06FF, // Arabic
            0x0750, 0x077F, // Arabic Supplement
            0x08A0, 0x08FF, // Arabic Extended-A
            0xFB50, 0xFDFF, // Arabic Presentation Forms-A
            0xFE70, 0xFEFF, // Arabic Presentation Forms-B
            0x2000, 0x206F, // General Punctuation
            0,
        });
        builder.BuildRanges(&ranges);
    }
    return ranges.Data;
}

// Arabic-only glyph ranges (for merging an Arabic font onto a Latin base font).
static const ImWchar* GetArabicGlyphRanges(ImFontAtlas* atlas)
{
    static ImVector<ImWchar> ranges;
    if (ranges.empty())
    {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges((ImWchar[]){
            0x0600, 0x06FF, // Arabic
            0x0750, 0x077F, // Arabic Supplement
            0x08A0, 0x08FF, // Arabic Extended-A
            0xFB50, 0xFDFF, // Arabic Presentation Forms-A
            0xFE70, 0xFEFF, // Arabic Presentation Forms-B
            0,
        });
        builder.BuildRanges(&ranges);
    }
    return ranges.Data;
}

int main(int argc, char** argv)
{
    const char* arabic_font_path = (argc > 1) ? argv[1] : RTL_DEMO_FONT_ARABIC;
    const char* latin_font_path  = (argc > 2) ? argv[2] : RTL_DEMO_FONT_LATIN;
    const char* screenshot_path  = (argc > 3) ? argv[3] : NULL;

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(1280, 760, "Dear ImGui RTL Demo", NULL, NULL);
    if (window == NULL)
        return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Load the font. The RTL shaper is attached automatically (IMGUI_ENABLE_RTL).
    // Light hinting matters for Arabic: full hinting grid-fits each glyph's horizontal
    // edges independently, which breaks the cursive joins and leaves tiny gaps between
    // letters. Light hinting snaps only vertically, so it preserves those joins (see
    // imgui_freetype.h: ImGuiFreeTypeLoaderFlags_LightHinting).
    ImFont* arabic_font = NULL;
    {
        ImFontConfig arabic_cfg;
        arabic_cfg.OversampleH = arabic_cfg.OversampleV = 2;
        arabic_cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
        arabic_font = io.Fonts->AddFontFromFileTTF(arabic_font_path, 36.0f, &arabic_cfg, GetRtlGlyphRanges(io.Fonts));

        if (arabic_font == NULL)
        {
            fprintf(stderr, "Failed to load the font.\n");
            return 1;
        }
    }

    // Merged-font example: a Latin-only base font with Noto Naskh Arabic merged in. This
    // exercises the shaper's per-source face fallback (Latin glyphs from source 0, Arabic
    // glyphs from the merged source).
    ImFont* merged_font = NULL;
    {
        ImFontConfig base_cfg;
        base_cfg.OversampleH = base_cfg.OversampleV = 2;
        merged_font = io.Fonts->AddFontFromFileTTF(latin_font_path, 28.0f, &base_cfg, io.Fonts->GetGlyphRangesDefault());

        ImFontConfig merge_cfg;
        merge_cfg.OversampleH = merge_cfg.OversampleV = 2;
        merge_cfg.MergeMode = true;
        merge_cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
        io.Fonts->AddFontFromFileTTF(arabic_font_path, 28.0f, &merge_cfg, GetArabicGlyphRanges(io.Fonts));
    }

    // Debug toggle: render with shaping disabled to compare against the shaped output.
    if (getenv("RTL_DISABLE_SHAPER") != NULL)
        io.Fonts->SetFontShaper(NULL); // Explicit: survives later atlas builds (unlike assigning the member directly).

    // Arabic samples.
    const char* ar_hello = "\xD8\xA7\xD9\x84\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xD8\xB9\xD9\x84\xD9\x8A\xD9\x83\xD9\x85";               // "السلام عليكم"
    const char* ar_rahman = "\xD8\xA8\xD9\x90\xD8\xB3\xD9\x92\xD9\x85\xD9\x90 \xD8\xA7\xD9\x84\xD9\x84\xD9\x91\xD9\x8E\xD9\x87\xD9\x90 \xD8\xA7\xD9\x84\xD8\xB1\xD9\x91\xD9\x8E\xD8\xAD\xD9\x92\xD9\x85\xD9\x8E\xD9\x86\xD9\x90 \xD8\xA7\xD9\x84\xD8\xB1\xD9\x91\xD9\x8E\xD8\xAD\xD9\x90\xD9\x8A\xD9\x85\xD9\x90"; // Bismillah with diacritics
    const char* ar_sentence = "\xD9\x85\xD9\x8E\xD8\xB1\xD9\x92\xD8\xAD\xD9\x8E\xD8\xA8\xD9\x8B\xD8\xA7 \xD8\xA8\xD9\x90\xD9\x83\xD9\x85\xD9\x92 \xD9\x81\xD9\x8A \xD9\x87\xD8\xB0\xD8\xA7 \xD8\xA7\xD9\x84\xD8\xAA\xD9\x91\xD9\x8E\xD8\xB7\xD8\xA8\xD9\x90\xD9\x8A\xD9\x82\xD9\x90"; // "مرحبًا بكم في هذا التطبيق"
    const char* mixed = "Mixed \xD9\x86\xD8\xB5 \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A and English 1234";
    const char* long_ar = "\xD9\x87\xD8\xB0\xD8\xA7 \xD9\x86\xD8\xB5 \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A \xD8\xB7\xD9\x88\xD9\x8A\xD9\x84 \xD8\xAC\xD8\xAF\xD9\x91\xD8\xA7 \xD9\x84\xD8\xA7\xD8\xAE\xD8\xAA\xD8\xA8\xD8\xA7\xD8\xB1 \xD8\xA7\xD9\x84\xD8\xAA\xD9\x81\xD8\xA7\xD9\x81 \xD8\xA7\xD9\x84\xD8\xA3\xD8\xB3\xD8\xB7\xD8\xB1";

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        {
        char buf[128];
            sprintf(buf, "FPS (معدل الإطارات): %.2f | Frame Time: %.3f ms", io.Framerate, 1000.0f / ImGui::GetIO().Framerate);
            ImGui::Begin("RTL / Arabic shaping demo");
            ImGui::Text(buf);
            ImGui::Separator();

            ImGui::PushFont(arabic_font);
            ImGui::Text("Noto Naskh Arabic (connected cursive):");
            ImGui::Text("  %s", ar_hello);
            ImGui::Text("  %s", ar_sentence);
            ImGui::Text("  %s", ar_rahman);
            ImGui::Text("  %s", mixed);
            ImGui::TextWrapped("  Wrapped RTL: \n%s", long_ar);
            ImGui::PopFont();

            ImGui::Separator();

            ImGui::PushFont(arabic_font);
            ImGui::Text("Button/label with RTL text:");
            if (ImGui::Button(ar_hello))
                {}
            ImGui::SameLine();
            ImGui::Text("<- this button label is shaped RTL");
            ImGui::PopFont();

            ImGui::Separator();

            // RTL text is right-aligned in widgets with a known width (Selectable, InputText).
            ImGui::PushFont(arabic_font);
            ImGui::Text("RTL right-aligned widgets (Selectable):");
            ImGui::Selectable((std::string(ar_hello) + "##selectable1").c_str());
            ImGui::Selectable(ar_sentence);
            ImGui::PopFont();

            ImGui::Separator();

            // Merged-font check: a Latin-only base with Noto Naskh Arabic merged in. The
            // Arabic glyphs fall back to the merged source and should still shape correctly.
            ImGui::PushFont(merged_font);
            ImGui::Text("Merged font (Latin base + Noto Naskh Arabic):");
            ImGui::Text("  %s", ar_hello);
            ImGui::Text("  %s", ar_rahman);
            ImGui::Text("  %s", mixed);
            ImGui::PopFont();

            ImGui::Separator();

            // RTL text editing (caret/mouse mapping is shaper-aware for single-line inputs).
            static char input_buf[256] = "\xD8\xA7\xD9\x84\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xD8\xB9\xD9\x84\xD9\x8A\xD9\x83\xD9\x85"; // "السلام عليكم"
            ImGui::PushFont(arabic_font);
            ImGui::Text("RTL text editing:");
            ImGui::InputText("##rtl_input", input_buf, IM_ARRAYSIZE(input_buf));
            ImGui::PopFont();

            ImGui::Separator();

            // Multi-line RTL editing (word-wrapped): exercises shaper-aware click, selection
            // and Up/Down caret movement across wrapped visual lines.
            static char multiline_buf[1024];
            if (multiline_buf[0] == 0)
                snprintf(multiline_buf, IM_ARRAYSIZE(multiline_buf), "%s\n%s\n%s", ar_hello, ar_rahman, long_ar);
            ImGui::PushFont(arabic_font);
            ImGui::Text("RTL multiline editing (word wrap):");
            ImGui::InputTextMultiline("##rtl_multiline", multiline_buf, IM_ARRAYSIZE(multiline_buf),
                ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 4.0f), ImGuiInputTextFlags_WordWrap);
            ImGui::PopFont();

            ImGui::Separator();

            // Selection highlight visual check (same byte->x mapping as InputText selection).
            // ar_hello = "السلام عليكم" (11 letters + space). Bytes 0..12 select "السلام";
            // bytes 13..23 select "عليكم".
            ImGui::PushFont(arabic_font);
            ImGui::Text("Selection highlight (logical byte ranges):");
            TextWithSelection(arabic_font, ar_hello, 0, 12);   // select "السلام"
            TextWithSelection(arabic_font, ar_hello, 13, 23);  // select "عليكم"
            TextWithSelection(arabic_font, ar_hello, 4, 12);   // select "سلام" (partial word)
            TextWithSelection(arabic_font, mixed, 6, 19);      // select Arabic "نص عربي" inside mixed text (bidi segments)
            TextWithSelection(arabic_font, mixed, 0, 10);   // "Mixed نص" (English word and Arabic word) (bidi segments)
            ImGui::PopFont();

            ImGui::Separator();
            DrawPassword();

            ImGui::End();
        }

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Capture after a couple of settled frames, reading the back buffer (front-buffer
        // reads return black under some software GL stacks).
        static int frame_count = 0;
        frame_count++;
        if (screenshot_path != NULL && frame_count >= 3)
        {
            int fb_w, fb_h;
            glfwGetFramebufferSize(window, &fb_w, &fb_h);
            glReadBuffer(GL_BACK);
            SaveFramebufferPPM(screenshot_path, fb_w, fb_h);
            break;
        }

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
