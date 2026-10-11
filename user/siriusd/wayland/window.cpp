// A Wayland toplevel as a window of the desktop: Dear ImGui draws the title
// bar, the border and the close button, and the client's pixels fill the
// rest, one texel per pixel.
//
// The size goes both ways. The client decides it by what it commits, and
// the window follows; the user drags the window's edge, and the client is
// asked (configure) to draw at the new size -- which it may decline, and
// then the window goes back to what the client drew.
#include "internal.h"
#include "../shell/app.h"

#include "imgui_internal.h"     // a window's title bar height, as Begin() laid it out

#include <cfloat>
#include <cmath>

namespace wayland {

class ToplevelWindow final : public desktop::App
{
public:
    explicit ToplevelWindow(Toplevel* toplevel) : m_toplevel(toplevel) { Remember(); }

    ~ToplevelWindow() override
    {
        if (m_toplevel != nullptr)
            m_toplevel->Window = nullptr;
    }

    // The toplevel is going: the window closes at the next frame.
    void Detach()
    {
        Remember();
        m_toplevel = nullptr;
    }

    std::string Title() const override { return m_title; }

    std::string StatusName() const override
    {
        // "simple-shm" -> "simple_shm": something the status line can parse.
        std::string name = m_title;
        for (char& c : name)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
        return name.empty() ? "Wayland" : name;
    }

    bool WantsToStayOpen() const override { return m_toplevel != nullptr && m_toplevel->Mapped; }

    ImVec2 DefaultSize() const override { return WithDecorations(ContentSize()); }

    ImGuiWindowFlags WindowFlags() const override
    {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;
        if (Fullscreen()) flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        if (m_toplevel != nullptr && m_toplevel->Maximized) flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        return flags;
    }

    bool CloseRequested() override
    {
        // Ask; the client closes its window, or does not.
        if (m_toplevel != nullptr)
            xdg_toplevel_send_close(m_toplevel->Resource);
        return false;
    }

    bool TakeMinimizeRequest() override
    {
        if (m_toplevel == nullptr || !m_toplevel->MinimizeRequested)
            return false;
        m_toplevel->MinimizeRequested = false;
        return true;
    }

    void BeforeBegin() override
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, Fullscreen() ? 0.0f : ImGui::GetStyle().WindowBorderSize);
        if (m_toplevel == nullptr)
            return;
        Remember();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();

        // The client's size, whenever it changes. (Not every frame: while
        // the user drags an edge, the window is bigger than what the client
        // has drawn so far, and must stay so.)
        // A client that has not followed a resize within half a second of the
        // user letting go is not going to: the window goes back to its size.
        const ImVec2 content = ContentSize();
        const double now = ImGui::GetTime();
        if ((m_drawn.x != content.x || m_drawn.y != content.y) && !PointerButtonDown() && now - m_configured_at > 0.5)
            m_applied = ImVec2(-1.0f, -1.0f);
        if (content.x != m_applied.x || content.y != m_applied.y)
        {
            ImGui::SetNextWindowSize(WithDecorations(content));
            m_applied = content;
        }
        if (Fullscreen())
            ImGui::SetNextWindowPos(viewport->Pos);
        else if (m_toplevel->Maximized)
            ImGui::SetNextWindowPos(viewport->WorkPos);

        // xdg_toplevel.move: the window follows the pointer while the button
        // that asked for it is down.
        if (m_toplevel->MoveRequested)
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            if (!m_moving)
            {
                m_moving = true;
                m_move_offset = ImVec2(mouse.x - m_position.x, mouse.y - m_position.y);
            }
            if (PointerButtonDown())
                ImGui::SetNextWindowPos(ImVec2(mouse.x - m_move_offset.x, mouse.y - m_move_offset.y));
            else
                m_toplevel->MoveRequested = m_moving = false;
        }

        // The client's limits, where it has any; otherwise anything that
        // fits on the screen.
        const ImVec2 min(m_toplevel->MinW > 0 ? (float)m_toplevel->MinW : 1.0f, m_toplevel->MinH > 0 ? (float)m_toplevel->MinH : 1.0f);
        const ImVec2 max(m_toplevel->MaxW > 0 ? (float)m_toplevel->MaxW : FLT_MAX, m_toplevel->MaxH > 0 ? (float)m_toplevel->MaxH : FLT_MAX);
        const ImVec2 limit = viewport->Size;
        ImGui::SetNextWindowSizeConstraints(WithDecorations(min),
                                            ImVec2(std::fmin(WithDecorations(max).x, limit.x), std::fmin(WithDecorations(max).y, limit.y)));
    }

    void AfterBegin() override { ImGui::PopStyleVar(2); }

    void Draw() override
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        m_position = window->Pos;
        if (m_toplevel == nullptr || m_toplevel->Xdg == nullptr || m_toplevel->Xdg->Base == nullptr)
            return;
        XdgSurface* xdg = m_toplevel->Xdg;
        Surface* surface = xdg->Base;

        const float border = window->WindowBorderSize;
        const ImVec2 content_min(window->Pos.x + border, window->Pos.y + window->TitleBarHeight);
        const ImVec2 content_max(window->Pos.x + window->Size.x - border, window->Pos.y + window->Size.y - border);
        const Rect geometry = xdg->WindowGeometry();
        const ImVec2 origin(content_min.x - (float)geometry.X, content_min.y - (float)geometry.Y);

        // The user has made the window a different size from what the
        // client drew: ask it to draw at the new one. Once per size.
        const int width = (int)(content_max.x - content_min.x), height = (int)(content_max.y - content_min.y);
        if (!m_toplevel->Maximized && !Fullscreen() && (width != geometry.W || height != geometry.H) &&
            (width != m_toplevel->ConfiguredW || height != m_toplevel->ConfiguredH) && width > 0 && height > 0)
        {
            ConfigureToplevel(m_toplevel, width, height);
            m_configured_at = ImGui::GetTime();
        }
        m_drawn = ImVec2((float)width, (float)height);

        if (const ImGui_ImplSoftRast_Texture* texture = TextureOf(surface))
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(content_min, content_max, true);
            draw->AddImage((ImTextureID)(intptr_t)texture, origin,
                           ImVec2(origin.x + (float)texture->Width, origin.y + (float)texture->Height));
            draw->PopClipRect();
        }
        surface->ScreenPos = origin;
        surface->Shown = true;
        xdg->ScreenOrigin = content_min;
        xdg->OnScreen = true;

        // The pointer is the client's if it is over the client's part of
        // this window, and not over one of Dear ImGui's own handles -- the
        // resize grip in the corner, the edges.
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && !ImGui::IsAnyItemHovered() &&
            mouse.x >= content_min.x && mouse.y >= content_min.y && mouse.x < content_max.x && mouse.y < content_max.y)
            OfferPointer(surface);
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            OfferKeyboard(surface);
    }

private:
    bool Fullscreen() const { return m_toplevel != nullptr && m_toplevel->Fullscreen; }

    // The size of the client's window, as it last drew it.
    ImVec2 ContentSize() const
    {
        if (m_toplevel == nullptr || m_toplevel->Xdg == nullptr || m_toplevel->Xdg->Base == nullptr)
            return m_applied;
        const Rect geometry = m_toplevel->Xdg->WindowGeometry();
        return ImVec2((float)geometry.W, (float)geometry.H);
    }

    // What Dear ImGui adds around the contents: a title bar, and borders.
    ImVec2 WithDecorations(ImVec2 content) const
    {
        if (Fullscreen())
            return content;
        const float border = ImGui::GetStyle().WindowBorderSize;
        return ImVec2(content.x + 2.0f * border, content.y + ImGui::GetFrameHeight() + border);
    }

    // The title outlives the toplevel, for the frame in which it closes.
    void Remember()
    {
        if (m_toplevel == nullptr)
            return;
        m_title = !m_toplevel->Title.empty() ? m_toplevel->Title : !m_toplevel->AppId.empty() ? m_toplevel->AppId : "Wayland client";
    }

    Toplevel* m_toplevel;
    std::string m_title;
    ImVec2 m_applied;           // the content size last given to the window
    ImVec2 m_drawn;             // the content size the window had, last frame
    double m_configured_at = 0.0;   // when the client was last asked for a new size
    ImVec2 m_position;          // where the window was, last frame
    bool   m_moving = false;
    ImVec2 m_move_offset;
};

void OpenWindow(Toplevel* toplevel)
{
    auto* window = new ToplevelWindow(toplevel);
    toplevel->Window = window;
    desktop::AddWindow(window);
}

void DetachWindow(Toplevel* toplevel)
{
    if (toplevel->Window != nullptr)
    {
        toplevel->Window->Detach();
        toplevel->Window = nullptr;
    }
}

} // namespace wayland
