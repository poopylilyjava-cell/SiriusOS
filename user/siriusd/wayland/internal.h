// What the compositor's parts share: the objects behind clients' resources,
// and the functions that cross from one part to another. Not for the rest of
// the desktop, which sees compositor.h.
//
// Every protocol object a client creates is a wl_resource; the C++ object
// behind it is created with it and deleted by its destroy callback, which
// libwayland calls whether the client destroyed it or disconnected. Anything
// that points at an object -- the pointer's focus, a toplevel's surface --
// is cleared in that destructor, through the Forget*() functions here.
#pragma once

#include "compositor.h"

#include "imgui.h"
#include "imgui_impl_softrast.h"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "xdg-decoration-unstable-v1-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wayland {

struct XdgSurface;
struct Toplevel;
struct Popup;
class ToplevelWindow;

wl_display* Display();
// Something changed that is not on screen yet, outside any surface's
// content: a window to map, a popup, a cursor. Draw a frame soon.
void RequestFrame();

// A logging line on the console, prefixed like the rest of siriusd's.
void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));

// --- surfaces (surface.cpp) -------------------------------------------------------

enum class SurfaceRole { None, XdgSurface, Cursor };

// The state wl_surface.commit applies all at once.
struct PendingState
{
    wl_resource* Buffer = nullptr;      // valid while BufferGone has not fired
    bool         Attached = false;      // attach was called: Buffer (maybe null) replaces the content
    int32_t      Dx = 0, Dy = 0;
    wl_list      Frames;                // wl_callback resources, linked through wl_resource_get_link()
};

struct Surface
{
    wl_resource* Resource;
    wl_client*   Client;
    PendingState Pending;
    struct BufferListener { wl_listener Listener; Surface* Owner; } BufferGone;

    // What is on screen: a copy of the last buffer committed. The client's
    // buffer is released as soon as it has been copied, so it may draw the
    // next frame into it straight away.
    std::vector<uint32_t> Pixels;
    int  Width = 0, Height = 0;
    bool HasContent = false;
    ImGui_ImplSoftRast_Texture Texture{};
    // Frame callbacks committed, to be answered when a frame shows them.
    wl_list Frames;

    SurfaceRole Role = SurfaceRole::None;
    XdgSurface* Xdg = nullptr;

    ImVec2 ScreenPos;           // where its (0, 0) was drawn, in the frame being built
    bool Shown = false;         // drawn in the frame being built
    bool ShownLastFrame = false;
    bool NewContent = false;    // committed since the last frame
};

Surface* SurfaceFrom(wl_resource* resource);
// Every live surface, in creation order.
const std::vector<Surface*>& Surfaces();
// The texture to draw `surface` with, or null if it has nothing to show.
const ImGui_ImplSoftRast_Texture* TextureOf(Surface* surface);
bool CreateCompositorGlobals(wl_display* display);

// --- xdg-shell (xdg.cpp) ------------------------------------------------------------

struct Rect { int X = 0, Y = 0, W = 0, H = 0; };

struct XdgSurface
{
    wl_resource* Resource;
    Surface*     Base;                  // null once the client has destroyed it, out of order
    Toplevel*    AsToplevel = nullptr;  // the role it was given, if any
    Popup*       AsPopup = nullptr;
    bool  HasGeometry = false, PendingHasGeometry = false;
    Rect  Geometry, PendingGeometry;
    uint32_t LastSerial = 0;    // of the last configure sent
    bool  Acked = false;        // the client has acknowledged a configure: it may attach a buffer
    bool  SentFirstConfigure = false;
    // Where the window geometry's top-left corner was drawn, in the frame
    // being built: popups are placed relative to it.
    ImVec2 ScreenOrigin;
    bool   OnScreen = false;

    // The part of the surface that is the window, in surface coordinates:
    // the geometry the client set, or the whole buffer.
    Rect WindowGeometry() const;
};

struct Toplevel
{
    wl_resource* Resource;
    XdgSurface*  Xdg;
    std::string  Title, AppId;
    int  MinW = 0, MinH = 0, MaxW = 0, MaxH = 0;
    bool Maximized = false, Fullscreen = false, Activated = false;
    int  ConfiguredW = 0, ConfiguredH = 0;     // the size last asked for; 0 lets the client choose
    int  RestoreW = 0, RestoreH = 0;           // the size before it was maximized or made fullscreen
    bool Mapped = false;
    ToplevelWindow* Window = nullptr;          // the desktop's window, while mapped
    wl_resource* Decoration = nullptr;         // zxdg_toplevel_decoration_v1
    bool MinimizeRequested = false;
    bool MoveRequested = false;                // xdg_toplevel.move: follow the pointer until the button comes up
};

struct Popup
{
    wl_resource* Resource;
    XdgSurface*  Xdg;
    XdgSurface*  Parent;
    Rect  Placement;            // relative to the parent's window geometry
    bool  Grab = false;
    bool  Mapped = false;
    int   Serial;               // for its Dear ImGui window's name
    ImVec2 ScreenOrigin;        // where its window geometry was drawn, last frame
    bool   Hovered = false;
};

// Send a configure for a toplevel at this size (0x0: the client's choice),
// with its states as they are now.
void ConfigureToplevel(Toplevel* toplevel, int width, int height);
// Let go of a toplevel's desktop window: the window is closing, or the
// toplevel is.
void DetachWindow(Toplevel* toplevel);
// A commit on a surface with the xdg_surface role.
void XdgSurfaceCommitted(XdgSurface* xdg);
// Dear ImGui windows for the mapped popups, above everything else.
void DrawPopups();
const std::vector<Popup*>& Popups();
// Close every popup of this client that grabs: the user clicked elsewhere.
void DismissPopups(wl_client* client);
bool CreateXdgGlobals(wl_display* display);
// The root toplevel of a popup chain, or the toplevel itself.
Toplevel* RootToplevel(XdgSurface* xdg);

// --- the desktop's window for a toplevel (window.cpp) -----------------------------------

// Make a desktop window for a toplevel that has just been mapped.
void OpenWindow(Toplevel* toplevel);

// --- input (seat.cpp) ----------------------------------------------------------------

bool CreateSeatGlobals(wl_display* display);
// What a frame found under the pointer, reported by the window drawing it
// (which has set the surface's ScreenPos).
void OfferPointer(Surface* surface);
// And which surface the focused window shows.
void OfferKeyboard(Surface* surface);
// Settle focus and send input events, once the frame's windows are built.
void UpdateInput();
// Draw the cursor a client asked for, if the pointer is over it.
void DrawCursor();
void ForgetSurfaceInSeat(Surface* surface);
// Is a mouse button held? (An xdg_toplevel.move is only honoured if so.)
bool PointerButtonDown();
uint32_t NextSerial();

// --- data devices (data.cpp) ------------------------------------------------------------

bool CreateDataDeviceGlobals(wl_display* display);

} // namespace wayland
