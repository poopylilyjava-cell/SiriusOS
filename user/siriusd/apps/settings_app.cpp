// Settings: the desktop's look, the keyboard and the mouse. Changes apply as
// they are made; "Save" keeps them for the next boot.
#include "apps.h"
#include "../shell/settings.h"

#include <sirius/system.hpp>

namespace desktop {
namespace {

class SettingsApp final : public App
{
public:
    std::string Title() const override { return "Settings"; }
    ImVec2 DefaultSize() const override { return ImVec2(460.0f, 470.0f); }

    void Draw() override
    {
        Settings& s = GetSettings();
        bool changed = false;

        ImGui::SeparatorText("Appearance");
        changed |= ImGui::Combo("Theme", &s.Theme, "Dark\0Light\0Classic\0");
        changed |= ImGui::ColorEdit3("Accent", s.Accent, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::SliderFloat("Text size", &s.FontSize, 10.0f, 24.0f, "%.0f px");
        changed |= ImGui::SliderFloat("Corners", &s.Rounding, 0.0f, 12.0f, "%.0f px");

        ImGui::SeparatorText("Wallpaper");
        changed |= ImGui::Combo("Style", &s.Wallpaper, "Gradient\0Solid colour\0");
        changed |= ImGui::ColorEdit3(s.Wallpaper == 0 ? "Top" : "Colour", s.WallpaperTop, ImGuiColorEditFlags_NoInputs);
        if (s.Wallpaper == 0)
            changed |= ImGui::ColorEdit3("Bottom", s.WallpaperBottom, ImGuiColorEditFlags_NoInputs);

        ImGui::SeparatorText("Keyboard and mouse");
        changed |= ImGui::Combo("Keyboard layout", &s.KeyboardLayout, "English (US)\0German\0");
        changed |= ImGui::SliderFloat("Pointer speed", &s.MouseSpeed, 0.25f, 4.0f, "%.2fx");
        ImGui::InputTextWithHint("##try", "try the keyboard here", m_scratch, sizeof(m_scratch));

        ImGui::SeparatorText("Panel");
        changed |= ImGui::Checkbox("Show seconds in the clock", &s.ShowSeconds);

        // Not in the middle of this window's own widgets: the style would
        // change under them. The desktop applies it between frames.
        if (changed) RequestApplySettings();

        ImGui::Separator();
        if (ImGui::Button("Save"))
            m_message = SaveSettings() ? "saved: these settings will be used at the next boot" : "could not write the settings file";
        ImGui::SameLine();
        if (ImGui::Button("Defaults"))
        {
            s = Settings();
            RequestApplySettings();
            m_message = "defaults restored (not saved)";
        }
        if (!m_message.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", m_message.c_str());
        }
    }

private:
    char m_scratch[128] = {};
    std::string m_message;
};

class About final : public App
{
public:
    std::string Title() const override { return "About Sirius"; }
    ImVec2 DefaultSize() const override { return ImVec2(420.0f, 250.0f); }
    ImGuiWindowFlags WindowFlags() const override { return ImGuiWindowFlags_NoResize; }

    void Draw() override
    {
        ImGui::SeparatorText("Sirius OS");
        ImGui::TextWrapped("An x86-64 operating system written from scratch. Its desktop is one Dear ImGui "
                           "program that owns the framebuffer and draws everything in software.");
        ImGui::Spacing();
        sirius::SystemInfo info;
        if (sirius::system_info(info))
        {
            ImGui::BulletText("%zu CPUs, %s of memory", info.cpu_ticks.size(), FormatSize(info.memory_total).c_str());
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::BulletText("Display %d x %d", (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        }
        ImGui::BulletText("Dear ImGui %s", ImGui::GetVersion());
        ImGui::BulletText("C library: mlibc; compiler: GCC");
    }
};

} // namespace

App* CreateSettings(const std::string&) { return new SettingsApp(); }
App* CreateAbout(const std::string&) { return new About(); }

} // namespace desktop
