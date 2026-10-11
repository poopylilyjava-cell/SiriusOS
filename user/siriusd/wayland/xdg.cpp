// xdg-shell: what makes a surface a window (xdg_toplevel) or a menu
// (xdg_popup), and xdg-decoration, by which a client agrees to let the
// desktop draw its title bar.
//
// The lifecycle of an xdg_surface, which everything here follows:
//   1. the client gives it a role and commits with no buffer;
//   2. the compositor answers with a configure: a size, and a serial;
//   3. the client acknowledges the serial, attaches a buffer and commits:
//      the surface is mapped -- a desktop window, or a popup on screen;
//   4. committing a null buffer unmaps it, and it starts again at 1.
#include "internal.h"

#include "imgui_internal.h"     // BringWindowToDisplayFront: popups stay above the window they belong to

#include <algorithm>

namespace wayland {
namespace {

std::vector<Popup*> g_popups;
int g_next_popup = 1;

// --- xdg_positioner: the rules for where a popup goes ------------------------------------

struct Positioner
{
    int W = 0, H = 0;
    Rect Anchor;
    uint32_t AnchorEdge = XDG_POSITIONER_ANCHOR_NONE;
    uint32_t Gravity = XDG_POSITIONER_GRAVITY_NONE;
    uint32_t Adjustment = 0;
    int OffsetX = 0, OffsetY = 0;
};

Positioner* PositionerFrom(wl_resource* resource) { return static_cast<Positioner*>(wl_resource_get_user_data(resource)); }

void PositionerDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
void PositionerSetSize(wl_client*, wl_resource* resource, int32_t w, int32_t h)
{
    if (w <= 0 || h <= 0) { wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "size must be positive"); return; }
    PositionerFrom(resource)->W = w;
    PositionerFrom(resource)->H = h;
}
void PositionerSetAnchorRect(wl_client*, wl_resource* resource, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (w < 0 || h < 0) { wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "negative anchor rectangle"); return; }
    PositionerFrom(resource)->Anchor = { x, y, w, h };
}
void PositionerSetAnchor(wl_client*, wl_resource* resource, uint32_t anchor) { PositionerFrom(resource)->AnchorEdge = anchor; }
void PositionerSetGravity(wl_client*, wl_resource* resource, uint32_t gravity) { PositionerFrom(resource)->Gravity = gravity; }
void PositionerSetAdjustment(wl_client*, wl_resource* resource, uint32_t adjustment) { PositionerFrom(resource)->Adjustment = adjustment; }
void PositionerSetOffset(wl_client*, wl_resource* resource, int32_t x, int32_t y)
{
    PositionerFrom(resource)->OffsetX = x;
    PositionerFrom(resource)->OffsetY = y;
}
void PositionerSetReactive(wl_client*, wl_resource*) {}
void PositionerSetParentSize(wl_client*, wl_resource*, int32_t, int32_t) {}
void PositionerSetParentConfigure(wl_client*, wl_resource*, uint32_t) {}

const struct xdg_positioner_interface kPositionerImpl = {
    PositionerDestroy, PositionerSetSize, PositionerSetAnchorRect, PositionerSetAnchor, PositionerSetGravity,
    PositionerSetAdjustment, PositionerSetOffset, PositionerSetReactive, PositionerSetParentSize, PositionerSetParentConfigure,
};

// Where a popup goes, relative to its parent's window geometry. Constraints
// are met by sliding it back onto the screen, whatever adjustments the
// client allowed: flipping a menu is a refinement for later.
Rect Place(const Positioner& p, ImVec2 parent_origin)
{
    const Rect& a = p.Anchor;
    int x = a.X + a.W / 2, y = a.Y + a.H / 2;   // the anchor point
    switch (p.AnchorEdge)
    {
    case XDG_POSITIONER_ANCHOR_TOP:          y = a.Y; break;
    case XDG_POSITIONER_ANCHOR_BOTTOM:       y = a.Y + a.H; break;
    case XDG_POSITIONER_ANCHOR_LEFT:         x = a.X; break;
    case XDG_POSITIONER_ANCHOR_RIGHT:        x = a.X + a.W; break;
    case XDG_POSITIONER_ANCHOR_TOP_LEFT:     x = a.X; y = a.Y; break;
    case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:  x = a.X; y = a.Y + a.H; break;
    case XDG_POSITIONER_ANCHOR_TOP_RIGHT:    x = a.X + a.W; y = a.Y; break;
    case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: x = a.X + a.W; y = a.Y + a.H; break;
    default: break;
    }
    // Gravity: the direction the popup extends in from the anchor point.
    int left = x - p.W / 2, top = y - p.H / 2;
    switch (p.Gravity)
    {
    case XDG_POSITIONER_GRAVITY_TOP:          top = y - p.H; break;
    case XDG_POSITIONER_GRAVITY_BOTTOM:       top = y; break;
    case XDG_POSITIONER_GRAVITY_LEFT:         left = x - p.W; break;
    case XDG_POSITIONER_GRAVITY_RIGHT:        left = x; break;
    case XDG_POSITIONER_GRAVITY_TOP_LEFT:     left = x - p.W; top = y - p.H; break;
    case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:  left = x - p.W; top = y; break;
    case XDG_POSITIONER_GRAVITY_TOP_RIGHT:    left = x; top = y - p.H; break;
    case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT: left = x; top = y; break;
    default: break;
    }
    left += p.OffsetX;
    top += p.OffsetY;

    const ImVec2 screen = ImGui::GetMainViewport()->Size;
    const float right_overflow = parent_origin.x + (float)(left + p.W) - screen.x;
    const float bottom_overflow = parent_origin.y + (float)(top + p.H) - screen.y;
    if (right_overflow > 0.0f) left -= (int)right_overflow;
    if (bottom_overflow > 0.0f) top -= (int)bottom_overflow;
    if (parent_origin.x + (float)left < 0.0f) left = -(int)parent_origin.x;
    if (parent_origin.y + (float)top < 0.0f) top = -(int)parent_origin.y;
    return { left, top, p.W, p.H };
}

// --- xdg_toplevel ----------------------------------------------------------------------

Toplevel* ToplevelFrom(wl_resource* resource) { return static_cast<Toplevel*>(wl_resource_get_user_data(resource)); }

void ToplevelDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
void ToplevelSetParent(wl_client*, wl_resource*, wl_resource*) {}
void ToplevelSetTitle(wl_client*, wl_resource* resource, const char* title) { ToplevelFrom(resource)->Title = title; }
void ToplevelSetAppId(wl_client*, wl_resource* resource, const char* app_id) { ToplevelFrom(resource)->AppId = app_id; }
void ToplevelShowWindowMenu(wl_client*, wl_resource*, wl_resource*, uint32_t, int32_t, int32_t) {}

void ToplevelMove(wl_client*, wl_resource* resource, wl_resource*, uint32_t)
{
    // Only while the button that started it is still down; a stale request
    // would move the window under a pointer the user is not holding.
    if (PointerButtonDown())
        ToplevelFrom(resource)->MoveRequested = true;
}

void ToplevelResize(wl_client*, wl_resource*, wl_resource*, uint32_t, uint32_t) {}

void ToplevelSetMaxSize(wl_client*, wl_resource* resource, int32_t w, int32_t h)
{
    if (w < 0 || h < 0) { wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE, "negative maximum size"); return; }
    ToplevelFrom(resource)->MaxW = w;
    ToplevelFrom(resource)->MaxH = h;
}

void ToplevelSetMinSize(wl_client*, wl_resource* resource, int32_t w, int32_t h)
{
    if (w < 0 || h < 0) { wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE, "negative minimum size"); return; }
    ToplevelFrom(resource)->MinW = w;
    ToplevelFrom(resource)->MinH = h;
}

// The size a maximized or fullscreen window's contents get.
void StateSize(const Toplevel* toplevel, int& width, int& height)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (toplevel->Fullscreen)
    {
        width = (int)viewport->Size.x;
        height = (int)viewport->Size.y;
        return;
    }
    // The work area, less the title bar and the borders the window will have.
    const ImGuiStyle& style = ImGui::GetStyle();
    width = (int)(viewport->WorkSize.x - 2.0f * style.WindowBorderSize);
    height = (int)(viewport->WorkSize.y - ImGui::GetFrameHeight() - style.WindowBorderSize);
}

// The size to go back to on leaving maximized or fullscreen.
struct SavedSize { int W, H; };

void Reconfigure(Toplevel* toplevel, int width, int height)
{
    if (toplevel->Xdg != nullptr && toplevel->Xdg->SentFirstConfigure)
        ConfigureToplevel(toplevel, width, height);
}

void EnterState(Toplevel* toplevel, bool& state)
{
    if (state)
        return;
    if (!toplevel->Maximized && !toplevel->Fullscreen && toplevel->Xdg != nullptr && toplevel->Xdg->Base != nullptr)
    {
        const Rect geometry = toplevel->Xdg->WindowGeometry();
        toplevel->RestoreW = geometry.W;
        toplevel->RestoreH = geometry.H;
    }
    state = true;
    int width, height;
    StateSize(toplevel, width, height);
    Reconfigure(toplevel, width, height);
}

void LeaveState(Toplevel* toplevel, bool& state)
{
    if (!state)
        return;
    state = false;
    if (toplevel->Maximized || toplevel->Fullscreen)
    {
        int width, height;
        StateSize(toplevel, width, height);
        Reconfigure(toplevel, width, height);
    }
    else
        Reconfigure(toplevel, toplevel->RestoreW, toplevel->RestoreH);
}

void ToplevelSetMaximized(wl_client*, wl_resource* resource) { EnterState(ToplevelFrom(resource), ToplevelFrom(resource)->Maximized); }
void ToplevelUnsetMaximized(wl_client*, wl_resource* resource) { LeaveState(ToplevelFrom(resource), ToplevelFrom(resource)->Maximized); }
void ToplevelSetFullscreen(wl_client*, wl_resource* resource, wl_resource*) { EnterState(ToplevelFrom(resource), ToplevelFrom(resource)->Fullscreen); }
void ToplevelUnsetFullscreen(wl_client*, wl_resource* resource) { LeaveState(ToplevelFrom(resource), ToplevelFrom(resource)->Fullscreen); }
void ToplevelSetMinimized(wl_client*, wl_resource* resource) { ToplevelFrom(resource)->MinimizeRequested = true; RequestFrame(); }

const struct xdg_toplevel_interface kToplevelImpl = {
    ToplevelDestroy, ToplevelSetParent, ToplevelSetTitle, ToplevelSetAppId, ToplevelShowWindowMenu,
    ToplevelMove, ToplevelResize, ToplevelSetMaxSize, ToplevelSetMinSize, ToplevelSetMaximized,
    ToplevelUnsetMaximized, ToplevelSetFullscreen, ToplevelUnsetFullscreen, ToplevelSetMinimized,
};

void ToplevelResourceDestroyed(wl_resource* resource)
{
    Toplevel* toplevel = ToplevelFrom(resource);
    DetachWindow(toplevel);
    if (toplevel->Xdg != nullptr)
        toplevel->Xdg->AsToplevel = nullptr;
    if (toplevel->Decoration != nullptr)
        wl_resource_set_user_data(toplevel->Decoration, nullptr);
    delete toplevel;
}

// --- xdg_popup -------------------------------------------------------------------------------

Popup* PopupFrom(wl_resource* resource) { return static_cast<Popup*>(wl_resource_get_user_data(resource)); }

void PopupDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void PopupGrab(wl_client*, wl_resource* resource, wl_resource*, uint32_t) { PopupFrom(resource)->Grab = true; }

void ConfigurePopup(Popup* popup)
{
    xdg_popup_send_configure(popup->Resource, popup->Placement.X, popup->Placement.Y, popup->Placement.W, popup->Placement.H);
    popup->Xdg->LastSerial = NextSerial();
    xdg_surface_send_configure(popup->Xdg->Resource, popup->Xdg->LastSerial);
}

ImVec2 ParentOrigin(const XdgSurface* parent) { return parent != nullptr ? parent->ScreenOrigin : ImVec2(0.0f, 0.0f); }

void PopupReposition(wl_client*, wl_resource* resource, wl_resource* positioner, uint32_t token)
{
    Popup* popup = PopupFrom(resource);
    popup->Placement = Place(*PositionerFrom(positioner), ParentOrigin(popup->Parent));
    xdg_popup_send_repositioned(resource, token);
    ConfigurePopup(popup);
}

const struct xdg_popup_interface kPopupImpl = { PopupDestroy, PopupGrab, PopupReposition };

void PopupResourceDestroyed(wl_resource* resource)
{
    Popup* popup = PopupFrom(resource);
    if (popup->Xdg != nullptr)
        popup->Xdg->AsPopup = nullptr;
    g_popups.erase(std::find(g_popups.begin(), g_popups.end(), popup));
    RequestFrame();
    delete popup;
}

// --- xdg_surface -----------------------------------------------------------------------------

XdgSurface* XdgFrom(wl_resource* resource) { return static_cast<XdgSurface*>(wl_resource_get_user_data(resource)); }

void XdgDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void XdgGetToplevel(wl_client* client, wl_resource* resource, uint32_t id)
{
    XdgSurface* xdg = XdgFrom(resource);
    if (xdg->AsToplevel != nullptr || xdg->AsPopup != nullptr)
    {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "the xdg_surface already has a role");
        return;
    }
    wl_resource* toplevel_resource = wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    if (toplevel_resource == nullptr) { wl_client_post_no_memory(client); return; }
    auto* toplevel = new Toplevel();
    toplevel->Resource = toplevel_resource;
    toplevel->Xdg = xdg;
    xdg->AsToplevel = toplevel;
    wl_resource_set_implementation(toplevel_resource, &kToplevelImpl, toplevel, ToplevelResourceDestroyed);
}

void XdgGetPopup(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* parent, wl_resource* positioner)
{
    XdgSurface* xdg = XdgFrom(resource);
    if (xdg->AsToplevel != nullptr || xdg->AsPopup != nullptr)
    {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "the xdg_surface already has a role");
        return;
    }
    wl_resource* popup_resource = wl_resource_create(client, &xdg_popup_interface, wl_resource_get_version(resource), id);
    if (popup_resource == nullptr) { wl_client_post_no_memory(client); return; }
    auto* popup = new Popup();
    popup->Resource = popup_resource;
    popup->Xdg = xdg;
    popup->Parent = parent != nullptr ? XdgFrom(parent) : nullptr;
    popup->Serial = g_next_popup++;
    popup->Placement = Place(*PositionerFrom(positioner), ParentOrigin(popup->Parent));
    xdg->AsPopup = popup;
    wl_resource_set_implementation(popup_resource, &kPopupImpl, popup, PopupResourceDestroyed);
    g_popups.push_back(popup);
}

void XdgSetWindowGeometry(wl_client*, wl_resource* resource, int32_t x, int32_t y, int32_t w, int32_t h)
{
    XdgSurface* xdg = XdgFrom(resource);
    if (w <= 0 || h <= 0)
    {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SIZE, "window geometry must be positive");
        return;
    }
    xdg->PendingGeometry = { x, y, w, h };
    xdg->PendingHasGeometry = true;
}

void XdgAckConfigure(wl_client*, wl_resource* resource, uint32_t serial)
{
    XdgSurface* xdg = XdgFrom(resource);
    if (!xdg->SentFirstConfigure)
    {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL, "no configure has been sent");
        return;
    }
    (void)serial;
    xdg->Acked = true;
}

const struct xdg_surface_interface kXdgSurfaceImpl = {
    XdgDestroy, XdgGetToplevel, XdgGetPopup, XdgSetWindowGeometry, XdgAckConfigure,
};

void XdgResourceDestroyed(wl_resource* resource)
{
    XdgSurface* xdg = XdgFrom(resource);
    if (xdg->AsToplevel != nullptr)
    {
        DetachWindow(xdg->AsToplevel);
        xdg->AsToplevel->Xdg = nullptr;
    }
    if (xdg->AsPopup != nullptr)
        xdg->AsPopup->Xdg = nullptr;
    for (Popup* popup : g_popups)
        if (popup->Parent == xdg)
            popup->Parent = nullptr;
    if (xdg->Base != nullptr)
    {
        xdg->Base->Xdg = nullptr;
        xdg->Base->Role = SurfaceRole::None;
    }
    delete xdg;
}

// --- xdg_wm_base -------------------------------------------------------------------------------

void WmDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void WmCreatePositioner(wl_client* client, wl_resource* resource, uint32_t id)
{
    wl_resource* positioner = wl_resource_create(client, &xdg_positioner_interface, wl_resource_get_version(resource), id);
    if (positioner == nullptr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(positioner, &kPositionerImpl, new Positioner(),
                                   [](wl_resource* r) { delete PositionerFrom(r); });
}

void WmGetXdgSurface(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surface_resource)
{
    Surface* surface = SurfaceFrom(surface_resource);
    if (surface->Role != SurfaceRole::None || surface->HasContent)
    {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE, "the surface already has a role, or content");
        return;
    }
    wl_resource* xdg_resource = wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    if (xdg_resource == nullptr) { wl_client_post_no_memory(client); return; }
    auto* xdg = new XdgSurface();
    xdg->Resource = xdg_resource;
    xdg->Base = surface;
    surface->Role = SurfaceRole::XdgSurface;
    surface->Xdg = xdg;
    wl_resource_set_implementation(xdg_resource, &kXdgSurfaceImpl, xdg, XdgResourceDestroyed);
}

void WmPong(wl_client*, wl_resource*, uint32_t) {}

const struct xdg_wm_base_interface kWmBaseImpl = { WmDestroy, WmCreatePositioner, WmGetXdgSurface, WmPong };

void BindWmBase(wl_client* client, void*, uint32_t version, uint32_t id)
{
    wl_resource* resource = wl_resource_create(client, &xdg_wm_base_interface, (int)version, id);
    if (resource == nullptr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &kWmBaseImpl, nullptr, nullptr);
}

// --- xdg-decoration: the desktop draws every window's frame ----------------------------------

void DecorationDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

// Whatever the client would like, the answer is the same.
void DecorationAnswer(wl_resource* resource)
{
    zxdg_toplevel_decoration_v1_send_configure(resource, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    auto* toplevel = static_cast<Toplevel*>(wl_resource_get_user_data(resource));
    if (toplevel != nullptr)
        Reconfigure(toplevel, toplevel->ConfiguredW, toplevel->ConfiguredH);
}

void DecorationSetMode(wl_client*, wl_resource* resource, uint32_t) { DecorationAnswer(resource); }
void DecorationUnsetMode(wl_client*, wl_resource* resource) { DecorationAnswer(resource); }

const struct zxdg_toplevel_decoration_v1_interface kDecorationImpl = { DecorationDestroy, DecorationSetMode, DecorationUnsetMode };

void DecorationManagerDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void DecorationManagerGet(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* toplevel_resource)
{
    Toplevel* toplevel = ToplevelFrom(toplevel_resource);
    if (toplevel->Decoration != nullptr)
    {
        wl_resource_post_error(resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED, "the toplevel already has a decoration object");
        return;
    }
    wl_resource* decoration = wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface, wl_resource_get_version(resource), id);
    if (decoration == nullptr) { wl_client_post_no_memory(client); return; }
    toplevel->Decoration = decoration;
    wl_resource_set_implementation(decoration, &kDecorationImpl, toplevel, [](wl_resource* r) {
        if (auto* owner = static_cast<Toplevel*>(wl_resource_get_user_data(r))) owner->Decoration = nullptr;
    });
    DecorationAnswer(decoration);
}

const struct zxdg_decoration_manager_v1_interface kDecorationManagerImpl = { DecorationManagerDestroy, DecorationManagerGet };

void BindDecorationManager(wl_client* client, void*, uint32_t version, uint32_t id)
{
    wl_resource* resource = wl_resource_create(client, &zxdg_decoration_manager_v1_interface, (int)version, id);
    if (resource == nullptr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &kDecorationManagerImpl, nullptr, nullptr);
}

void Unmap(XdgSurface* xdg)
{
    if (xdg->AsToplevel != nullptr)
    {
        xdg->AsToplevel->Mapped = false;
        DetachWindow(xdg->AsToplevel);
    }
    if (xdg->AsPopup != nullptr)
        xdg->AsPopup->Mapped = false;
    // A surface that is mapped again starts from the initial commit.
    xdg->SentFirstConfigure = false;
    xdg->Acked = false;
    RequestFrame();
}

} // namespace

Rect XdgSurface::WindowGeometry() const
{
    if (HasGeometry)
        return Geometry;
    return { 0, 0, Base != nullptr ? Base->Width : 0, Base != nullptr ? Base->Height : 0 };
}

void ConfigureToplevel(Toplevel* toplevel, int width, int height)
{
    if (toplevel->Xdg == nullptr)
        return;
    wl_array states;
    wl_array_init(&states);
    auto add = [&](uint32_t state) {
        if (auto* slot = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)))) *slot = state;
    };
    if (toplevel->Maximized) add(XDG_TOPLEVEL_STATE_MAXIMIZED);
    if (toplevel->Fullscreen) add(XDG_TOPLEVEL_STATE_FULLSCREEN);
    if (toplevel->Activated) add(XDG_TOPLEVEL_STATE_ACTIVATED);
    xdg_toplevel_send_configure(toplevel->Resource, width, height, &states);
    wl_array_release(&states);
    toplevel->ConfiguredW = width;
    toplevel->ConfiguredH = height;
    toplevel->Xdg->LastSerial = NextSerial();
    xdg_surface_send_configure(toplevel->Xdg->Resource, toplevel->Xdg->LastSerial);
}

void XdgSurfaceCommitted(XdgSurface* xdg)
{
    if (xdg->PendingHasGeometry)
    {
        xdg->Geometry = xdg->PendingGeometry;
        xdg->HasGeometry = true;
    }
    Surface* surface = xdg->Base;
    if (xdg->AsToplevel == nullptr && xdg->AsPopup == nullptr)
        return;   // no role yet: nothing to configure

    if (!xdg->SentFirstConfigure)
    {
        if (surface->HasContent)
        {
            wl_resource_post_error(xdg->Resource, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER, "a buffer was committed before the first configure");
            return;
        }
        xdg->SentFirstConfigure = true;
        if (Toplevel* toplevel = xdg->AsToplevel)
        {
            if (wl_resource_get_version(toplevel->Resource) >= XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
            {
                wl_array capabilities;
                wl_array_init(&capabilities);
                for (const uint32_t capability : { XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE, XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN,
                                                   XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE })
                    if (auto* slot = static_cast<uint32_t*>(wl_array_add(&capabilities, sizeof(uint32_t)))) *slot = capability;
                xdg_toplevel_send_wm_capabilities(toplevel->Resource, &capabilities);
                wl_array_release(&capabilities);
            }
            int width = toplevel->ConfiguredW, height = toplevel->ConfiguredH;
            if (toplevel->Maximized || toplevel->Fullscreen)
                StateSize(toplevel, width, height);
            ConfigureToplevel(toplevel, width, height);
        }
        else
            ConfigurePopup(xdg->AsPopup);
        return;
    }

    if (surface->HasContent && !xdg->Acked)
    {
        wl_resource_post_error(xdg->Resource, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER, "a buffer was committed before a configure was acknowledged");
        return;
    }
    if (Toplevel* toplevel = xdg->AsToplevel)
    {
        if (surface->HasContent && !toplevel->Mapped)
        {
            toplevel->Mapped = true;
            OpenWindow(toplevel);
            RequestFrame();
        }
        else if (!surface->HasContent && toplevel->Mapped)
            Unmap(xdg);
    }
    else if (Popup* popup = xdg->AsPopup)
    {
        if (surface->HasContent && !popup->Mapped)
        {
            popup->Mapped = true;
            RequestFrame();
        }
        else if (!surface->HasContent && popup->Mapped)
            Unmap(xdg);
    }
}

Toplevel* RootToplevel(XdgSurface* xdg)
{
    for (int depth = 0; xdg != nullptr && depth < 32; depth++)
    {
        if (xdg->AsToplevel != nullptr) return xdg->AsToplevel;
        if (xdg->AsPopup == nullptr) return nullptr;
        xdg = xdg->AsPopup->Parent;
    }
    return nullptr;
}

const std::vector<Popup*>& Popups() { return g_popups; }

void DismissPopups(wl_client* client)
{
    // Topmost first, as the protocol asks.
    for (auto it = g_popups.rbegin(); it != g_popups.rend(); ++it)
    {
        Popup* popup = *it;
        if (popup->Grab && popup->Mapped && wl_resource_get_client(popup->Resource) == client)
        {
            xdg_popup_send_popup_done(popup->Resource);
            popup->Mapped = false;   // it is gone from the screen now, whatever the client does next
            popup->Grab = false;
        }
    }
    RequestFrame();
}

void DrawPopups()
{
    for (Popup* popup : g_popups)
    {
        popup->Hovered = false;
        XdgSurface* xdg = popup->Xdg;
        if (!popup->Mapped || xdg == nullptr || xdg->Base == nullptr || popup->Parent == nullptr || !popup->Parent->OnScreen)
            continue;
        const ImGui_ImplSoftRast_Texture* texture = TextureOf(xdg->Base);
        if (texture == nullptr)
            continue;

        // The popup's window geometry goes where the positioner put it;
        // the surface is offset from that by the geometry's own origin.
        const Rect geometry = xdg->WindowGeometry();
        const ImVec2 origin(popup->Parent->ScreenOrigin.x + (float)popup->Placement.X,
                            popup->Parent->ScreenOrigin.y + (float)popup->Placement.Y);
        const ImVec2 surface_origin(origin.x - (float)geometry.X, origin.y - (float)geometry.Y);
        const std::string name = "##wayland-popup-" + std::to_string(popup->Serial);
        ImGui::SetNextWindowPos(origin);
        ImGui::SetNextWindowSize(ImVec2((float)geometry.W, (float)geometry.H));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin(name.c_str(), nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar(3);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)texture, surface_origin,
                                             ImVec2(surface_origin.x + (float)texture->Width, surface_origin.y + (float)texture->Height));
        xdg->ScreenOrigin = origin;
        xdg->OnScreen = true;
        xdg->Base->ScreenPos = surface_origin;
        xdg->Base->Shown = true;
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        {
            popup->Hovered = true;
            OfferPointer(xdg->Base);
        }
        // Clicking a menu gives its Dear ImGui window the focus; the
        // keyboard stays with the window the menu belongs to.
        if (ImGui::IsWindowFocused())
            if (Toplevel* root = RootToplevel(xdg); root != nullptr && root->Xdg != nullptr && root->Xdg->Base != nullptr)
                OfferKeyboard(root->Xdg->Base);
        ImGui::End();
    }
}

bool CreateXdgGlobals(wl_display* display)
{
    // Version 5: wm_capabilities, which it sends. Later versions add
    // states (suspended, constrained) that it would have to manage.
    return wl_global_create(display, &xdg_wm_base_interface, 5, nullptr, BindWmBase) != nullptr &&
           wl_global_create(display, &zxdg_decoration_manager_v1_interface, 1, nullptr, BindDecorationManager) != nullptr;
}

} // namespace wayland
