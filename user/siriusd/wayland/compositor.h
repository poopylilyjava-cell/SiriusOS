// The Wayland compositor inside siriusd (ADR 0013): the server side of the
// protocol, built on libwayland-server, for programs that draw their own
// windows.
//
// A client's window becomes one more window of the desktop (shell/app.h),
// decorated by Dear ImGui like the built-in applications, with the client's
// pixels inside it. Input reaches a client through Dear ImGui's view of the
// world: the pointer when it is over the client's part of a window that Dear
// ImGui says is under it, the keyboard when that window has the focus.
//
// Everything runs on siriusd's one thread, between and inside its frames:
//
//   CollectFds()     the descriptor to poll() for clients' requests
//   Dispatch()       before a frame: handle whatever requests have arrived
//   EndFrame()       after the windows are built: pointer and keyboard focus
//   FramePresented() after the frame is on screen: frame callbacks, flush
#pragma once

#include <sirius/input.h>

#include <string>
#include <vector>

namespace wayland {

// Open a listening socket in $XDG_RUNTIME_DIR (made, if unset, under /tmp)
// and export WAYLAND_DISPLAY for the programs the desktop starts. The display
// is described to clients as one output of this size. False if there can be
// no compositor; the desktop runs without one.
bool Start(int display_width, int display_height);
void Stop();
bool Running();

// The socket's name ("wayland-0"), for the status line and the About box.
const char* SocketName();

void CollectFds(std::vector<int>& fds);
void Dispatch();
void EndFrame();
void FramePresented();

// How long the desktop may sleep before it has to draw again for a client:
// 0 when a client has committed something not yet on screen, a frame's
// length when one is waiting for a frame callback, -1 for no limit.
int FrameDeadlineMs();

// A raw input event, before Dear ImGui sees it: key presses go to the
// client with the keyboard focus, as the key codes they are.
void HandleInputEvent(const sirius_input_event& event);

// The keyboard layout clients are told about, as the desktop's settings
// number them (ImGui_ImplSirius_Layout). Clients get the new keymap at once.
void SetKeyboardLayout(int layout);

// How many clients are connected, for the status line.
int ClientCount();

} // namespace wayland
