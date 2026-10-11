// siriusd: the Sirius desktop. One process that owns the framebuffer and the
// input device, and draws everything with Dear ImGui: the panel, the windows,
// and the applications inside them (shell/, apps/) -- and, as a Wayland
// compositor (wayland/), the windows of programs that draw their own.
//
// Every frame is rasterized in software into a back buffer in ordinary memory
// and then copied to the display: blending reads the pixels underneath, and
// the framebuffer is mapped write-combining, where reading is ruinously slow.
//
// A frame is drawn when there is a reason to: input arrived, a terminal's
// shell said something, a Wayland client drew, or enough time has passed
// that the clock has moved. In between, the process sleeps in poll().
#include "imgui.h"
#include "imgui_impl_sirius.h"
#include "imgui_impl_softrast.h"
#include "shell/desktop.h"
#include "shell/settings.h"
#include "shell/splash.h"
#ifdef SIRIUSD_WAYLAND
#include "wayland/compositor.h"
#endif

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <emmintrin.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <sirius/fb.h>

namespace {

double Seconds()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void Smooth(float& average, float sample) { average = average == 0.0f ? sample : average * 0.9f + sample * 0.1f; }

struct Display
{
    sirius_fb_info info;
    unsigned char* pixels;      // the framebuffer itself: write only
    bool native;                // its pixels are XRGB8888, as the rasterizer draws them
};

bool OpenDisplay(Display& display)
{
    const int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { std::printf("[siriusd] cannot open /dev/fb0: %s\n", std::strerror(errno)); return false; }
    if (ioctl(fd, SIRIUS_FB_GET_INFO, &display.info) != 0)
    {
        std::printf("[siriusd] /dev/fb0 does not describe itself: %s\n", std::strerror(errno));
        return false;
    }
    void* mapping = mmap(nullptr, display.info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) { std::printf("[siriusd] cannot map /dev/fb0: %s\n", std::strerror(errno)); return false; }
    close(fd);   // the mapping stays

    const sirius_fb_info& info = display.info;
    display.pixels = (unsigned char*)mapping;
    display.native = info.bpp == 32 && info.red_shift == 16 && info.green_shift == 8 && info.blue_shift == 0;
    return info.bpp == 32;
}

// Copy the finished frame to the display, a row at a time: the display's
// rows may be longer than the image.
void Present(const Display& display, const uint32_t* frame)
{
    const sirius_fb_info& info = display.info;
    for (uint32_t y = 0; y < info.height; y++)
    {
        const uint32_t* src = frame + (size_t)y * info.width;
        uint32_t* dst = (uint32_t*)(display.pixels + (size_t)y * info.pitch);
        if (display.native)
        {
            // Non-temporal stores: the data is headed for a write-combining
            // buffer, not the cache. The back buffer's rows are 16-byte
            // aligned (see main); the display's rows are assumed to be.
            uint32_t x = 0;
            if (((uintptr_t)dst & 15) == 0)
                for (; x + 4 <= info.width; x += 4)
                    _mm_stream_si128((__m128i*)(dst + x), _mm_load_si128((const __m128i*)(src + x)));
            for (; x < info.width; x++)
                dst[x] = src[x];
        }
        else
        {
            // Some other channel order (BGR panels exist): repack each pixel.
            for (uint32_t x = 0; x < info.width; x++)
                dst[x] = (((src[x] >> 16) & 0xff) << info.red_shift) | (((src[x] >> 8) & 0xff) << info.green_shift) |
                         ((src[x] & 0xff) << info.blue_shift);
        }
    }
    _mm_sfence();
}

} // namespace

int main()
{
    // Writing to a shell that has exited must be an error to handle, not a
    // signal that ends the desktop.
    signal(SIGPIPE, SIG_IGN);

    Display display{};
    if (!OpenDisplay(display))
        return 1;
    const int width = (int)display.info.width, height = (int)display.info.height;
    std::printf("[siriusd] display %dx%d, %u bpp, pitch %u%s\n", width, height, display.info.bpp, display.info.pitch,
                display.native ? "" : " (not XRGB: converting on present)");

    const int input = open("/dev/input", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (input < 0)
        std::printf("[siriusd] no input: cannot open /dev/input: %s\n", std::strerror(errno));

    // Each row starts on a 16-byte boundary when the width is a multiple of
    // four pixels, which every real mode's is.
    const size_t frame_bytes = ((size_t)width * height * 4 + 4095) & ~(size_t)4095;
    auto* frame = (uint32_t*)aligned_alloc(4096, frame_bytes);
    auto* wallpaper = (uint32_t*)aligned_alloc(4096, frame_bytes);
    if (frame == nullptr || wallpaper == nullptr) { std::printf("[siriusd] out of memory for the back buffer\n"); return 1; }
    const ImGui_ImplSoftRast_Target target{ frame, width, height, width * 4 };

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // window positions are not worth a disk write every few seconds
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.Fonts->TexDesiredFormat = ImTextureFormat_Alpha8;
    ImGui_ImplSirius_Init(width, height);
    ImGui_ImplSoftRast_Init();
    desktop::Init("/etc/siriusd.conf");
    // Out of the kernel's black screen: a loading screen while the rest
    // starts, fading into the desktop.
    desktop::StartSplash(1.8);
#ifdef SIRIUSD_WAYLAND
    // Programs the desktop starts find it through the environment.
    if (getenv("PATH") == nullptr)
        setenv("PATH", "/bin:/usr/bin", 1);   // /usr/bin: programs from the ports
    if (wayland::Start(width, height))
        ImGui_ImplSirius_SetEventObserver([](const sirius_input_event* event, void*) { wayland::HandleInputEvent(*event); }, nullptr);
#endif

    desktop::FrameTimes times;
    double last_frame = Seconds(), last_report = last_frame;
    long frames = 0, frames_at_report = 0, events = 0, events_at_report = 0;
    std::vector<int> wake_fds;
    std::vector<pollfd> polled;

    for (;;)
    {
        // Sleep until there is something to draw.
        wake_fds.clear();
        desktop::CollectFds(wake_fds);
        polled.clear();
        if (input >= 0) polled.push_back({ input, POLLIN, 0 });
        for (const int fd : wake_fds) polled.push_back({ fd, POLLIN, 0 });
        poll(polled.data(), polled.size(), desktop::IdleTimeoutMs());

        if (input >= 0)
        {
            const int n = ImGui_ImplSirius_PollDevice(input);
            if (n > 0) events += n;
        }

        desktop::BeginFrame();
        if (desktop::WallpaperChanged())
            desktop::RenderWallpaper(wallpaper, width, height);

        ImGui_ImplSoftRast_NewFrame();
        ImGui_ImplSirius_NewFrame();
        ImGui::NewFrame();
        const double build_start = Seconds();
        desktop::Draw(times, ImGui_ImplSoftRast_GetStats());
        ImGui::Render();
        const double raster_start = Seconds();
        std::memcpy(frame, wallpaper, (size_t)width * height * 4);
        ImGui_ImplSoftRast_RenderDrawData(ImGui::GetDrawData(), target);
        const double present_start = Seconds();
        Present(display, frame);
#ifdef SIRIUSD_WAYLAND
        wayland::FramePresented();
#endif
        const double now = Seconds();

        Smooth(times.Build, (float)((raster_start - build_start) * 1000.0));
        Smooth(times.Raster, (float)((present_start - raster_start) * 1000.0));
        Smooth(times.Present, (float)((now - present_start) * 1000.0));
        Smooth(times.Total, (float)((now - last_frame) * 1000.0));
        last_frame = now;
        frames++;

        // Once a second on the console: for whoever is watching the serial
        // port, and for the tests, which cannot see the screen. "frame" is
        // what one frame costs to produce; the desktop draws far fewer than
        // that allows, because it draws only when something has changed.
        // Not while the loading screen is up: the desktop is not there to
        // be described (or tested) yet, and its frames are not the desktop's.
        if (desktop::SplashActive())
        {
            last_report = now;
            frames_at_report = frames;
            events_at_report = events;
        }
        else if (now - last_report >= 1.0)
        {
            std::printf("[siriusd] %dx%d: frame %.2f ms (build %.2f, raster %.2f, present %.2f), %ld drawn; "
                        "%ld input events, mouse at %.0f,%.0f; %s\n",
                        width, height, times.Build + times.Raster + times.Present, times.Build, times.Raster, times.Present,
                        frames - frames_at_report, events - events_at_report, io.MousePos.x, io.MousePos.y,
                        desktop::StatusLine().c_str());
            std::fflush(stdout);
            last_report = now;
            frames_at_report = frames;
            events_at_report = events;
        }
    }
}
