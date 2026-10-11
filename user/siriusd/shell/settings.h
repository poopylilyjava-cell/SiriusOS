// What the user can change about the desktop, and where it is kept.
#pragma once

#include <string>

namespace desktop {

struct Settings
{
    int   Theme = 0;                // 0 dark, 1 light, 2 classic
    float Accent[3] = { 0.26f, 0.59f, 0.98f };
    float FontSize = 13.0f;         // pixels
    float Rounding = 4.0f;          // of windows and controls
    int   Wallpaper = 0;            // 0 gradient, 1 solid
    float WallpaperTop[3] = { 0.10f, 0.14f, 0.24f };
    float WallpaperBottom[3] = { 0.03f, 0.04f, 0.07f };
    int   KeyboardLayout = 0;       // ImGui_ImplSirius_Layout
    float MouseSpeed = 1.0f;
    bool  ShowSeconds = true;       // in the panel's clock
};

Settings& GetSettings();

// Read and write the settings file: lines of "key = value". Loading a file
// that is missing or damaged leaves the defaults.
bool LoadSettings(const std::string& path);
bool SaveSettings();

// Make Dear ImGui and the input backend match the settings. Call between
// frames -- it replaces the style -- after changing any of them.
void ApplySettings();
// Ask for that from inside a frame: the desktop does it before the next one.
void RequestApplySettings();
bool ApplyRequested();

// Paint the wallpaper into a width x height XRGB buffer. The desktop's main
// loop starts every frame from a copy of it.
void RenderWallpaper(unsigned int* pixels, int width, int height);
// True once after the wallpaper settings change: time to render it again.
bool WallpaperChanged();

} // namespace desktop
