#include "desktop.h"
#include "app.h"
#include "settings.h"
#include "splash.h"
#include "../apps/apps.h"

#include "imgui.h"
#include "imgui_impl_softrast.h"

#ifdef SIRIUSD_WAYLAND
#include "../wayland/compositor.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>

#include <sys/wait.h>
#include <unistd.h>

namespace desktop {
namespace {

// One open window.
struct Window
{
    std::unique_ptr<App> Application;
    const AppKind* Kind;        // null for a window the desktop did not launch
    int Serial;                 // distinguishes instances of one kind, and keeps the window's ImGui identity stable
    bool Open = true;
    bool FocusRequested = true; // bring to the front at the next frame
    bool Minimized = false;
    ImVec2 Position, Size;      // as drawn last frame, for the status line
    bool Focused = false;

    // "Terminal###Terminal-3": the title may change, the identity after ### may not.
    std::string ImGuiName() const
    {
        return Application->Title() + "###" + (Kind != nullptr ? Kind->Name : "Window") + "-" + std::to_string(Serial);
    }
    // What the status line calls it.
    std::string ShortName() const
    {
        std::string name = Application->StatusName();
        if (name.empty()) name = Kind != nullptr ? Kind->Name : "Window";
        return name + "#" + std::to_string(Serial);
    }
};

std::vector<std::unique_ptr<Window>> g_windows;
std::vector<std::pair<std::string, std::string>> g_launch_queue;   // kind, argument: opened between frames
std::vector<std::unique_ptr<App>> g_added;                         // windows from elsewhere, also opened between frames
std::vector<pid_t> g_to_reap;
int g_next_serial = 1;
bool g_show_frame_times = false;
double g_last_activity = 0.0;   // ImGui time of the last input
std::string g_launchers;        // where the panel's launch buttons are, for the status line

const AppKind* FindKind(const std::string& name)
{
    for (const AppKind& kind : AppKinds())
        if (name == kind.Name) return &kind;
    return nullptr;
}

// Is `program` an executable somewhere in $PATH?
bool Installed(const char* program)
{
    const char* path = std::getenv("PATH");
    std::string directories = path != nullptr && path[0] != '\0' ? path : "/bin:/usr/bin";
    size_t start = 0;
    while (start <= directories.size())
    {
        const size_t end = std::min(directories.find(':', start), directories.size());
        const std::string candidate = directories.substr(start, end - start) + "/" + program;
        if (access(candidate.c_str(), X_OK) == 0)
            return true;
        start = end + 1;
    }
    return false;
}

// Kinds that can be launched here: programs only where they are installed.
bool Available(const AppKind& kind)
{
    return kind.Program == nullptr || Installed(kind.Program);
}

// Start a program with windows of its own. It inherits the desktop's
// environment, WAYLAND_DISPLAY included, and its output goes where the
// desktop's does: the console.
void Start(const AppKind& kind)
{
    const pid_t pid = fork();
    if (pid == 0)
    {
        char* const argv[] = { const_cast<char*>(kind.Program), nullptr };
        execvp(kind.Program, argv);
        std::printf("[siriusd] cannot start %s: %s\n", kind.Program, std::strerror(errno));
        _exit(127);
    }
    if (pid < 0)
    {
        std::printf("[siriusd] cannot start %s: %s\n", kind.Program, std::strerror(errno));
        return;
    }
    std::printf("[siriusd] started %s, pid %d\n", kind.Program, (int)pid);
    ReapLater(pid);
}

// "Wayland Terminal" -> "Wayland_Terminal": what the status line can parse.
std::string Word(const char* text)
{
    std::string word = text;
    for (char& c : word)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
    return word;
}

Window* NewWindow(App* app, const AppKind* kind)
{
    auto window = std::make_unique<Window>();
    window->Application.reset(app);
    window->Kind = kind;
    window->Serial = g_next_serial++;
    g_windows.push_back(std::move(window));
    return g_windows.back().get();
}

void OpenQueued()
{
    for (const auto& [kind_name, argument] : g_launch_queue)
    {
        const AppKind* kind = FindKind(kind_name);
        if (kind == nullptr)
            continue;
        if (kind->SingleInstance)
        {
            bool found = false;
            for (auto& window : g_windows)
                if (window->Kind == kind) { window->FocusRequested = true; window->Minimized = false; found = true; }
            if (found)
                continue;
        }
        if (kind->Program != nullptr)
            Start(*kind);
        else
            NewWindow(kind->Create(argument), kind);
    }
    g_launch_queue.clear();
    for (auto& app : g_added)
        NewWindow(app.release(), nullptr);
    g_added.clear();
}

void DrawPanel(const FrameTimes& times)
{
    // A taller bar than a window's menu bar: it is the one thing always on
    // screen, and its buttons are what the mouse aims for most.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 7.0f));
    if (!ImGui::BeginMainMenuBar())
    {
        ImGui::PopStyleVar();
        return;
    }

    if (ImGui::BeginMenu("Sirius"))
    {
        bool programs = false;
        for (const AppKind& kind : AppKinds())
        {
            if (!Available(kind))
                continue;
            if (kind.Program != nullptr && !programs)
            {
                ImGui::SeparatorText("Wayland");
                programs = true;
            }
            if (ImGui::MenuItem(kind.Name, nullptr, false, true))
                Launch(kind.Name);
        }
        ImGui::Separator();
        ImGui::MenuItem("Show frame times", nullptr, &g_show_frame_times);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    g_launchers.clear();
    for (const AppKind& kind : AppKinds())
    {
        if (!kind.QuickLaunch || !Available(kind))
            continue;
        if (ImGui::MenuItem(kind.Name))
            Launch(kind.Name);
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        char text[96];
        std::snprintf(text, sizeof(text), " %s@%.0f,%.0f", Word(kind.Name).c_str(), (double)(min.x + max.x) * 0.5, (double)(min.y + max.y) * 0.5);
        g_launchers += text;
    }
    ImGui::Separator();

    // The task list: one entry per window. Clicking brings it forward, or
    // puts it away if it already is.
    for (auto& window : g_windows)
    {
        const std::string label = window->Application->Title() + "##task" + std::to_string(window->Serial);
        if (ImGui::MenuItem(label.c_str(), nullptr, window->Focused && !window->Minimized))
        {
            if (window->Focused && !window->Minimized) window->Minimized = true;
            else { window->Minimized = false; window->FocusRequested = true; }
        }
    }

    // The clock, against the right edge.
    const time_t now = std::time(nullptr);
    char clock_text[64];
    std::strftime(clock_text, sizeof(clock_text), GetSettings().ShowSeconds ? "%a %d %b  %H:%M:%S" : "%a %d %b  %H:%M", std::gmtime(&now));
    char right[128];
    if (g_show_frame_times)
        std::snprintf(right, sizeof(right), "%.1f ms (build %.1f, draw %.1f, show %.1f)   %s", (double)(times.Build + times.Raster + times.Present),
                      (double)times.Build, (double)times.Raster, (double)times.Present, clock_text);
    else
        std::snprintf(right, sizeof(right), "%s", clock_text);
    const float width = ImGui::CalcTextSize(right).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - width);
    ImGui::TextUnformatted(right);

    ImGui::EndMainMenuBar();
    ImGui::PopStyleVar();
}

void DrawWindows()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    for (size_t i = 0; i < g_windows.size(); i++)
    {
        Window& window = *g_windows[i];
        window.Application->Update();
        if (!window.Application->WantsToStayOpen()) window.Open = false;
        if (window.Application->TakeMinimizeRequest()) window.Minimized = true;
        if (window.Minimized || !window.Open)
        {
            window.Focused = false;
            continue;
        }

        // New windows cascade down from the top left of the work area.
        const float step = 28.0f * (float)((window.Serial - 1) % 8);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 40.0f + step, viewport->WorkPos.y + 30.0f + step), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(window.Application->DefaultSize(), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(220.0f, 120.0f), viewport->WorkSize);
        if (window.FocusRequested)
        {
            ImGui::SetNextWindowFocus();
            window.FocusRequested = false;
        }

        window.Application->BeforeBegin();
        const std::string name = window.ImGuiName();
        const bool visible = ImGui::Begin(name.c_str(), &window.Open, window.Application->WindowFlags() | ImGuiWindowFlags_NoCollapse);
        window.Application->AfterBegin();
        if (!window.Open && !window.Application->CloseRequested())
            window.Open = true;   // asked to close; it will, in its own time
        if (visible)
            window.Application->Draw();
        window.Position = ImGui::GetWindowPos();
        window.Size = ImGui::GetWindowSize();
        window.Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        ImGui::End();
    }

    // Closed windows go after the frame's last use of them.
    for (size_t i = 0; i < g_windows.size();)
    {
        if (g_windows[i]->Open) i++;
        else g_windows.erase(g_windows.begin() + (long)i);
    }
}

} // namespace

const std::vector<AppKind>& AppKinds()
{
    static const std::vector<AppKind> kinds = {
        { "Terminal", "A command line", CreateTerminal, false, true },
        { "Files", "Browse the filesystem", CreateFiles, false, true },
        { "Monitor", "Processors, memory and processes", CreateMonitor, true, true },
        { "Settings", "Appearance, keyboard and mouse", CreateSettings, true, true },
        { "Editor", "Edit a text file", CreateEditor, false, false },
        { "About", "About this system", CreateAbout, true, false },
#ifdef SIRIUSD_WAYLAND
        // Wayland clients, from the weston port, where it is installed.
        { "Wayland Terminal", "weston-terminal: a terminal emulator", nullptr, false, true, "weston-terminal" },
        { "Wayland Demo", "weston-simple-shm: a client drawing into shared memory", nullptr, false, true, "weston-simple-shm" },
#endif
    };
    return kinds;
}

void Launch(const std::string& kind, const std::string& argument)
{
    // Not now: the caller is in the middle of drawing some other window.
    g_launch_queue.emplace_back(kind, argument);
}

void AddWindow(App* app) { g_added.emplace_back(app); }

void ReapLater(pid_t pid) { g_to_reap.push_back(pid); }

void Init(const std::string& settings_path)
{
    LoadSettings(settings_path);
    ApplySettings();
}

void Shutdown()
{
    g_windows.clear();
    g_added.clear();
}

void BeginFrame()
{
    if (ApplyRequested())
        ApplySettings();
#ifdef SIRIUSD_WAYLAND
    wayland::SetKeyboardLayout(GetSettings().KeyboardLayout);
    wayland::Dispatch();
#endif
    OpenQueued();
    for (size_t i = 0; i < g_to_reap.size();)
    {
        int status;
        if (waitpid(g_to_reap[i], &status, WNOHANG) != 0) g_to_reap.erase(g_to_reap.begin() + (long)i);
        else i++;
    }
}

void Draw(const FrameTimes& times, const ImGui_ImplSoftRast_Stats&)
{
    const ImGuiIO& io = ImGui::GetIO();
    // A focused text field is "active" for as long as it has the focus, so
    // an active item is not activity: that would keep the desktop drawing
    // flat out behind every idle terminal.
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || io.MouseWheel != 0.0f || ImGui::IsAnyMouseDown() ||
        io.InputQueueCharacters.Size > 0)
        g_last_activity = ImGui::GetTime();

    // Under a loading screen that hides everything, nothing else is drawn.
    if (!SplashCovers())
    {
        DrawPanel(times);
        DrawWindows();
    }
#ifdef SIRIUSD_WAYLAND
    wayland::EndFrame();
#endif
    DrawSplash();
}

void CollectFds(std::vector<int>& fds)
{
    for (auto& window : g_windows)
        window->Application->CollectFds(fds);
#ifdef SIRIUSD_WAYLAND
    wayland::CollectFds(fds);
#endif
}

int IdleTimeoutMs()
{
    // Dear ImGui wants a few frames after any input to settle (hover states,
    // popups opening); a blinking text cursor wants a frame now and then;
    // and the clock one a second.
    const double idle = ImGui::GetTime() - g_last_activity;
    if (!g_launch_queue.empty() || !g_added.empty()) return 0;
    if (SplashActive()) return 10;   // animating
    int timeout;
    if (idle < 0.5) timeout = 5;    // interactive: frames as input arrives, up to 200 a second
    else if (ImGui::GetIO().WantTextInput) timeout = 100;
    else timeout = GetSettings().ShowSeconds || !g_windows.empty() ? 250 : 1000;
#ifdef SIRIUSD_WAYLAND
    // A client waiting to see its last frame on screen, or for the next.
    const int deadline = wayland::FrameDeadlineMs();
    if (deadline >= 0 && deadline < timeout) timeout = deadline;
#endif
    return timeout;
}

std::string StatusLine()
{
    std::string line = "launchers" + g_launchers + "; ";
#ifdef SIRIUSD_WAYLAND
    line += std::to_string(wayland::ClientCount()) + " clients; ";
#endif
    line += std::to_string(g_windows.size()) + " windows";
    for (const auto& window : g_windows)
    {
        char text[160];
        if (window->Minimized)
            std::snprintf(text, sizeof(text), "; %s minimized", window->ShortName().c_str());
        else
            std::snprintf(text, sizeof(text), "; %s at %.0f,%.0f %.0fx%.0f%s", window->ShortName().c_str(), (double)window->Position.x,
                          (double)window->Position.y, (double)window->Size.x, (double)window->Size.y, window->Focused ? " focused" : "");
        line += text;
    }
    return line;
}

} // namespace desktop
