#include "settings.h"

#include "imgui.h"
#include "imgui_impl_sirius.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace desktop {
namespace {

Settings g_settings;
std::string g_path;
bool g_wallpaper_changed = true;
Settings g_applied_wallpaper;   // what the wallpaper was last rendered from

// One table drives loading and saving alike.
struct Field { const char* Key; enum { Int, Float, Float3, Bool } Type; void* Value; };

const Field* Fields(int& count)
{
    Settings& s = g_settings;
    static const Field fields[] = {
        { "theme", Field::Int, &s.Theme },
        { "accent", Field::Float3, s.Accent },
        { "font_size", Field::Float, &s.FontSize },
        { "rounding", Field::Float, &s.Rounding },
        { "wallpaper", Field::Int, &s.Wallpaper },
        { "wallpaper_top", Field::Float3, s.WallpaperTop },
        { "wallpaper_bottom", Field::Float3, s.WallpaperBottom },
        { "keyboard_layout", Field::Int, &s.KeyboardLayout },
        { "mouse_speed", Field::Float, &s.MouseSpeed },
        { "show_seconds", Field::Bool, &s.ShowSeconds },
    };
    count = (int)(sizeof(fields) / sizeof(fields[0]));
    return fields;
}

float Clamp(float value, float low, float high) { return value < low ? low : (value > high ? high : value); }

// Whatever a file said, the desktop must stay usable.
void Sanitize()
{
    Settings& s = g_settings;
    if (s.Theme < 0 || s.Theme > 2) s.Theme = 0;
    if (s.Wallpaper < 0 || s.Wallpaper > 1) s.Wallpaper = 0;
    if (s.KeyboardLayout < 0 || s.KeyboardLayout >= ImGui_ImplSirius_Layout_COUNT) s.KeyboardLayout = 0;
    s.FontSize = Clamp(s.FontSize, 10.0f, 32.0f);
    s.Rounding = Clamp(s.Rounding, 0.0f, 12.0f);
    s.MouseSpeed = Clamp(s.MouseSpeed, 0.25f, 4.0f);
    for (float* color : { s.Accent, s.WallpaperTop, s.WallpaperBottom })
        for (int i = 0; i < 3; i++) color[i] = Clamp(color[i], 0.0f, 1.0f);
}

} // namespace

Settings& GetSettings() { return g_settings; }

bool LoadSettings(const std::string& path)
{
    g_path = path;
    FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr)
        return false;
    int count;
    const Field* fields = Fields(count);
    char line[256];
    while (std::fgets(line, sizeof(line), file))
    {
        char key[64];
        char value[160];
        if (std::sscanf(line, " %63[a-z_] = %159[^\n]", key, value) != 2)
            continue;
        for (int i = 0; i < count; i++)
        {
            if (std::strcmp(fields[i].Key, key) != 0)
                continue;
            switch (fields[i].Type)
            {
            case Field::Int:    *(int*)fields[i].Value = std::atoi(value); break;
            case Field::Bool:   *(bool*)fields[i].Value = std::atoi(value) != 0; break;
            case Field::Float:  *(float*)fields[i].Value = (float)std::atof(value); break;
            case Field::Float3:
            {
                float* v = (float*)fields[i].Value;
                float parsed[3];
                if (std::sscanf(value, "%f %f %f", &parsed[0], &parsed[1], &parsed[2]) == 3)
                    std::memcpy(v, parsed, sizeof(parsed));
                break;
            }
            }
        }
    }
    std::fclose(file);
    Sanitize();
    return true;
}

bool SaveSettings()
{
    if (g_path.empty())
        return false;
    FILE* file = std::fopen(g_path.c_str(), "w");
    if (file == nullptr)
        return false;
    std::fprintf(file, "# The Sirius desktop's settings. Written by the Settings application.\n");
    int count;
    const Field* fields = Fields(count);
    for (int i = 0; i < count; i++)
    {
        switch (fields[i].Type)
        {
        case Field::Int:    std::fprintf(file, "%s = %d\n", fields[i].Key, *(int*)fields[i].Value); break;
        case Field::Bool:   std::fprintf(file, "%s = %d\n", fields[i].Key, *(bool*)fields[i].Value ? 1 : 0); break;
        case Field::Float:  std::fprintf(file, "%s = %.3f\n", fields[i].Key, (double)*(float*)fields[i].Value); break;
        case Field::Float3:
        {
            const float* v = (const float*)fields[i].Value;
            std::fprintf(file, "%s = %.3f %.3f %.3f\n", fields[i].Key, (double)v[0], (double)v[1], (double)v[2]);
            break;
        }
        }
    }
    return std::fclose(file) == 0;
}

void ApplySettings()
{
    Sanitize();
    const Settings& s = g_settings;
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    switch (s.Theme)
    {
    case 1:  ImGui::StyleColorsLight(); break;
    case 2:  ImGui::StyleColorsClassic(); break;
    default: ImGui::StyleColorsDark(); break;
    }

    // Lines are drawn as geometry with exact coverage in the vertex colours.
    // The alternative samples a baked line texture and depends on a GPU's
    // bilinear filter to look right.
    style.AntiAliasedLinesUseTex = false;

    style.FontSizeBase = s.FontSize;
    style.WindowRounding = s.Rounding;
    style.FrameRounding = s.Rounding * 0.75f;
    style.PopupRounding = s.Rounding * 0.75f;
    style.GrabRounding = s.Rounding * 0.75f;
    style.TabRounding = s.Rounding * 0.75f;
    style.ScrollbarRounding = s.Rounding;
    style.WindowBorderSize = 1.0f;
    style.WindowPadding = ImVec2(10.0f, 8.0f);
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;   // no collapse arrow: the panel minimizes

    // The accent colour, at the strengths the stock themes use blue.
    const ImVec4 accent(s.Accent[0], s.Accent[1], s.Accent[2], 1.0f);
    auto tint = [&](ImGuiCol index, float alpha) { style.Colors[index] = ImVec4(accent.x, accent.y, accent.z, alpha); };
    tint(ImGuiCol_Header, 0.31f);            tint(ImGuiCol_HeaderHovered, 0.80f);   tint(ImGuiCol_HeaderActive, 1.00f);
    tint(ImGuiCol_Button, 0.40f);            tint(ImGuiCol_ButtonHovered, 1.00f);
    tint(ImGuiCol_FrameBgHovered, 0.40f);    tint(ImGuiCol_FrameBgActive, 0.67f);
    tint(ImGuiCol_CheckMark, 1.00f);         tint(ImGuiCol_SliderGrabActive, 1.00f);
    tint(ImGuiCol_SeparatorHovered, 0.78f);  tint(ImGuiCol_SeparatorActive, 1.00f);
    tint(ImGuiCol_ResizeGrip, 0.20f);        tint(ImGuiCol_ResizeGripHovered, 0.67f); tint(ImGuiCol_ResizeGripActive, 0.95f);
    tint(ImGuiCol_TabHovered, 0.80f);        tint(ImGuiCol_TextSelectedBg, 0.35f);
    tint(ImGuiCol_NavCursor, 1.00f);         tint(ImGuiCol_PlotHistogram, 0.90f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(accent.x * 0.8f, accent.y * 0.8f, accent.z * 0.8f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(accent.x * 0.9f, accent.y * 0.9f, accent.z * 0.9f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(accent.x * 0.55f, accent.y * 0.55f, accent.z * 0.55f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(accent.x, accent.y, accent.z, 1.0f);
    // Windows are opaque: every translucent pixel is one the rasterizer
    // must blend, over a whole window, every frame.
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;

    ImGui_ImplSirius_SetKeyboardLayout((ImGui_ImplSirius_Layout)s.KeyboardLayout);
    ImGui_ImplSirius_SetMouseSpeed(s.MouseSpeed);

    if (s.Wallpaper != g_applied_wallpaper.Wallpaper ||
        std::memcmp(s.WallpaperTop, g_applied_wallpaper.WallpaperTop, sizeof(s.WallpaperTop)) != 0 ||
        std::memcmp(s.WallpaperBottom, g_applied_wallpaper.WallpaperBottom, sizeof(s.WallpaperBottom)) != 0)
        g_wallpaper_changed = true;
}

namespace { bool g_apply_requested = false; }

void RequestApplySettings() { g_apply_requested = true; }

bool ApplyRequested()
{
    const bool requested = g_apply_requested;
    g_apply_requested = false;
    return requested;
}

bool WallpaperChanged()
{
    const bool changed = g_wallpaper_changed;
    g_wallpaper_changed = false;
    return changed;
}

void RenderWallpaper(unsigned int* pixels, int width, int height)
{
    const Settings& s = g_settings;
    g_applied_wallpaper = s;
    for (int y = 0; y < height; y++)
    {
        // Top to bottom; a solid wallpaper is the top colour throughout.
        const float t = s.Wallpaper == 0 && height > 1 ? (float)y / (float)(height - 1) : 0.0f;
        unsigned int channel[3];
        for (int i = 0; i < 3; i++)
            channel[i] = (unsigned int)((s.WallpaperTop[i] + (s.WallpaperBottom[i] - s.WallpaperTop[i]) * t) * 255.0f + 0.5f);
        unsigned int* row = pixels + (size_t)y * (size_t)width;
        for (int x = 0; x < width; x++)
        {
            // An ordered dither of the last bit hides the banding a smooth
            // gradient shows at eight bits a channel.
            const unsigned int bump = s.Wallpaper == 0 && ((x ^ y) & 1) && channel[2] < 255 ? 1 : 0;
            row[x] = (channel[0] << 16) | (channel[1] << 8) | (channel[2] + bump);
        }
    }
}

} // namespace desktop
