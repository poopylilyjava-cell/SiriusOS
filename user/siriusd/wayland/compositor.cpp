// The compositor's outermost layer: the display, its socket, the output, and
// the rhythm of dispatching requests and answering frame callbacks.
#include "internal.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <sys/stat.h>

namespace wayland {
namespace {

wl_display* g_display = nullptr;
wl_event_loop* g_loop = nullptr;
const char* g_socket = nullptr;
int g_width = 0, g_height = 0;
double g_last_present = 0.0;
double g_last_done = 0.0;       // when frame callbacks were last answered
bool g_frame_requested = false;

// A frame every this many milliseconds while a client is animating.
constexpr int kFrameIntervalMs = 16;

double Seconds()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

uint32_t Milliseconds() { return (uint32_t)(uint64_t)(Seconds() * 1000.0); }

// --- wl_output: one, the whole display ---------------------------------------------

void OutputRelease(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
const struct wl_output_interface kOutputImpl = { OutputRelease };

void BindOutput(wl_client* client, void*, uint32_t version, uint32_t id)
{
    wl_resource* resource = wl_resource_create(client, &wl_output_interface, (int)version, id);
    if (resource == nullptr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &kOutputImpl, nullptr, nullptr);
    // A physical size is a guess: at about 96 dots per inch.
    wl_output_send_geometry(resource, 0, 0, g_width * 254 / 960, g_height * 254 / 960, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "Sirius", "framebuffer", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, g_width, g_height, 60000);
    if (version >= WL_OUTPUT_SCALE_SINCE_VERSION)
        wl_output_send_scale(resource, 1);
    if (version >= WL_OUTPUT_DONE_SINCE_VERSION)
        wl_output_send_done(resource);
}

// libwayland's own logging -- protocol errors, mostly -- on the console.
void LogHandler(const char* format, va_list args)
{
    char line[512];
    std::vsnprintf(line, sizeof(line), format, args);
    std::printf("[siriusd] wayland: %s", line);
    if (line[0] != '\0' && line[std::strlen(line) - 1] != '\n') std::printf("\n");
}

} // namespace

wl_display* Display() { return g_display; }

void RequestFrame() { g_frame_requested = true; }

void Log(const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::printf("[siriusd] wayland: %s\n", line);
    std::fflush(stdout);
}

bool Start(int display_width, int display_height)
{
    g_width = display_width;
    g_height = display_height;

    // The socket goes in the runtime directory, which must be on a
    // filesystem that can hold one -- on Sirius, the tmpfs at /tmp.
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || runtime[0] == '\0')
    {
        if (mkdir("/tmp/runtime", 0700) != 0 && errno != EEXIST)
        {
            Log("cannot make /tmp/runtime: %s", std::strerror(errno));
            return false;
        }
        setenv("XDG_RUNTIME_DIR", "/tmp/runtime", 1);
    }

    wl_log_set_handler_server(LogHandler);
    g_display = wl_display_create();
    if (g_display == nullptr) { Log("cannot create a display"); return false; }
    // wl_shm comes with libwayland: ARGB8888 and XRGB8888, the two every
    // client can draw.
    if (wl_display_init_shm(g_display) != 0 ||
        wl_global_create(g_display, &wl_output_interface, 3, nullptr, BindOutput) == nullptr ||
        !CreateCompositorGlobals(g_display) || !CreateXdgGlobals(g_display) ||
        !CreateSeatGlobals(g_display) || !CreateDataDeviceGlobals(g_display))
    {
        Log("cannot create the globals");
        Stop();
        return false;
    }

    g_socket = wl_display_add_socket_auto(g_display);
    if (g_socket == nullptr)
    {
        Log("cannot open a socket in %s: %s", std::getenv("XDG_RUNTIME_DIR"), std::strerror(errno));
        Stop();
        return false;
    }
    setenv("WAYLAND_DISPLAY", g_socket, 1);
    g_loop = wl_display_get_event_loop(g_display);
    Log("listening on %s/%s", std::getenv("XDG_RUNTIME_DIR"), g_socket);
    return true;
}

void Stop()
{
    if (g_display == nullptr)
        return;
    wl_display_destroy_clients(g_display);
    wl_display_destroy(g_display);
    g_display = nullptr;
    g_loop = nullptr;
    g_socket = nullptr;
}

bool Running() { return g_display != nullptr; }

const char* SocketName() { return g_socket != nullptr ? g_socket : ""; }

void CollectFds(std::vector<int>& fds)
{
    if (g_loop != nullptr)
        fds.push_back(wl_event_loop_get_fd(g_loop));
}

void Dispatch()
{
    if (g_display == nullptr)
        return;
    wl_event_loop_dispatch(g_loop, 0);
    wl_display_flush_clients(g_display);
}

void EndFrame()
{
    if (g_display == nullptr)
        return;
    UpdateInput();
    DrawCursor();
}

void FramePresented()
{
    if (g_display == nullptr)
        return;
    g_last_present = Seconds();
    g_frame_requested = false;
    // Every surface that was in this frame has been seen: tell its client
    // it may draw the next one -- but not more often than a display would
    // refresh. A client that draws as soon as it is told (simple-shm does)
    // would otherwise have the desktop drawing a thousand frames a second.
    // Callbacks held back are answered by the frame FrameDeadlineMs() asks
    // for once the interval has passed.
    const bool answer = (g_last_present - g_last_done) * 1000.0 >= kFrameIntervalMs - 1;
    if (answer)
        g_last_done = g_last_present;
    const uint32_t now = Milliseconds();
    for (Surface* surface : Surfaces())
    {
        if (surface->Shown && answer)
        {
            wl_resource* callback;
            wl_resource* next;
            wl_resource_for_each_safe(callback, next, &surface->Frames)
            {
                wl_callback_send_done(callback, now);
                wl_resource_destroy(callback);
            }
        }
        if (surface->Shown)
            surface->NewContent = false;
        surface->ShownLastFrame = surface->Shown;
        surface->Shown = false;
        if (surface->Xdg != nullptr)
            surface->Xdg->OnScreen = false;
    }
    wl_display_flush_clients(g_display);
}

int FrameDeadlineMs()
{
    if (g_display == nullptr)
        return -1;
    bool wanted = g_frame_requested;
    for (Surface* surface : Surfaces())
        if (surface->ShownLastFrame && (surface->NewContent || !wl_list_empty(&surface->Frames)))
            wanted = true;
    if (!wanted)
        return -1;
    const int since = (int)((Seconds() - g_last_done) * 1000.0);
    return since >= kFrameIntervalMs ? 0 : kFrameIntervalMs - since;
}

int ClientCount()
{
    if (g_display == nullptr)
        return 0;
    int count = 0;
    wl_client* client;
    wl_client_for_each(client, wl_display_get_client_list(g_display))
        count++;
    return count;
}

} // namespace wayland
