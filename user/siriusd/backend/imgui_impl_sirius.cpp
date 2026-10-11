// dear imgui: Platform Backend for Sirius OS. See imgui_impl_sirius.h.

#include "imgui.h"
#ifndef IMGUI_DISABLE
#include "imgui_impl_sirius.h"

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace {

struct ImGui_ImplSirius_Data
{
    int         DisplayWidth, DisplayHeight;
    double      Time;                   // of the previous NewFrame, in seconds
    float       MouseX, MouseY;         // a relative device only reports motion: the position is kept here
    bool        Shift, CapsLock, AltGr; // for turning key codes into characters
    bool        Ctrl, Alt, Super;
    const struct Layout* Keymap;
    float       MouseSpeed;
    ImGuiTextBuffer Clipboard;
    ImGui_ImplSirius_EventObserver Observer;
    void*       ObserverData;
};

ImGui_ImplSirius_Data* GetBackendData()
{
    return ImGui::GetCurrentContext() ? (ImGui_ImplSirius_Data*)ImGui::GetIO().BackendPlatformUserData : nullptr;
}

double Now()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// What Dear ImGui calls each physical key. This does not depend on the
// layout: ImGuiKey_A is the key in the A position of a US keyboard, whatever
// it types.
ImGuiKey KeyFor(unsigned code)
{
    static ImGuiKey table[SIRIUS_KEY_MAX + 1];
    static bool built = false;
    if (!built)
    {
        built = true;
        auto set = [](unsigned key_code, ImGuiKey key) { table[key_code] = key; };

        const char* const letters = "qwertyuiop" "asdfghjkl" "zxcvbnm";
        const unsigned rows[3][2] = { { SIRIUS_KEY_Q, 10 }, { SIRIUS_KEY_A, 9 }, { SIRIUS_KEY_Z, 7 } };
        for (int row = 0, n = 0; row < 3; row++)
            for (unsigned i = 0; i < rows[row][1]; i++, n++)
                set(rows[row][0] + i, (ImGuiKey)(ImGuiKey_A + (letters[n] - 'a')));
        for (unsigned i = 0; i < 10; i++)   // KEY_1 .. KEY_9, then KEY_0
            set(SIRIUS_KEY_1 + i, (ImGuiKey)(i == 9 ? ImGuiKey_0 : ImGuiKey_1 + (int)i));

        set(SIRIUS_KEY_MINUS, ImGuiKey_Minus);
        set(SIRIUS_KEY_EQUAL, ImGuiKey_Equal);
        set(SIRIUS_KEY_LEFTBRACE, ImGuiKey_LeftBracket);
        set(SIRIUS_KEY_RIGHTBRACE, ImGuiKey_RightBracket);
        set(SIRIUS_KEY_SEMICOLON, ImGuiKey_Semicolon);
        set(SIRIUS_KEY_APOSTROPHE, ImGuiKey_Apostrophe);
        set(SIRIUS_KEY_GRAVE, ImGuiKey_GraveAccent);
        set(SIRIUS_KEY_BACKSLASH, ImGuiKey_Backslash);
        set(SIRIUS_KEY_102ND, ImGuiKey_Oem102);
        set(SIRIUS_KEY_COMMA, ImGuiKey_Comma);
        set(SIRIUS_KEY_DOT, ImGuiKey_Period);
        set(SIRIUS_KEY_SLASH, ImGuiKey_Slash);
        set(SIRIUS_KEY_SPACE, ImGuiKey_Space);

        set(SIRIUS_KEY_ESC, ImGuiKey_Escape);
        set(SIRIUS_KEY_BACKSPACE, ImGuiKey_Backspace);
        set(SIRIUS_KEY_TAB, ImGuiKey_Tab);
        set(SIRIUS_KEY_ENTER, ImGuiKey_Enter);
        set(SIRIUS_KEY_LEFTCTRL, ImGuiKey_LeftCtrl);
        set(SIRIUS_KEY_RIGHTCTRL, ImGuiKey_RightCtrl);
        set(SIRIUS_KEY_LEFTSHIFT, ImGuiKey_LeftShift);
        set(SIRIUS_KEY_RIGHTSHIFT, ImGuiKey_RightShift);
        set(SIRIUS_KEY_LEFTALT, ImGuiKey_LeftAlt);
        set(SIRIUS_KEY_RIGHTALT, ImGuiKey_RightAlt);
        set(SIRIUS_KEY_LEFTMETA, ImGuiKey_LeftSuper);
        set(SIRIUS_KEY_RIGHTMETA, ImGuiKey_RightSuper);
        set(SIRIUS_KEY_COMPOSE, ImGuiKey_Menu);
        set(SIRIUS_KEY_CAPSLOCK, ImGuiKey_CapsLock);
        set(SIRIUS_KEY_NUMLOCK, ImGuiKey_NumLock);
        set(SIRIUS_KEY_SCROLLLOCK, ImGuiKey_ScrollLock);
        set(SIRIUS_KEY_SYSRQ, ImGuiKey_PrintScreen);
        set(SIRIUS_KEY_PAUSE, ImGuiKey_Pause);
        for (unsigned i = 0; i < 10; i++)
            set(SIRIUS_KEY_F1 + i, (ImGuiKey)(ImGuiKey_F1 + (int)i));
        set(SIRIUS_KEY_F11, ImGuiKey_F11);
        set(SIRIUS_KEY_F12, ImGuiKey_F12);

        set(SIRIUS_KEY_HOME, ImGuiKey_Home);
        set(SIRIUS_KEY_END, ImGuiKey_End);
        set(SIRIUS_KEY_PAGEUP, ImGuiKey_PageUp);
        set(SIRIUS_KEY_PAGEDOWN, ImGuiKey_PageDown);
        set(SIRIUS_KEY_INSERT, ImGuiKey_Insert);
        set(SIRIUS_KEY_DELETE, ImGuiKey_Delete);
        set(SIRIUS_KEY_UP, ImGuiKey_UpArrow);
        set(SIRIUS_KEY_DOWN, ImGuiKey_DownArrow);
        set(SIRIUS_KEY_LEFT, ImGuiKey_LeftArrow);
        set(SIRIUS_KEY_RIGHT, ImGuiKey_RightArrow);

        const unsigned keypad[10] = { SIRIUS_KEY_KP0, SIRIUS_KEY_KP1, SIRIUS_KEY_KP2, SIRIUS_KEY_KP3, SIRIUS_KEY_KP4,
                                      SIRIUS_KEY_KP5, SIRIUS_KEY_KP6, SIRIUS_KEY_KP7, SIRIUS_KEY_KP8, SIRIUS_KEY_KP9 };
        for (int i = 0; i < 10; i++)
            set(keypad[i], (ImGuiKey)(ImGuiKey_Keypad0 + i));
        set(SIRIUS_KEY_KPDOT, ImGuiKey_KeypadDecimal);
        set(SIRIUS_KEY_KPSLASH, ImGuiKey_KeypadDivide);
        set(SIRIUS_KEY_KPASTERISK, ImGuiKey_KeypadMultiply);
        set(SIRIUS_KEY_KPMINUS, ImGuiKey_KeypadSubtract);
        set(SIRIUS_KEY_KPPLUS, ImGuiKey_KeypadAdd);
        set(SIRIUS_KEY_KPENTER, ImGuiKey_KeypadEnter);
    }
    return code <= SIRIUS_KEY_MAX ? table[code] : ImGuiKey_None;
}

// What a key types: plain, with Shift, and with AltGr (the right Alt key, on
// layouts that have one). Unicode code points; 0 for nothing.
struct KeyChars { ImWchar Plain, Shifted, AltGr; };

struct Layout
{
    KeyChars Keys[SIRIUS_KEY_MAX + 1];
    bool     HasAltGr;
};

const Layout* BuildLayout(ImGui_ImplSirius_Layout which)
{
    static Layout layouts[ImGui_ImplSirius_Layout_COUNT];
    static bool built[ImGui_ImplSirius_Layout_COUNT];
    Layout& layout = layouts[which];
    if (built[which])
        return &layout;
    built[which] = true;

    auto set = [&](unsigned code, ImWchar plain, ImWchar shifted, ImWchar altgr = 0) { layout.Keys[code] = { plain, shifted, altgr }; };
    // A row of keys from one string each for the plain and the shifted level
    // (Latin-1 bytes, so one char is one key).
    auto row = [&](unsigned first, const char* plain, const char* shifted) {
        for (unsigned i = 0; plain[i]; i++)
            set(first + i, (ImWchar)(unsigned char)plain[i], (ImWchar)(unsigned char)shifted[i]);
    };

    // Common to both: the keypad types its digits whatever Num Lock says --
    // there is no second set of arrow keys to miss, and no LED for the state.
    const unsigned keypad[10] = { SIRIUS_KEY_KP0, SIRIUS_KEY_KP1, SIRIUS_KEY_KP2, SIRIUS_KEY_KP3, SIRIUS_KEY_KP4,
                                  SIRIUS_KEY_KP5, SIRIUS_KEY_KP6, SIRIUS_KEY_KP7, SIRIUS_KEY_KP8, SIRIUS_KEY_KP9 };
    for (int i = 0; i < 10; i++)
        set(keypad[i], (ImWchar)('0' + i), (ImWchar)('0' + i));
    set(SIRIUS_KEY_KPSLASH, '/', '/');
    set(SIRIUS_KEY_KPASTERISK, '*', '*');
    set(SIRIUS_KEY_KPMINUS, '-', '-');
    set(SIRIUS_KEY_KPPLUS, '+', '+');
    set(SIRIUS_KEY_SPACE, ' ', ' ');

    if (which == ImGui_ImplSirius_Layout_DE)
    {
        layout.HasAltGr = true;
        row(SIRIUS_KEY_1, "1234567890", "!\"\xa7$%&/()=");           // \xa7 is the section sign
        row(SIRIUS_KEY_Q, "qwertzuiop\xfc+", "QWERTZUIOP\xdc*");     // u-umlaut
        row(SIRIUS_KEY_A, "asdfghjkl\xf6\xe4", "ASDFGHJKL\xd6\xc4"); // o- and a-umlaut
        row(SIRIUS_KEY_Z, "yxcvbnm,.-", "YXCVBNM;:_");
        set(SIRIUS_KEY_MINUS, 0xdf, '?', '\\');                       // sharp s
        set(SIRIUS_KEY_EQUAL, 0xb4, '`');                               // the accents, typed as characters
        set(SIRIUS_KEY_GRAVE, '^', 0xb0);                               // circumflex, degree
        set(SIRIUS_KEY_BACKSLASH, '#', '\'');
        set(SIRIUS_KEY_102ND, '<', '>', '|');
        set(SIRIUS_KEY_KPDOT, ',', ',');
        // The third level.
        layout.Keys[SIRIUS_KEY_Q].AltGr = '@';
        layout.Keys[SIRIUS_KEY_E].AltGr = 0x20ac;                       // euro
        layout.Keys[SIRIUS_KEY_M].AltGr = 0xb5;                         // micro
        layout.Keys[SIRIUS_KEY_2].AltGr = 0xb2;
        layout.Keys[SIRIUS_KEY_3].AltGr = 0xb3;
        layout.Keys[SIRIUS_KEY_7].AltGr = '{';
        layout.Keys[SIRIUS_KEY_8].AltGr = '[';
        layout.Keys[SIRIUS_KEY_9].AltGr = ']';
        layout.Keys[SIRIUS_KEY_0].AltGr = '}';
        layout.Keys[SIRIUS_KEY_RIGHTBRACE].AltGr = '~';
    }
    else
    {
        row(SIRIUS_KEY_1, "1234567890-=", "!@#$%^&*()_+");
        row(SIRIUS_KEY_Q, "qwertyuiop[]", "QWERTYUIOP{}");
        row(SIRIUS_KEY_A, "asdfghjkl;'`", "ASDFGHJKL:\"~");
        row(SIRIUS_KEY_Z, "zxcvbnm,./", "ZXCVBNM<>?");
        set(SIRIUS_KEY_BACKSLASH, '\\', '|');
        set(SIRIUS_KEY_102ND, '\\', '|');
        set(SIRIUS_KEY_KPDOT, '.', '.');
    }
    return &layout;
}

// Is this a letter, as far as Caps Lock is concerned? (Latin-1.)
bool IsLetter(ImWchar c)
{
    return (c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7);
}

void ProcessKey(ImGui_ImplSirius_Data* bd, ImGuiIO& io, const sirius_input_event& event)
{
    const bool down = event.value != 0;

    if (event.code >= SIRIUS_BTN_LEFT && event.code <= SIRIUS_BTN_MIDDLE)
    {
        // evdev orders the buttons left, right, middle -- and so does Dear ImGui.
        if (event.value != 2)
            io.AddMouseButtonEvent(event.code - SIRIUS_BTN_LEFT, down);
        return;
    }

    const ImGuiKey key = KeyFor(event.code);
    if (key == ImGuiKey_None)
        return;
    // With an AltGr layout the right Alt key selects the third level, and is
    // not the Alt of shortcuts.
    const bool altgr_key = key == ImGuiKey_RightAlt && bd->Keymap->HasAltGr;

    switch (key)
    {
    case ImGuiKey_LeftShift: case ImGuiKey_RightShift:  bd->Shift = down; io.AddKeyEvent(ImGuiMod_Shift, down); break;
    case ImGuiKey_LeftCtrl:  case ImGuiKey_RightCtrl:   bd->Ctrl = down;  io.AddKeyEvent(ImGuiMod_Ctrl, down); break;
    case ImGuiKey_LeftAlt:                              bd->Alt = down;   io.AddKeyEvent(ImGuiMod_Alt, down); break;
    case ImGuiKey_RightAlt:
        if (altgr_key) bd->AltGr = down;
        else { bd->Alt = down; io.AddKeyEvent(ImGuiMod_Alt, down); }
        break;
    case ImGuiKey_LeftSuper: case ImGuiKey_RightSuper:  bd->Super = down; io.AddKeyEvent(ImGuiMod_Super, down); break;
    case ImGuiKey_CapsLock:                             if (event.value == 1) bd->CapsLock = !bd->CapsLock; break;
    default: break;
    }

    // An auto-repeat is not a new press -- Dear ImGui repeats held keys by
    // its own clock -- but it does type the character again.
    if (event.value != 2)
        io.AddKeyEvent(key, down);

    if (down && !bd->Ctrl && !bd->Alt && !bd->Super)
    {
        const KeyChars& chars = bd->Keymap->Keys[event.code];
        ImWchar c;
        if (bd->AltGr)
            c = chars.AltGr;
        else
            c = (IsLetter(chars.Plain) ? (bd->Shift != bd->CapsLock) : bd->Shift) ? chars.Shifted : chars.Plain;
        if (c != 0)
            io.AddInputCharacter((unsigned int)c);
    }
}

void MoveMouse(ImGui_ImplSirius_Data* bd, ImGuiIO& io, float x, float y)
{
    const float max_x = (float)(bd->DisplayWidth - 1), max_y = (float)(bd->DisplayHeight - 1);
    bd->MouseX = x < 0.0f ? 0.0f : (x > max_x ? max_x : x);
    bd->MouseY = y < 0.0f ? 0.0f : (y > max_y ? max_y : y);
    io.AddMousePosEvent(bd->MouseX, bd->MouseY);
}

const char* GetClipboardText(ImGuiContext*)
{
    ImGui_ImplSirius_Data* bd = GetBackendData();
    return bd->Clipboard.c_str();
}

void SetClipboardText(ImGuiContext*, const char* text)
{
    ImGui_ImplSirius_Data* bd = GetBackendData();
    bd->Clipboard.clear();
    bd->Clipboard.append(text);
}

} // namespace

void ImGui_ImplSirius_ProcessEvent(const sirius_input_event* event)
{
    ImGui_ImplSirius_Data* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplSirius_Init()?");
    ImGuiIO& io = ImGui::GetIO();
    if (bd->Observer != nullptr)
        bd->Observer(event, bd->ObserverData);

    switch (event->type)
    {
    case SIRIUS_EV_KEY:
        ProcessKey(bd, io, *event);
        break;
    case SIRIUS_EV_REL:
        if (event->code == SIRIUS_REL_X)          MoveMouse(bd, io, bd->MouseX + (float)event->value * bd->MouseSpeed, bd->MouseY);
        else if (event->code == SIRIUS_REL_Y)     MoveMouse(bd, io, bd->MouseX, bd->MouseY + (float)event->value * bd->MouseSpeed);
        else if (event->code == SIRIUS_REL_WHEEL) io.AddMouseWheelEvent(0.0f, (float)event->value);
        break;
    case SIRIUS_EV_ABS:
        // Divide first: the fraction is exact at both ends of the range.
        if (event->code == SIRIUS_ABS_X)
            MoveMouse(bd, io, (float)event->value / (float)SIRIUS_ABS_MAX * (float)(bd->DisplayWidth - 1), bd->MouseY);
        else if (event->code == SIRIUS_ABS_Y)
            MoveMouse(bd, io, bd->MouseX, (float)event->value / (float)SIRIUS_ABS_MAX * (float)(bd->DisplayHeight - 1));
        break;
    default:
        break;   // SIRIUS_EV_SYN: Dear ImGui orders and coalesces events itself
    }
}

int ImGui_ImplSirius_PollDevice(int fd)
{
    int count = 0;
    for (;;)
    {
        sirius_input_event events[32];
        const ssize_t n = read(fd, events, sizeof(events));
        if (n < 0)
            return errno == EAGAIN || errno == EINTR ? count : -1;
        const int got = (int)((size_t)n / sizeof(events[0]));
        for (int i = 0; i < got; i++)
            ImGui_ImplSirius_ProcessEvent(&events[i]);
        count += got;
        if ((size_t)n < sizeof(events))
            return count;
    }
}

void ImGui_ImplSirius_SetEventObserver(ImGui_ImplSirius_EventObserver observer, void* user_data)
{
    ImGui_ImplSirius_Data* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplSirius_Init()?");
    bd->Observer = observer;
    bd->ObserverData = user_data;
}

void ImGui_ImplSirius_SetKeyboardLayout(ImGui_ImplSirius_Layout layout)
{
    ImGui_ImplSirius_Data* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplSirius_Init()?");
    if (layout >= 0 && layout < ImGui_ImplSirius_Layout_COUNT)
        bd->Keymap = BuildLayout(layout);
}

void ImGui_ImplSirius_SetMouseSpeed(float pixels_per_unit)
{
    ImGui_ImplSirius_Data* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplSirius_Init()?");
    bd->MouseSpeed = pixels_per_unit > 0.0f ? pixels_per_unit : 1.0f;
}

void ImGui_ImplSirius_SetDisplaySize(int width, int height)
{
    ImGui_ImplSirius_Data* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplSirius_Init()?");
    bd->DisplayWidth = width;
    bd->DisplayHeight = height;
}

bool ImGui_ImplSirius_Init(int display_width, int display_height)
{
    ImGuiIO& io = ImGui::GetIO();
    IMGUI_CHECKVERSION();
    IM_ASSERT(io.BackendPlatformUserData == nullptr && "Already initialized a platform backend!");

    ImGui_ImplSirius_Data* bd = IM_NEW(ImGui_ImplSirius_Data)();
    bd->DisplayWidth = display_width;
    bd->DisplayHeight = display_height;
    bd->Time = Now();
    bd->MouseX = (float)(display_width / 2);
    bd->MouseY = (float)(display_height / 2);
    bd->MouseSpeed = 1.0f;
    bd->Keymap = BuildLayout(ImGui_ImplSirius_Layout_US);

    io.BackendPlatformUserData = (void*)bd;
    io.BackendPlatformName = "imgui_impl_sirius";
    // No hardware cursor exists: Dear ImGui draws one, in whatever shape it wants.
    io.MouseDrawCursor = true;
    io.AddMousePosEvent(bd->MouseX, bd->MouseY);

    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Platform_GetClipboardTextFn = GetClipboardText;
    platform_io.Platform_SetClipboardTextFn = SetClipboardText;
    return true;
}

void ImGui_ImplSirius_Shutdown()
{
    ImGui_ImplSirius_Data* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "No platform backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

    platform_io.Platform_GetClipboardTextFn = nullptr;
    platform_io.Platform_SetClipboardTextFn = nullptr;
    io.BackendPlatformName = nullptr;
    io.BackendPlatformUserData = nullptr;
    io.MouseDrawCursor = false;
    IM_DELETE(bd);
}

void ImGui_ImplSirius_NewFrame()
{
    ImGui_ImplSirius_Data* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplSirius_Init()?");
    ImGuiIO& io = ImGui::GetIO();

    io.DisplaySize = ImVec2((float)bd->DisplayWidth, (float)bd->DisplayHeight);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    // A coarse clock can report the same instant twice; Dear ImGui needs
    // time to move forward.
    const double now = Now();
    io.DeltaTime = now > bd->Time ? (float)(now - bd->Time) : 1.0f / 1000.0f;
    bd->Time = now;
}

#endif // #ifndef IMGUI_DISABLE
