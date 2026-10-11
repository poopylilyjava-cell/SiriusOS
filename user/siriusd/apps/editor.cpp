// Editor: a plain text editor. One file, one window.
#include "apps.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace desktop {
namespace {

constexpr size_t kMaxFile = 1024 * 1024;

class Editor final : public App
{
public:
    explicit Editor(const std::string& path) : m_path(path)
    {
        if (!m_path.empty()) Load();
        std::snprintf(m_path_input, sizeof(m_path_input), "%s", m_path.c_str());
    }

    std::string Title() const override
    {
        const size_t slash = m_path.find_last_of('/');
        const std::string name = m_path.empty() ? "untitled" : m_path.substr(slash == std::string::npos ? 0 : slash + 1);
        return std::string("Editor - ") + name + (m_modified ? " *" : "");
    }
    ImVec2 DefaultSize() const override { return ImVec2(680.0f, 480.0f); }
    ImGuiWindowFlags WindowFlags() const override { return m_modified ? ImGuiWindowFlags_UnsavedDocument : 0; }

    void Draw() override
    {
        const bool save_shortcut = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                   ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false);
        ImGui::BeginDisabled(m_read_only);
        if (ImGui::Button("Save") || (save_shortcut && !m_read_only)) Save();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##path", "where to save: a path", m_path_input, sizeof(m_path_input)))
            m_path = m_path_input;

        const float status_height = ImGui::GetTextLineHeightWithSpacing();
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize;
        if (m_read_only) flags |= ImGuiInputTextFlags_ReadOnly;
        // Dear ImGui edits a char buffer in place and asks, through the
        // callback, for it to grow: the std::string is that buffer.
        if (ImGui::InputTextMultiline("##text", m_text.data(), m_text.capacity() + 1, ImVec2(-FLT_MIN, -status_height), flags,
                                      &Editor::ResizeCallback, this))
            m_modified = true;

        if (!m_message.empty()) ImGui::TextUnformatted(m_message.c_str());
        else ImGui::TextDisabled("%zu bytes%s - Ctrl+S saves", m_text.size(), m_read_only ? ", read only" : "");
    }

private:
    static int ResizeCallback(ImGuiInputTextCallbackData* data)
    {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
        {
            auto* self = (Editor*)data->UserData;
            self->m_text.resize((size_t)data->BufTextLen);
            data->Buf = self->m_text.data();
        }
        return 0;
    }

    void Load()
    {
        FILE* file = std::fopen(m_path.c_str(), "rb");
        if (file == nullptr) { m_message = std::string("cannot open: ") + std::strerror(errno); return; }
        m_text.clear();
        char chunk[4096];
        size_t n;
        while ((n = std::fread(chunk, 1, sizeof(chunk), file)) > 0 && m_text.size() < kMaxFile)
            m_text.append(chunk, n);
        const bool truncated = n > 0;
        std::fclose(file);
        // A NUL would end the text as far as the editing widget is concerned,
        // and saving would then cut the file short.
        const bool binary = m_text.find('\0') != std::string::npos;
        if (binary) m_text.resize(m_text.find('\0'));
        m_read_only = truncated || binary;
        m_message = truncated ? "too large to edit: showing the first megabyte, read only"
                  : binary ? "not a text file: shown up to its first NUL byte, read only" : "";
    }

    void Save()
    {
        if (m_path.empty()) { m_message = "give the file a path first"; return; }
        FILE* file = std::fopen(m_path.c_str(), "wb");
        if (file == nullptr) { m_message = std::string("cannot save: ") + std::strerror(errno); return; }
        const bool written = std::fwrite(m_text.data(), 1, m_text.size(), file) == m_text.size();
        const bool closed = std::fclose(file) == 0;
        if (written && closed) { m_modified = false; m_message = "saved"; }
        else m_message = std::string("cannot save: ") + std::strerror(errno);
    }

    std::string m_path;
    char m_path_input[512] = {};
    std::string m_text;
    bool m_modified = false;
    bool m_read_only = false;
    std::string m_message;
};

} // namespace

App* CreateEditor(const std::string& path) { return new Editor(path); }

} // namespace desktop
