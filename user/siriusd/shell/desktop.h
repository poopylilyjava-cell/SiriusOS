// The Sirius desktop shell: the panel along the top of the screen (launcher,
// running applications, clock) and the windows of the applications under it.
// Everything on screen except the wallpaper is drawn here, with Dear ImGui.
//
// Shared with tests/host_harness, so the same desktop runs in a window on
// Linux.
#pragma once

#include <string>
#include <vector>

struct ImGui_ImplSoftRast_Stats;

namespace desktop {

// Smoothed timings of recent frames, in milliseconds.
struct FrameTimes
{
    float Total = 0.0f;     // start of one frame to the start of the next, sleeps included
    float Build = 0.0f;     // Dear ImGui building its draw lists
    float Raster = 0.0f;    // the software rasterizer
    float Present = 0.0f;   // copying the finished frame to the display
};

// Load the settings from `settings_path` and set up the style. Call once,
// after the Dear ImGui backends are initialised.
void Init(const std::string& settings_path);
void Shutdown();

// Between frames: apply settings changed during the last one, reap finished
// child processes.
void BeginFrame();

// Build one frame: between ImGui::NewFrame() and ImGui::Render().
void Draw(const FrameTimes& times, const ImGui_ImplSoftRast_Stats& stats);

// Descriptors, beside the input device, that the main loop should wake for.
void CollectFds(std::vector<int>& fds);

// How long the main loop may sleep before the next frame when no input
// arrives: short while something on screen is moving, long when only the
// clock is.
int IdleTimeoutMs();

// One line describing the windows, for the serial console: what a test, which
// cannot see the screen, checks.
std::string StatusLine();

} // namespace desktop
