// An application running inside siriusd: one window, drawn with Dear ImGui.
//
// The desktop owns the window -- title bar, position, focus, the close
// button -- and the application draws what is inside it. Most are compiled
// into the desktop (apps/); a Wayland client's window is one too, whose
// contents are the client's pixels (wayland/window.cpp).
#pragma once

#include "imgui.h"

#include <string>
#include <vector>

namespace desktop {

class App
{
public:
    virtual ~App() = default;

    // The window's title. May change from frame to frame.
    virtual std::string Title() const = 0;
    // The window's contents; called between Begin() and End() while the
    // window is open and not collapsed.
    virtual void Draw() = 0;
    // Called every frame regardless: for work that does not wait to be
    // looked at, like collecting a child process's output.
    virtual void Update() {}
    // Descriptors whose becoming readable should wake the desktop from its
    // idle sleep.
    virtual void CollectFds(std::vector<int>&) {}
    // False once the application has nothing more to show and its window
    // should go (the user is asked nothing).
    virtual bool WantsToStayOpen() const { return true; }

    virtual ImVec2 DefaultSize() const { return ImVec2(640.0f, 420.0f); }
    virtual ImGuiWindowFlags WindowFlags() const { return 0; }

    // Just before and just after the window's ImGui::Begin(), every frame it
    // is open and not minimized: the place for SetNextWindow*(), and for
    // style changes that Begin() must see and nothing after it.
    virtual void BeforeBegin() {}
    virtual void AfterBegin() {}
    // The user pressed the close button. True closes the window now; false
    // leaves it to the application, which says so through WantsToStayOpen().
    virtual bool CloseRequested() { return true; }
    // True, once, when the application wants its window minimized.
    virtual bool TakeMinimizeRequest() { return false; }
    // The name the status line gives the window, before its serial: letters,
    // digits and '_'. Empty for the name of its kind.
    virtual std::string StatusName() const { return ""; }
};

// The kinds of application there are, for the launcher.
struct AppKind
{
    const char* Name;           // "Terminal": the launcher's label, and what Launch() takes
    const char* Description;
    App* (*Create)(const std::string& argument);
    bool SingleInstance;        // launching it again focuses the one there is
    bool QuickLaunch;           // gets a button on the panel
    // A program with windows of its own, started as a process of its own:
    // its name, looked up in $PATH. Offered only where it is installed.
    // Create is null for these.
    const char* Program = nullptr;
};

const std::vector<AppKind>& AppKinds();

// Open an application's window (or focus it, for single-instance ones), or
// start a program. `argument` is the kind's own business: a path for Files
// and Editor.
void Launch(const std::string& kind, const std::string& argument = "");

// Give the desktop a window it did not launch -- a Wayland client's. It
// opens before the next frame, focused, and the desktop owns `app` from now.
void AddWindow(App* app);

} // namespace desktop
