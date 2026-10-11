#include "splash.h"
#include "settings.h"

#include "imgui.h"

#include <cfloat>
#include <cmath>

namespace desktop {
namespace {

constexpr float kPi = 3.14159265f;
constexpr double kFadeIn = 0.7;     // out of the kernel's black screen
constexpr double kFadeOut = 0.6;    // into the desktop

double g_start = -1.0;              // ImGui time it started; < 0 when there is none
double g_hold = 0.0;

double Elapsed() { return ImGui::GetTime() - g_start; }

float Clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
float EaseOut(float x) { x = Clamp01(x); return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x); }

ImU32 Color(const float rgb[3], float alpha)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], Clamp01(alpha)));
}

ImU32 Color(float r, float g, float b, float alpha)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, Clamp01(alpha)));
}

// A four-pointed star: the curve |x|^1/2 + |y|^1/2 = r^1/2, whose sides bow
// inwards to sharp points -- turned by `angle`.
void Star(ImDrawList* draw, ImVec2 centre, float radius, float angle, ImU32 color)
{
    constexpr int kPoints = 96;
    ImVec2 points[kPoints];
    const float ca = std::cos(angle), sa = std::sin(angle);
    for (int i = 0; i < kPoints; i++)
    {
        const float t = 2.0f * kPi * (float)i / (float)kPoints;
        const float c = std::cos(t), s = std::sin(t);
        const float x = radius * std::copysign(c * c * c * c, c), y = radius * std::copysign(s * s * s * s, s);
        points[i] = ImVec2(centre.x + x * ca - y * sa, centre.y + x * sa + y * ca);
    }
    draw->AddConcavePolyFilled(points, kPoints, color);
}

// Text centred on `x`, with its top at `y`, letters spread by `spacing`.
void SpacedText(ImDrawList* draw, float size, float x, float y, const char* text, float spacing, ImU32 color)
{
    ImFont* font = ImGui::GetFont();
    float width = 0.0f;
    for (const char* p = text; *p; p++)
        width += font->CalcTextSizeA(size, FLT_MAX, 0.0f, p, p + 1).x + (p[1] ? spacing : 0.0f);
    float pen = x - width * 0.5f;
    for (const char* p = text; *p; p++)
    {
        draw->AddText(font, size, ImVec2(std::floor(pen), std::floor(y)), color, p, p + 1);
        pen += font->CalcTextSizeA(size, FLT_MAX, 0.0f, p, p + 1).x + spacing;
    }
}

// A comet: an arc that grows and shrinks as it turns, bright at its head
// and fading along its tail.
void Spinner(ImDrawList* draw, ImVec2 centre, float radius, float thickness, float t, const float accent[3], float alpha)
{
    draw->AddCircle(centre, radius, Color(accent, alpha * 0.14f), 64, thickness);

    const float length = 0.18f + 0.52f * (0.5f - 0.5f * std::cos(2.0f * kPi * t / 1.6f));   // in turns
    const float head = (float)std::fmod(t * 0.85, 1.0) + length * 0.5f;
    constexpr int kSegments = 48;
    for (int i = 0; i < kSegments; i++)
    {
        const float f0 = (float)i / kSegments, f1 = (float)(i + 1) / kSegments;
        const float a0 = 2.0f * kPi * (head - length * (1.0f - f0)), a1 = 2.0f * kPi * (head - length * (1.0f - f1));
        const ImVec2 p0(centre.x + radius * std::cos(a0), centre.y + radius * std::sin(a0));
        const ImVec2 p1(centre.x + radius * std::cos(a1), centre.y + radius * std::sin(a1));
        const float strength = f1 * (2.0f - f1);    // eased: most of the arc is bright, the end of the tail fades
        // Towards the head the colour goes to white.
        const float white = f1 * f1 * 0.5f;
        draw->AddLine(p0, p1, Color(accent[0] + (1.0f - accent[0]) * white, accent[1] + (1.0f - accent[1]) * white,
                                    accent[2] + (1.0f - accent[2]) * white, alpha * strength), thickness);
    }
    const float a = 2.0f * kPi * head;
    draw->AddCircleFilled(ImVec2(centre.x + radius * std::cos(a), centre.y + radius * std::sin(a)), thickness * 0.5f,
                          Color(1.0f, 1.0f, 1.0f, alpha * 0.9f), 16);
}

} // namespace

void StartSplash(double seconds)
{
    g_start = ImGui::GetTime();
    g_hold = seconds;
}

bool SplashActive() { return g_start >= 0.0; }

bool SplashCovers() { return g_start >= 0.0 && Elapsed() < g_hold; }

void DrawSplash()
{
    if (g_start < 0.0)
        return;
    const double t = Elapsed();
    if (t >= g_hold + kFadeOut)
    {
        g_start = -1.0;     // gone: the desktop is all there is
        return;
    }
    const Settings& settings = GetSettings();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 size = viewport->Size, origin = viewport->Pos;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    // Everything fades out together at the end; the pieces come in one by one.
    const float out = t < g_hold ? 1.0f : 1.0f - EaseOut((float)((t - g_hold) / kFadeOut));
    const float scale = size.y / 1080.0f < 0.6f ? 0.6f : size.y / 1080.0f;

    if (SplashCovers())
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);

    // The wallpaper, exactly as the desktop paints it, so that the desktop
    // appears out of it rather than in place of it.
    const float* bottom = settings.Wallpaper == 0 ? settings.WallpaperBottom : settings.WallpaperTop;
    draw->AddRectFilledMultiColor(origin, ImVec2(origin.x + size.x, origin.y + size.y), Color(settings.WallpaperTop, out),
                                  Color(settings.WallpaperTop, out), Color(bottom, out), Color(bottom, out));

    const ImVec2 centre(origin.x + size.x * 0.5f, origin.y + size.y * 0.44f);
    const float logo_in = EaseOut((float)(t / 0.9));
    const float radius = 58.0f * scale * (0.82f + 0.18f * logo_in);
    const float pulse = 0.5f + 0.5f * std::sin((float)t * 2.0f * kPi / 2.4f);

    // The glow: rings of faint accent colour, adding up towards the middle.
    // Many and faint, so that its edge is too gentle to see.
    for (int i = 0; i < 26; i++)
    {
        const float r = radius * (0.3f + 0.115f * (float)i) * (0.96f + 0.08f * pulse);
        draw->AddCircleFilled(centre, r, Color(settings.Accent, out * logo_in * (0.012f + 0.007f * pulse)), 96);
    }
    // The star, and a smaller one across it that twinkles.
    Star(draw, centre, radius * 0.55f, kPi / 4.0f, Color(0.80f, 0.88f, 1.0f, out * logo_in * (0.35f + 0.35f * pulse)));
    Star(draw, centre, radius, 0.0f, Color(0.93f, 0.96f, 1.0f, out * logo_in));
    draw->AddCircleFilled(centre, radius * 0.07f, Color(1.0f, 1.0f, 1.0f, out * logo_in), 24);

    // The name, then what is happening.
    const float text_in = EaseOut((float)((t - 0.25) / 0.8));
    const float name_y = centre.y + radius * 1.45f + (1.0f - text_in) * 12.0f * scale;
    SpacedText(draw, 40.0f * scale, centre.x, name_y, "SIRIUS", 14.0f * scale, Color(0.90f, 0.93f, 1.0f, out * text_in));
    SpacedText(draw, 15.0f * scale, centre.x, name_y + 56.0f * scale, "starting the desktop", 1.5f * scale,
               Color(0.65f, 0.72f, 0.85f, out * text_in * 0.8f));

    const float spinner_in = EaseOut((float)((t - 0.45) / 0.7));
    Spinner(draw, ImVec2(centre.x, name_y + 125.0f * scale), 19.0f * scale, 4.0f * scale, (float)t, settings.Accent, out * spinner_in);

    // Up out of the kernel's black screen.
    if (t < kFadeIn)
        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), Color(0.0f, 0.0f, 0.0f, 1.0f - EaseOut((float)(t / kFadeIn))));
}

} // namespace desktop
