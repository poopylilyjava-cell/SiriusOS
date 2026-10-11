// Host-side tests for the Sirius platform backend (imgui_impl_sirius): the
// same event records the kernel's input device produces go in, and Dear
// ImGui's view of the keyboard and mouse comes out.

#include <cstdio>
#include <string>

#include "imgui.h"
#include "imgui_impl_sirius.h"

static int g_failures = 0;

static void expect(bool condition, const char* what) {
    if (condition) return;
    printf("  FAIL  %s\n", what);
    g_failures++;
}

namespace {

void send(uint16_t type, uint16_t code, int32_t value) {
    const sirius_input_event event{ 0, type, code, value };
    ImGui_ImplSirius_ProcessEvent(&event);
}

void tap(uint16_t code) { send(SIRIUS_EV_KEY, code, 1); send(SIRIUS_EV_KEY, code, 0); }

float g_wheel = 0.0f;   // as of the last frame: Dear ImGui clears it when the frame ends

// The characters queued for text input by the events since the last call.
std::string typed() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplSirius_NewFrame();
    ImGui::NewFrame();
    std::string text;
    for (ImWchar c : io.InputQueueCharacters) text += (char)c;
    g_wheel = io.MouseWheel;
    ImGui::EndFrame();
    return text;
}

} // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // Deliver every queued event in the next frame rather than one per frame.
    io.ConfigInputTrickleEventQueue = false;
    // No renderer is attached; claim one that manages textures, so that Dear
    // ImGui does not insist on a font atlas being built for it.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui_ImplSirius_Init(800, 600);
    typed();

    printf("keyboard:\n");
    {
        tap(SIRIUS_KEY_H); tap(SIRIUS_KEY_I); tap(SIRIUS_KEY_SPACE); tap(SIRIUS_KEY_1); tap(SIRIUS_KEY_0); tap(SIRIUS_KEY_SLASH);
        expect(typed() == "hi 10/", "plain keys type their US-layout characters");

        send(SIRIUS_EV_KEY, SIRIUS_KEY_LEFTSHIFT, 1);
        tap(SIRIUS_KEY_H); tap(SIRIUS_KEY_1); tap(SIRIUS_KEY_SLASH); tap(SIRIUS_KEY_MINUS);
        send(SIRIUS_EV_KEY, SIRIUS_KEY_LEFTSHIFT, 0);
        tap(SIRIUS_KEY_H);
        expect(typed() == "H!?_h", "shift selects the upper character, until released");

        tap(SIRIUS_KEY_CAPSLOCK);
        tap(SIRIUS_KEY_A); tap(SIRIUS_KEY_1);
        send(SIRIUS_EV_KEY, SIRIUS_KEY_RIGHTSHIFT, 1); tap(SIRIUS_KEY_A); send(SIRIUS_EV_KEY, SIRIUS_KEY_RIGHTSHIFT, 0);
        tap(SIRIUS_KEY_CAPSLOCK);
        tap(SIRIUS_KEY_A);
        expect(typed() == "A1aa", "caps lock affects letters only, and shift undoes it");

        send(SIRIUS_EV_KEY, SIRIUS_KEY_X, 1);
        send(SIRIUS_EV_KEY, SIRIUS_KEY_X, 2);
        send(SIRIUS_EV_KEY, SIRIUS_KEY_X, 2);
        send(SIRIUS_EV_KEY, SIRIUS_KEY_X, 0);
        expect(typed() == "xxx", "auto-repeat types the character again");

        send(SIRIUS_EV_KEY, SIRIUS_KEY_LEFTCTRL, 1);
        send(SIRIUS_EV_KEY, SIRIUS_KEY_C, 1);
        const std::string with_ctrl = typed();
        expect(with_ctrl.empty() && io.KeyCtrl && ImGui::IsKeyDown(ImGuiKey_C) && ImGui::IsKeyDown(ImGuiKey_LeftCtrl),
               "ctrl+c is a shortcut, not a character");
        send(SIRIUS_EV_KEY, SIRIUS_KEY_C, 0);
        send(SIRIUS_EV_KEY, SIRIUS_KEY_LEFTCTRL, 0);
        typed();
        expect(!io.KeyCtrl && !ImGui::IsKeyDown(ImGuiKey_C), "and releasing both clears them");

        send(SIRIUS_EV_KEY, SIRIUS_KEY_UP, 1); send(SIRIUS_EV_KEY, SIRIUS_KEY_F5, 1); send(SIRIUS_EV_KEY, SIRIUS_KEY_KPENTER, 1);
        expect(typed().empty() && ImGui::IsKeyDown(ImGuiKey_UpArrow) && ImGui::IsKeyDown(ImGuiKey_F5) &&
               ImGui::IsKeyDown(ImGuiKey_KeypadEnter), "navigation and function keys map to their ImGuiKey");
        send(SIRIUS_EV_KEY, SIRIUS_KEY_UP, 0); send(SIRIUS_EV_KEY, SIRIUS_KEY_F5, 0); send(SIRIUS_EV_KEY, SIRIUS_KEY_KPENTER, 0);

        send(SIRIUS_EV_KEY, 200, 1); send(SIRIUS_EV_KEY, 60000, 1);
        expect(typed().empty(), "unknown key codes are ignored");
    }

    printf("keyboard layouts:\n");
    {
        ImGui_ImplSirius_SetKeyboardLayout(ImGui_ImplSirius_Layout_DE);
        tap(SIRIUS_KEY_Y); tap(SIRIUS_KEY_Z); tap(SIRIUS_KEY_MINUS); tap(SIRIUS_KEY_SEMICOLON);
        expect(typed() == "zy\xdf\xf6", "German: y and z change places, and the umlauts have keys");

        send(SIRIUS_EV_KEY, SIRIUS_KEY_LEFTSHIFT, 1);
        tap(SIRIUS_KEY_2); tap(SIRIUS_KEY_7); tap(SIRIUS_KEY_SEMICOLON); tap(SIRIUS_KEY_SLASH);
        send(SIRIUS_EV_KEY, SIRIUS_KEY_LEFTSHIFT, 0);
        expect(typed() == "\"/\xd6_", "shift gives the German second level");

        // AltGr is the right Alt key: a third level, not the Alt of shortcuts.
        send(SIRIUS_EV_KEY, SIRIUS_KEY_RIGHTALT, 1);
        tap(SIRIUS_KEY_Q); tap(SIRIUS_KEY_7); tap(SIRIUS_KEY_0); tap(SIRIUS_KEY_MINUS); tap(SIRIUS_KEY_102ND);
        const std::string third = typed();
        expect(third == "@{}\\|" && !io.KeyAlt, "AltGr types @ { } \\ | without counting as Alt");
        send(SIRIUS_EV_KEY, SIRIUS_KEY_RIGHTALT, 0);

        tap(SIRIUS_KEY_CAPSLOCK); tap(SIRIUS_KEY_APOSTROPHE); tap(SIRIUS_KEY_1); tap(SIRIUS_KEY_CAPSLOCK);
        expect(typed() == "\xc4" "1", "caps lock capitalises an umlaut, and leaves digits alone");

        ImGui_ImplSirius_SetKeyboardLayout(ImGui_ImplSirius_Layout_US);
        send(SIRIUS_EV_KEY, SIRIUS_KEY_RIGHTALT, 1);
        tap(SIRIUS_KEY_Q);
        const std::string with_right_alt = typed();
        expect(with_right_alt.empty() && io.KeyAlt, "on the US layout the right Alt key is Alt");
        send(SIRIUS_EV_KEY, SIRIUS_KEY_RIGHTALT, 0);
        tap(SIRIUS_KEY_Y); tap(SIRIUS_KEY_Z);
        expect(typed() == "yz", "and the layout switches back");

        ImGui_ImplSirius_SetMouseSpeed(2.0f);
        typed();
        const float before = io.MousePos.x;
        send(SIRIUS_EV_REL, SIRIUS_REL_X, -10);
        typed();
        expect(io.MousePos.x == before - 20.0f, "pointer speed scales relative motion");
        ImGui_ImplSirius_SetMouseSpeed(1.0f);
        send(SIRIUS_EV_REL, SIRIUS_REL_X, 20);
    }

    printf("mouse:\n");
    {
        typed();
        expect(io.MousePos.x == 400 && io.MousePos.y == 300, "the pointer starts in the middle of the display");

        send(SIRIUS_EV_REL, SIRIUS_REL_X, 25); send(SIRIUS_EV_REL, SIRIUS_REL_Y, -100); send(SIRIUS_EV_SYN, 0, 0);
        typed();
        expect(io.MousePos.x == 425 && io.MousePos.y == 200, "relative motion moves it");

        send(SIRIUS_EV_REL, SIRIUS_REL_X, 100000); send(SIRIUS_EV_REL, SIRIUS_REL_Y, -100000);
        typed();
        expect(io.MousePos.x == 799 && io.MousePos.y == 0, "and stops at the edges of the display");

        send(SIRIUS_EV_ABS, SIRIUS_ABS_X, 0); send(SIRIUS_EV_ABS, SIRIUS_ABS_Y, SIRIUS_ABS_MAX);
        typed();
        expect(io.MousePos.x == 0 && io.MousePos.y == 599, "absolute coordinates span the display");

        send(SIRIUS_EV_KEY, SIRIUS_BTN_LEFT, 1); send(SIRIUS_EV_KEY, SIRIUS_BTN_MIDDLE, 1);
        typed();
        expect(io.MouseDown[0] && !io.MouseDown[1] && io.MouseDown[2], "buttons arrive as left, right, middle");
        send(SIRIUS_EV_KEY, SIRIUS_BTN_LEFT, 0); send(SIRIUS_EV_KEY, SIRIUS_BTN_MIDDLE, 0); send(SIRIUS_EV_KEY, SIRIUS_BTN_RIGHT, 1);
        typed();
        expect(!io.MouseDown[0] && io.MouseDown[1] && !io.MouseDown[2], "and are released");
        send(SIRIUS_EV_KEY, SIRIUS_BTN_RIGHT, 0);

        send(SIRIUS_EV_REL, SIRIUS_REL_WHEEL, -3);
        typed();
        expect(g_wheel == -3.0f, "the wheel scrolls");
    }

    printf("clipboard:\n");
    {
        ImGui::SetClipboardText("copied on Sirius");
        expect(std::string(ImGui::GetClipboardText()) == "copied on Sirius", "text set on the clipboard comes back");
    }

    ImGui_ImplSirius_Shutdown();
    ImGui::DestroyContext();

    if (g_failures) {
        printf("imgui_platform_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("imgui_platform_test: all passed\n");
    return 0;
}
