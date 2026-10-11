// Host-side tests for the software rasterizer (imgui_impl_softrast).
//
// Each test has Dear ImGui build a real draw list, rasterizes it into a small
// buffer, and checks pixels: exact coverage, clipping, blending arithmetic,
// and that triangles sharing an edge cover every pixel exactly once. Run
// under asan, so a span written past its row is caught too.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "imgui.h"
#include "imgui_impl_softrast.h"

static int g_failures = 0;

static void expect(bool condition, const char* what) {
    if (condition) return;
    printf("  FAIL  %s\n", what);
    g_failures++;
}

namespace {

constexpr int kWidth = 64, kHeight = 48;
constexpr uint32_t kBackground = 0x00102030;

struct Canvas {
    // A row is wider than the image, as a real framebuffer's may be; the
    // padding must come back untouched.
    static constexpr int kPitchPixels = kWidth + 3;
    std::vector<uint32_t> pixels = std::vector<uint32_t>(kPitchPixels * kHeight, kBackground);
    uint32_t at(int x, int y) const { return pixels[y * kPitchPixels + x]; }
    ImGui_ImplSoftRast_Target target() { return { pixels.data(), kWidth, kHeight, kPitchPixels * 4 }; }
    bool padding_intact() const {
        for (int y = 0; y < kHeight; y++)
            for (int x = kWidth; x < kPitchPixels; x++)
                if (at(x, y) != kBackground) return false;
        return true;
    }
};

// Run one frame whose whole content is whatever `draw` adds to the
// background draw list, and rasterize it.
template <typename F>
Canvas render(F draw) {
    ImGui::NewFrame();
    draw(ImGui::GetBackgroundDrawList());
    ImGui::Render();
    Canvas canvas;
    ImGui_ImplSoftRast_RenderDrawData(ImGui::GetDrawData(), canvas.target());
    expect(canvas.padding_intact(), "nothing is written past the end of a row");
    return canvas;
}

// What blending `source` at `alpha` over the background must give.
uint32_t blended(uint32_t source, uint32_t alpha) {
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        const uint32_t s = (source >> shift) & 0xff, d = (kBackground >> shift) & 0xff;
        uint32_t x = s * alpha + d * (255 - alpha) + 128;
        x = (x + (x >> 8)) >> 8;
        out |= x << shift;
    }
    return out;
}

int count_pixels(const Canvas& canvas, uint32_t value) {
    int count = 0;
    for (int y = 0; y < kHeight; y++)
        for (int x = 0; x < kWidth; x++)
            if (canvas.at(x, y) == value) count++;
    return count;
}

} // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(kWidth, kHeight);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->TexDesiredFormat = ImTextureFormat_Alpha8;
    ImGui_ImplSoftRast_Init();
    // Exact coverage is what is under test; anti-aliasing fringes would blur it.
    ImGui::GetStyle().AntiAliasedFill = false;
    ImGui::GetStyle().AntiAliasedLines = false;

    printf("rectangles:\n");
    {
        const Canvas canvas = render([](ImDrawList* list) {
            list->AddRectFilled(ImVec2(10, 5), ImVec2(20, 9), IM_COL32(255, 128, 0, 255));
        });
        bool exact = true;
        for (int y = 0; y < kHeight; y++)
            for (int x = 0; x < kWidth; x++) {
                const bool inside = x >= 10 && x < 20 && y >= 5 && y < 9;
                if (canvas.at(x, y) != (inside ? 0x00ff8000u : kBackground)) exact = false;
            }
        expect(exact, "an opaque rectangle covers exactly its pixels, in XRGB order");
        expect(ImGui_ImplSoftRast_GetStats().Rects == 1 && ImGui_ImplSoftRast_GetStats().PixelsFilled == 40,
               "it takes the rectangle fast path");
    }
    {
        // Every width from 1 to 12 at every x offset 0..3: all the ways a row
        // can split between the four-pixel SSE2 loop and its scalar tail.
        bool all = true;
        for (int width = 1; width <= 12; width++)
            for (int start = 0; start < 4; start++) {
                const Canvas canvas = render([&](ImDrawList* list) {
                    list->AddRectFilled(ImVec2((float)start, 1), ImVec2((float)(start + width), 2), IM_COL32(200, 100, 50, 77));
                });
                for (int x = 0; x < kWidth; x++) {
                    const bool inside = x >= start && x < start + width;
                    if (canvas.at(x, 1) != (inside ? blended(0x00c86432, 77) : kBackground)) all = false;
                }
            }
        expect(all, "translucent spans blend identically in the vector loop and its tail");
    }
    {
        const Canvas canvas = render([](ImDrawList* list) {
            list->PushClipRect(ImVec2(12, 6), ImVec2(16, 40));
            list->AddRectFilled(ImVec2(0, 0), ImVec2(kWidth, 8), IM_COL32(255, 255, 255, 255));
            list->PopClipRect();
        });
        expect(count_pixels(canvas, 0x00ffffff) == 4 * 2 && canvas.at(12, 6) == 0x00ffffff && canvas.at(15, 7) == 0x00ffffff,
               "a clip rectangle limits what is drawn");
    }
    {
        const Canvas canvas = render([](ImDrawList* list) {
            list->AddRectFilled(ImVec2(-50, -50), ImVec2(500, 500), IM_COL32(1, 2, 3, 255));
        });
        expect(count_pixels(canvas, 0x00010203) == kWidth * kHeight, "a rectangle larger than the target is clipped to it");
    }

    printf("triangles:\n");
    {
        // Four translucent triangles fanned around an interior point tile a
        // rectangle. With the top-left rule each pixel is blended once: a
        // pixel on a shared edge drawn twice, or not at all, shows here.
        bool watertight = true;
        const ImVec2 centers[] = { ImVec2(20.5f, 17.5f), ImVec2(13.25f, 30.75f), ImVec2(33, 20), ImVec2(9.1f, 9.9f) };
        for (const ImVec2& center : centers) {
            const Canvas canvas = render([&](ImDrawList* list) {
                const ImVec2 a(8, 8), b(40, 8), c(40, 36), d(8, 36);
                const ImU32 color = IM_COL32(255, 255, 255, 128);
                list->AddTriangleFilled(a, b, center, color);
                list->AddTriangleFilled(b, c, center, color);
                list->AddTriangleFilled(c, d, center, color);
                list->AddTriangleFilled(d, a, center, color);
            });
            for (int y = 0; y < kHeight; y++)
                for (int x = 0; x < kWidth; x++) {
                    const bool inside = x >= 8 && x < 40 && y >= 8 && y < 36;
                    if (canvas.at(x, y) != (inside ? blended(0x00ffffff, 128) : kBackground)) watertight = false;
                }
        }
        expect(watertight, "triangles sharing edges cover each pixel exactly once");
    }
    {
        // Both windings must draw: Dear ImGui does not cull.
        const Canvas clockwise = render([](ImDrawList* list) {
            list->AddTriangleFilled(ImVec2(4, 4), ImVec2(30, 4), ImVec2(4, 30), IM_COL32(0, 255, 0, 255));
        });
        const Canvas counter = render([](ImDrawList* list) {
            list->AddTriangleFilled(ImVec2(4, 4), ImVec2(4, 30), ImVec2(30, 4), IM_COL32(0, 255, 0, 255));
        });
        const int covered = count_pixels(clockwise, 0x0000ff00);
        expect(covered > 300 && covered < 380 && clockwise.pixels == counter.pixels, "both windings draw the same triangle");
        expect(clockwise.at(5, 5) == 0x0000ff00 && clockwise.at(29, 29) == kBackground, "and only inside it");
    }
    {
        const Canvas canvas = render([](ImDrawList* list) {
            list->AddRectFilledMultiColor(ImVec2(0, 10), ImVec2(kWidth, 20), IM_COL32(0, 0, 0, 255), IM_COL32(255, 255, 255, 255),
                                          IM_COL32(255, 255, 255, 255), IM_COL32(0, 0, 0, 255));
        });
        bool monotonic = true;
        for (int x = 1; x < kWidth; x++)
            if ((canvas.at(x, 15) & 0xff) < (canvas.at(x - 1, 15) & 0xff)) monotonic = false;
        const uint32_t middle = canvas.at(kWidth / 2, 15);
        expect(monotonic && (canvas.at(0, 15) & 0xff) < 8 && (canvas.at(kWidth - 1, 15) & 0xff) > 247,
               "vertex colours are interpolated across a gradient");
        expect(((middle >> 16) & 0xff) == (middle & 0xff) && std::abs((int)(middle & 0xff) - 128) < 6, "evenly, in every channel");
    }

    printf("textures:\n");
    {
        // 2x2, bytes R G B A: red, green / blue, half-transparent white.
        static const unsigned char rgba[16] = { 255, 0, 0, 255,  0, 255, 0, 255,  0, 0, 255, 255,  255, 255, 255, 128 };
        static const ImGui_ImplSoftRast_Texture texture{ 2, 2, ImTextureFormat_RGBA32, rgba };
        const Canvas canvas = render([](ImDrawList* list) {
            list->AddImage((ImTextureID)(intptr_t)&texture, ImVec2(10, 10), ImVec2(12, 12));
        });
        expect(canvas.at(10, 10) == 0x00ff0000 && canvas.at(11, 10) == 0x0000ff00 && canvas.at(10, 11) == 0x000000ff,
               "an RGBA texture drawn at one texel per pixel is copied exactly");
        expect(canvas.at(11, 11) == blended(0x00ffffff, 128) && count_pixels(canvas, kBackground) == kWidth * kHeight - 4,
               "with its alpha blended, and nothing around it touched");

        const Canvas tinted = render([](ImDrawList* list) {
            list->AddImage((ImTextureID)(intptr_t)&texture, ImVec2(10, 10), ImVec2(12, 12), ImVec2(0, 0), ImVec2(1, 1),
                           IM_COL32(128, 255, 255, 255));
        });
        expect(tinted.at(10, 10) == 0x00800000, "the vertex colour modulates the texture");

        const Canvas scaled = render([](ImDrawList* list) {
            list->AddImage((ImTextureID)(intptr_t)&texture, ImVec2(10, 10), ImVec2(30, 30));
        });
        const uint32_t corner = scaled.at(10, 10), between = scaled.at(20, 12);
        expect(corner == 0x00ff0000 && ((between >> 16) & 0xff) > 100 && ((between >> 8) & 0xff) > 100,
               "a magnified texture is filtered between texels");
    }
    {
        // What Wayland clients send: wl_shm's XRGB8888 and ARGB8888, which in
        // memory are B, G, R, X and B, G, R, A with A already multiplied in.
        // 5x1, so the opaque blit runs its four-pixel loop and its tail.
        static const unsigned char bgrx[20] = { 0, 0, 255, 99,  0, 255, 0, 7,  255, 0, 0, 0,  1, 2, 3, 4,  9, 8, 7, 255 };
        static const ImGui_ImplSoftRast_Texture opaque{ 5, 1, ImTextureFormat_RGBA32, bgrx, ImGui_ImplSoftRast_Layout_BGRX };
        const Canvas canvas = render([](ImDrawList* list) {
            list->AddImage((ImTextureID)(intptr_t)&opaque, ImVec2(20, 3), ImVec2(25, 4));
        });
        expect(canvas.at(20, 3) == 0x00ff0000 && canvas.at(21, 3) == 0x0000ff00 && canvas.at(22, 3) == 0x000000ff &&
               canvas.at(23, 3) == 0x00030201 && canvas.at(24, 3) == 0x00070809,
               "an XRGB8888 buffer is copied as it is, whatever its unused byte holds");

        // Opaque, transparent, half-covered grey (64 of 128), and a broken
        // pixel whose red exceeds its alpha.
        static const unsigned char bgra[16] = { 0, 255, 0, 255,  0, 0, 0, 0,  64, 64, 64, 128,  0, 0, 250, 100 };
        static const ImGui_ImplSoftRast_Texture premultiplied{ 4, 1, ImTextureFormat_RGBA32, bgra, ImGui_ImplSoftRast_Layout_BGRA_Premultiplied };
        const Canvas blended_canvas = render([](ImDrawList* list) {
            list->AddImage((ImTextureID)(intptr_t)&premultiplied, ImVec2(20, 5), ImVec2(24, 6));
        });
        const uint32_t half = blended_canvas.at(22, 5);
        auto near = [](uint32_t value, uint32_t want) { return value + 1 >= want && value <= want + 1; };
        expect(blended_canvas.at(20, 5) == 0x0000ff00 && blended_canvas.at(21, 5) == kBackground,
               "a premultiplied ARGB8888 buffer: opaque pixels replace, transparent ones leave alone");
        expect(near((half >> 16) & 0xff, 64 + 0x10 / 2) && near((half >> 8) & 0xff, 64 + 0x20 / 2) && near(half & 0xff, 64 + 0x30 / 2),
               "and the rest are added over what is underneath, scaled by what they do not cover");
        expect(((blended_canvas.at(23, 5) >> 16) & 0xff) == 255, "a channel brighter than its alpha saturates instead of wrapping");

        // Scaled, the same pixels go through the general path, which works
        // in straight alpha: the grey must come out the same.
        const Canvas scaled = render([](ImDrawList* list) {
            list->AddImage((ImTextureID)(intptr_t)&premultiplied, ImVec2(0, 10), ImVec2(40, 20));
        });
        expect(scaled.at(1, 15) == 0x0000ff00, "scaled, a premultiplied texture is unmultiplied before it is filtered");
    }
    {
        static const unsigned char coverage[4] = { 255, 0, 64, 255 };
        static const ImGui_ImplSoftRast_Texture texture{ 2, 2, ImTextureFormat_Alpha8, coverage };
        const Canvas canvas = render([](ImDrawList* list) {
            list->AddImage((ImTextureID)(intptr_t)&texture, ImVec2(3, 3), ImVec2(5, 5), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 0, 255, 255));
        });
        expect(canvas.at(3, 3) == 0x00ff00ff && canvas.at(4, 3) == kBackground && canvas.at(3, 4) == blended(0x00ff00ff, 64),
               "an alpha-only texture is coverage for the vertex colour");
    }
    {
        const Canvas canvas = render([](ImDrawList* list) {
            list->PushClipRect(ImVec2(0, 0), ImVec2(kWidth, 20));
            list->AddText(ImVec2(2, 2), IM_COL32(255, 255, 255, 255), "Sirius");
            list->PopClipRect();
        });
        int lit = 0, lit_outside = 0;
        for (int y = 0; y < kHeight; y++)
            for (int x = 0; x < kWidth; x++)
                if (canvas.at(x, y) != kBackground) { lit++; if (y >= 20 || y < 2) lit_outside++; }
        expect(lit > 40 && lit < 400 && lit_outside == 0, "text from the font atlas is drawn, where it belongs");
        expect(count_pixels(canvas, 0x00ffffff) > 20, "with fully covered pixels at full brightness");
    }

    ImGui_ImplSoftRast_Shutdown();
    ImGui::DestroyContext();

    if (g_failures) {
        printf("softrast_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("softrast_test: all passed\n");
    return 0;
}
