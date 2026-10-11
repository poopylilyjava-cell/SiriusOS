// siriusd on the build machine. See CMakeLists.txt for what it is for.
//
// Everything Sirius-specific is replaced by the thinnest possible stand-in:
// the framebuffer by a malloc'd buffer (shown in an SDL window, or written
// to a .ppm), and /dev/input by SDL events translated into the same
// sirius_input_event records the kernel produces. The backends and the
// desktop are the real ones.
#include "imgui.h"
#include "imgui_impl_sirius.h"
#include "imgui_impl_softrast.h"
#include "desktop.h"
#include "app.h"
#include "settings.h"
#include "splash.h"
#ifdef SIRIUSD_WAYLAND
#include "../wayland/compositor.h"
#endif

#include <chrono>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <string>
#include <vector>

#ifdef HARNESS_HAVE_SDL
#include <SDL.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

float MillisecondsSince(Clock::time_point start)
{
    return std::chrono::duration<float, std::milli>(Clock::now() - start).count();
}

void Smooth(float& average, float sample) { average = average == 0.0f ? sample : average * 0.9f + sample * 0.1f; }

struct Screen
{
    int width, height;
    std::vector<uint32_t> pixels, wallpaper;
    ImGui_ImplSoftRast_Target target() { return { pixels.data(), width, height, width * 4 }; }
    void resize(int w, int h)
    {
        width = w; height = h;
        pixels.assign((size_t)w * h, 0);
        wallpaper.assign((size_t)w * h, 0);
        desktop::RenderWallpaper(wallpaper.data(), w, h);
    }
};

void Send(uint16_t type, uint16_t code, int32_t value)
{
    const sirius_input_event event{ 0, type, code, value };
    ImGui_ImplSirius_ProcessEvent(&event);
}

// Build and rasterize one frame; returns the time each half took.
void RenderFrame(Screen& screen, desktop::FrameTimes& times)
{
    desktop::BeginFrame();
    if (desktop::WallpaperChanged())
        desktop::RenderWallpaper(screen.wallpaper.data(), screen.width, screen.height);

    ImGui_ImplSoftRast_NewFrame();
    ImGui_ImplSirius_NewFrame();
    ImGui::NewFrame();
    const auto build_start = Clock::now();
    desktop::Draw(times, ImGui_ImplSoftRast_GetStats());
    ImGui::Render();
    Smooth(times.Build, MillisecondsSince(build_start));

    const auto raster_start = Clock::now();
    const ImGui_ImplSoftRast_Target target = screen.target();
    screen.pixels = screen.wallpaper;
    ImGui_ImplSoftRast_RenderDrawData(ImGui::GetDrawData(), target);
    Smooth(times.Raster, MillisecondsSince(raster_start));
}

bool WritePpm(const char* path, const Screen& screen)
{
    FILE* file = std::fopen(path, "wb");
    if (file == nullptr)
        return false;
    std::fprintf(file, "P6\n%d %d\n255\n", screen.width, screen.height);
    for (uint32_t pixel : screen.pixels)
    {
        const unsigned char rgb[3] = { (unsigned char)(pixel >> 16), (unsigned char)(pixel >> 8), (unsigned char)pixel };
        std::fwrite(rgb, 1, 3, file);
    }
    return std::fclose(file) == 0;
}

// What a scripted run does at a given frame (--at FRAME:ACTION).
struct Step
{
    int Frame;
    std::string Action;     // launch:KIND, move:X,Y, click:X,Y, type:TEXT, key:CODE
};

// The key that types `c` on a US keyboard, and whether it needs Shift.
bool KeyFor(char c, uint16_t& code, bool& shift)
{
    static const char* const rows[] = { "1234567890-=", "qwertyuiop[]", "asdfghjkl;'", "zxcvbnm,./" };
    static const char* const shifted[] = { "!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"", "ZXCVBNM<>?" };
    static const uint16_t first[] = { SIRIUS_KEY_1, SIRIUS_KEY_Q, SIRIUS_KEY_A, SIRIUS_KEY_Z };
    shift = false;
    if (c == ' ') { code = SIRIUS_KEY_SPACE; return true; }
    if (c == '\n') { code = SIRIUS_KEY_ENTER; return true; }
    for (int row = 0; row < 4; row++)
        for (int pass = 0; pass < 2; pass++)
            if (const char* hit = std::strchr(pass ? shifted[row] : rows[row], c); hit != nullptr && c != '\0')
            {
                code = (uint16_t)(first[row] + (hit - (pass ? shifted[row] : rows[row])));
                shift = pass == 1;
                return true;
            }
    return false;
}

void Perform(const std::string& action, int width, int height)
{
    const size_t colon = action.find(':');
    const std::string verb = action.substr(0, colon), argument = colon == std::string::npos ? "" : action.substr(colon + 1);
    int x = 0, y = 0;
    if (verb == "launch")
        desktop::Launch(argument);
    else if ((verb == "move" || verb == "click") && std::sscanf(argument.c_str(), "%d,%d", &x, &y) == 2)
    {
        Send(SIRIUS_EV_ABS, SIRIUS_ABS_X, x * SIRIUS_ABS_MAX / (width - 1));
        Send(SIRIUS_EV_ABS, SIRIUS_ABS_Y, y * SIRIUS_ABS_MAX / (height - 1));
        if (verb == "click") { Send(SIRIUS_EV_KEY, SIRIUS_BTN_LEFT, 1); Send(SIRIUS_EV_KEY, SIRIUS_BTN_LEFT, 0); }
    }
    else if (verb == "type")
    {
        for (size_t i = 0; i < argument.size(); i++)
        {
            char c = argument[i];
            if (c == '\\' && i + 1 < argument.size() && argument[i + 1] == 'n') { c = '\n'; i++; }
            uint16_t code;
            bool shift;
            if (!KeyFor(c, code, shift)) continue;
            if (shift) Send(SIRIUS_EV_KEY, SIRIUS_KEY_LEFTSHIFT, 1);
            Send(SIRIUS_EV_KEY, code, 1);
            Send(SIRIUS_EV_KEY, code, 0);
            if (shift) Send(SIRIUS_EV_KEY, SIRIUS_KEY_LEFTSHIFT, 0);
        }
    }
    else if (verb == "key")
    {
        const uint16_t code = (uint16_t)std::atoi(argument.c_str());
        Send(SIRIUS_EV_KEY, code, 1);
        Send(SIRIUS_EV_KEY, code, 0);
    }
    else
        std::fprintf(stderr, "harness: what is '%s'?\n", action.c_str());
}

// No window: draw `frames` frames, poking the UI along the way so that more
// than the first, idle frame is exercised, and report what they cost. With a
// script, do what it says at the frames it says, a frame every `frame_ms`.
int RunHeadless(Screen& screen, int frames, const char* out_path, bool demo, const std::vector<Step>& script, int frame_ms)
{
    desktop::FrameTimes times;
    float worst = 0.0f, sum = 0.0f;
    for (int frame = 0; frame < frames; frame++)
    {
        // The same records a tablet and a keyboard would produce on Sirius.
        if (frame == 2) { Send(SIRIUS_EV_ABS, SIRIUS_ABS_X, SIRIUS_ABS_MAX / 8); Send(SIRIUS_EV_ABS, SIRIUS_ABS_Y, SIRIUS_ABS_MAX / 4); }
        if (frame == 4) Send(SIRIUS_EV_REL, SIRIUS_REL_WHEEL, -2);
        // --demo: one of everything, and a command typed into the terminal.
        if (demo && frame == 1) for (const char* kind : { "Files", "Monitor", "Settings", "Editor", "Terminal" }) desktop::Launch(kind);
        if (demo && frame == 12) for (const uint16_t key : { SIRIUS_KEY_L, SIRIUS_KEY_S, SIRIUS_KEY_SPACE, SIRIUS_KEY_SLASH, SIRIUS_KEY_ENTER })
            { Send(SIRIUS_EV_KEY, key, 1); Send(SIRIUS_EV_KEY, key, 0); }
        if (demo && frame > 12) std::this_thread::sleep_for(std::chrono::milliseconds(20));   // let the shell answer
        for (const Step& step : script)
            if (step.Frame == frame) Perform(step.Action, screen.width, screen.height);
        if (frame_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(frame_ms));

        const auto start = Clock::now();
        RenderFrame(screen, times);
#ifdef SIRIUSD_WAYLAND
        wayland::FramePresented();
#endif
        const float ms = MillisecondsSince(start);
        Smooth(times.Total, ms);
        if (frame > 0) { sum += ms; if (ms > worst) worst = ms; }   // the first frame builds the font atlas
    }

    const ImGui_ImplSoftRast_Stats& stats = ImGui_ImplSoftRast_GetStats();
    std::printf("%dx%d, %d frames: %.2f ms average (%.0f fps), %.2f ms worst; build %.2f ms, raster %.2f ms\n",
                screen.width, screen.height, frames, frames > 1 ? sum / (float)(frames - 1) : 0.0f,
                frames > 1 && sum > 0.0f ? 1000.0f * (float)(frames - 1) / sum : 0.0f, worst, times.Build, times.Raster);
    std::printf("last frame: %d rects, %d triangles; %.2f Mpx filled, %.2f Mpx text, %.2f Mpx shaded\n",
                stats.Rects, stats.Triangles, (double)stats.PixelsFilled / 1e6, (double)stats.PixelsTextured / 1e6,
                (double)stats.PixelsShaded / 1e6);

    if (out_path != nullptr)
    {
        if (!WritePpm(out_path, screen)) { std::fprintf(stderr, "cannot write %s\n", out_path); return 1; }
        std::printf("wrote %s\n", out_path);
    }
    return 0;
}

#ifdef HARNESS_HAVE_SDL

// SDL scancodes are USB HID usages; Sirius key codes are evdev's. The few
// ranges below cover a full keyboard.
int KeyCodeFor(SDL_Scancode scancode)
{
    static const uint16_t letters[26] = {
        SIRIUS_KEY_A, SIRIUS_KEY_B, SIRIUS_KEY_C, SIRIUS_KEY_D, SIRIUS_KEY_E, SIRIUS_KEY_F, SIRIUS_KEY_G, SIRIUS_KEY_H,
        SIRIUS_KEY_I, SIRIUS_KEY_J, SIRIUS_KEY_K, SIRIUS_KEY_L, SIRIUS_KEY_M, SIRIUS_KEY_N, SIRIUS_KEY_O, SIRIUS_KEY_P,
        SIRIUS_KEY_Q, SIRIUS_KEY_R, SIRIUS_KEY_S, SIRIUS_KEY_T, SIRIUS_KEY_U, SIRIUS_KEY_V, SIRIUS_KEY_W, SIRIUS_KEY_X,
        SIRIUS_KEY_Y, SIRIUS_KEY_Z };
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) return letters[scancode - SDL_SCANCODE_A];
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_0) return SIRIUS_KEY_1 + (scancode - SDL_SCANCODE_1);
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F10) return SIRIUS_KEY_F1 + (scancode - SDL_SCANCODE_F1);
    switch (scancode)
    {
    case SDL_SCANCODE_RETURN: return SIRIUS_KEY_ENTER;
    case SDL_SCANCODE_ESCAPE: return SIRIUS_KEY_ESC;
    case SDL_SCANCODE_BACKSPACE: return SIRIUS_KEY_BACKSPACE;
    case SDL_SCANCODE_TAB: return SIRIUS_KEY_TAB;
    case SDL_SCANCODE_SPACE: return SIRIUS_KEY_SPACE;
    case SDL_SCANCODE_MINUS: return SIRIUS_KEY_MINUS;
    case SDL_SCANCODE_EQUALS: return SIRIUS_KEY_EQUAL;
    case SDL_SCANCODE_LEFTBRACKET: return SIRIUS_KEY_LEFTBRACE;
    case SDL_SCANCODE_RIGHTBRACKET: return SIRIUS_KEY_RIGHTBRACE;
    case SDL_SCANCODE_BACKSLASH: return SIRIUS_KEY_BACKSLASH;
    case SDL_SCANCODE_NONUSBACKSLASH: return SIRIUS_KEY_102ND;
    case SDL_SCANCODE_SEMICOLON: return SIRIUS_KEY_SEMICOLON;
    case SDL_SCANCODE_APOSTROPHE: return SIRIUS_KEY_APOSTROPHE;
    case SDL_SCANCODE_GRAVE: return SIRIUS_KEY_GRAVE;
    case SDL_SCANCODE_COMMA: return SIRIUS_KEY_COMMA;
    case SDL_SCANCODE_PERIOD: return SIRIUS_KEY_DOT;
    case SDL_SCANCODE_SLASH: return SIRIUS_KEY_SLASH;
    case SDL_SCANCODE_CAPSLOCK: return SIRIUS_KEY_CAPSLOCK;
    case SDL_SCANCODE_F11: return SIRIUS_KEY_F11;
    case SDL_SCANCODE_F12: return SIRIUS_KEY_F12;
    case SDL_SCANCODE_INSERT: return SIRIUS_KEY_INSERT;
    case SDL_SCANCODE_HOME: return SIRIUS_KEY_HOME;
    case SDL_SCANCODE_PAGEUP: return SIRIUS_KEY_PAGEUP;
    case SDL_SCANCODE_DELETE: return SIRIUS_KEY_DELETE;
    case SDL_SCANCODE_END: return SIRIUS_KEY_END;
    case SDL_SCANCODE_PAGEDOWN: return SIRIUS_KEY_PAGEDOWN;
    case SDL_SCANCODE_RIGHT: return SIRIUS_KEY_RIGHT;
    case SDL_SCANCODE_LEFT: return SIRIUS_KEY_LEFT;
    case SDL_SCANCODE_DOWN: return SIRIUS_KEY_DOWN;
    case SDL_SCANCODE_UP: return SIRIUS_KEY_UP;
    case SDL_SCANCODE_LCTRL: return SIRIUS_KEY_LEFTCTRL;
    case SDL_SCANCODE_LSHIFT: return SIRIUS_KEY_LEFTSHIFT;
    case SDL_SCANCODE_LALT: return SIRIUS_KEY_LEFTALT;
    case SDL_SCANCODE_LGUI: return SIRIUS_KEY_LEFTMETA;
    case SDL_SCANCODE_RCTRL: return SIRIUS_KEY_RIGHTCTRL;
    case SDL_SCANCODE_RSHIFT: return SIRIUS_KEY_RIGHTSHIFT;
    case SDL_SCANCODE_RALT: return SIRIUS_KEY_RIGHTALT;
    case SDL_SCANCODE_RGUI: return SIRIUS_KEY_RIGHTMETA;
    case SDL_SCANCODE_KP_ENTER: return SIRIUS_KEY_KPENTER;
    default: return -1;
    }
}

int RunWindow(Screen& screen)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window* window = SDL_CreateWindow("siriusd (host harness)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          screen.width, screen.height, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, 0) : nullptr;
    if (renderer == nullptr) { std::fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                             screen.width, screen.height);
    // Dear ImGui draws the cursor, as it does on Sirius.
    SDL_ShowCursor(SDL_DISABLE);

    desktop::FrameTimes times;
    for (bool running = true; running; )
    {
        const auto frame_start = Clock::now();
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                {
                    screen.resize(event.window.data1, event.window.data2);
                    SDL_DestroyTexture(texture);
                    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                                screen.width, screen.height);
                    ImGui_ImplSirius_SetDisplaySize(screen.width, screen.height);
                }
                break;
            case SDL_MOUSEMOTION:
                // Absolute, like a tablet: the window does not own the pointer.
                Send(SIRIUS_EV_ABS, SIRIUS_ABS_X, event.motion.x * SIRIUS_ABS_MAX / (screen.width > 1 ? screen.width - 1 : 1));
                Send(SIRIUS_EV_ABS, SIRIUS_ABS_Y, event.motion.y * SIRIUS_ABS_MAX / (screen.height > 1 ? screen.height - 1 : 1));
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            {
                const int down = event.type == SDL_MOUSEBUTTONDOWN;
                if (event.button.button == SDL_BUTTON_LEFT)   Send(SIRIUS_EV_KEY, SIRIUS_BTN_LEFT, down);
                if (event.button.button == SDL_BUTTON_RIGHT)  Send(SIRIUS_EV_KEY, SIRIUS_BTN_RIGHT, down);
                if (event.button.button == SDL_BUTTON_MIDDLE) Send(SIRIUS_EV_KEY, SIRIUS_BTN_MIDDLE, down);
                break;
            }
            case SDL_MOUSEWHEEL:
                Send(SIRIUS_EV_REL, SIRIUS_REL_WHEEL, event.wheel.y);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (const int code = KeyCodeFor(event.key.keysym.scancode); code >= 0)
                    Send(SIRIUS_EV_KEY, (uint16_t)code, event.type == SDL_KEYUP ? 0 : (event.key.repeat ? 2 : 1));
                break;
            default:
                break;
            }
        }

        RenderFrame(screen, times);

        const auto present_start = Clock::now();
        SDL_UpdateTexture(texture, nullptr, screen.pixels.data(), screen.width * 4);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
#ifdef SIRIUSD_WAYLAND
        wayland::FramePresented();
#endif
        Smooth(times.Present, MillisecondsSince(present_start));
        Smooth(times.Total, MillisecondsSince(frame_start));
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

#endif // HARNESS_HAVE_SDL

} // namespace

int main(int argc, char** argv)
{
    Screen screen;
    int width = 1280, height = 720, headless_frames = 0;
    const char* out_path = nullptr;
    bool demo = false;
    std::vector<Step> script;
    int frame_ms = 0;
    bool splash = false;
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--headless") == 0 && i + 1 < argc) headless_frames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--at") == 0 && i + 1 < argc && std::strchr(argv[i + 1], ':') != nullptr)
        {
            const char* text = argv[++i];
            script.push_back({ std::atoi(text), std::strchr(text, ':') + 1 });
            if (frame_ms == 0) frame_ms = 16;   // in real time: programs are being run
        }
        else if (std::strcmp(argv[i], "--frame-ms") == 0 && i + 1 < argc) frame_ms = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--splash") == 0) splash = true;
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (std::strcmp(argv[i], "--demo") == 0) demo = true;
        else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc && std::sscanf(argv[i + 1], "%dx%d", &width, &height) == 2) i++;
        else
        {
            std::fprintf(stderr, "usage: %s [--size WxH] [--headless FRAMES [--demo] [--out frame.ppm]\n"
                                 "          [--at FRAME:launch:KIND|move:X,Y|click:X,Y|type:TEXT|key:CODE ...] [--frame-ms MS] [--splash]]\n", argv[0]);
            return 2;
        }
    }
    if (width < 16 || height < 16 || width > 8192 || height > 8192) { std::fprintf(stderr, "unreasonable size\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);   // a terminal's shell may exit while it is being written to

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    // One byte of coverage per atlas texel is all the rasterizer needs.
    io.Fonts->TexDesiredFormat = ImTextureFormat_Alpha8;
    ImGui_ImplSirius_Init(width, height);
    ImGui_ImplSoftRast_Init();
    // Settings are kept beside wherever the harness is run from, not in /etc.
    desktop::Init("siriusd-host.conf");
    screen.resize(width, height);
#ifdef SIRIUSD_WAYLAND
    // Clients started from the harness's desktop -- or by hand, with
    // WAYLAND_DISPLAY set to what this prints -- open their windows in it.
    if (wayland::Start(width, height))
        ImGui_ImplSirius_SetEventObserver([](const sirius_input_event* event, void*) { wayland::HandleInputEvent(*event); }, nullptr);
#endif

    int status;
    // In a window, as on Sirius; headless runs go straight to the desktop
    // unless asked (--splash, with --frame-ms to give it time to move).
    if (headless_frames <= 0 || splash)
        desktop::StartSplash(1.8);
#ifdef HARNESS_HAVE_SDL
    status = headless_frames > 0 ? RunHeadless(screen, headless_frames, out_path, demo, script, frame_ms) : RunWindow(screen);
#else
    if (headless_frames <= 0) headless_frames = 100;
    status = RunHeadless(screen, headless_frames, out_path, demo, script, frame_ms);
#endif

    desktop::Shutdown();
#ifdef SIRIUSD_WAYLAND
    wayland::Stop();
#endif
    ImGui_ImplSoftRast_Shutdown();
    ImGui_ImplSirius_Shutdown();
    ImGui::DestroyContext();
    return status;
}
