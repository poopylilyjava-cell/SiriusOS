// dear imgui: Platform Backend for Sirius OS.
//
// Feeds Dear ImGui from Sirius input events (<sirius/input.h>: evdev-style
// key codes and mouse motion) and a monotonic clock. Nothing here names a
// window system: the display is one fixed-size surface, the mouse cursor is
// drawn by Dear ImGui itself, and the clipboard lives in this process.
//
// It uses only POSIX and the event record, so it builds unchanged on Linux:
// tests/host_harness turns SDL events into the same records and feeds them
// to ImGui_ImplSirius_ProcessEvent, which is how this file gets debugged.
//
// Implemented features:
//  [X] Platform: Keyboard support, typing with a US or a German layout. [io.AddKeyEvent / io.AddInputCharacter]
//  [X] Platform: Mouse support from relative (mouse) or absolute (tablet) devices, with wheel.
//  [X] Platform: Clipboard support, within the process.
// Missing features:
//  [ ] Platform: Mouse cursor shapes: Dear ImGui draws the cursor (io.MouseDrawCursor).
//  [ ] Platform: Gamepads, IME, multiple viewports.
#pragma once
#include "imgui.h"
#ifndef IMGUI_DISABLE

#include <sirius/input.h>

IMGUI_IMPL_API bool     ImGui_ImplSirius_Init(int display_width, int display_height);
IMGUI_IMPL_API void     ImGui_ImplSirius_Shutdown();
IMGUI_IMPL_API void     ImGui_ImplSirius_NewFrame();

// Hand one event to Dear ImGui.
IMGUI_IMPL_API void     ImGui_ImplSirius_ProcessEvent(const sirius_input_event* event);

// Whoever else wants the events themselves, with their key codes rather than
// what Dear ImGui makes of them: the Wayland compositor, which hands them to
// its clients. Called for each event before Dear ImGui sees it. Null stops it.
typedef void (*ImGui_ImplSirius_EventObserver)(const sirius_input_event* event, void* user_data);
IMGUI_IMPL_API void     ImGui_ImplSirius_SetEventObserver(ImGui_ImplSirius_EventObserver observer, void* user_data);

// Read every event waiting on `fd` -- /dev/input, opened O_NONBLOCK -- and
// process it. Returns how many there were, or -1 if the device failed.
IMGUI_IMPL_API int      ImGui_ImplSirius_PollDevice(int fd);

// Which characters the keys type. Key codes name positions on the keyboard;
// the layout is what is printed on them.
enum ImGui_ImplSirius_Layout
{
    ImGui_ImplSirius_Layout_US,
    ImGui_ImplSirius_Layout_DE,     // German: QWERTZ, umlauts, AltGr for @ { [ ] } \ | ~
    ImGui_ImplSirius_Layout_COUNT
};
IMGUI_IMPL_API void     ImGui_ImplSirius_SetKeyboardLayout(ImGui_ImplSirius_Layout layout);

// Pointer speed: how many pixels one unit of relative mouse motion moves it.
IMGUI_IMPL_API void     ImGui_ImplSirius_SetMouseSpeed(float pixels_per_unit);

// The display changed size (the host harness' window; never on Sirius yet).
IMGUI_IMPL_API void     ImGui_ImplSirius_SetDisplaySize(int width, int height);

#endif // #ifndef IMGUI_DISABLE
