// Files: browse the filesystem, and do the few things to it the kernel can:
// make and delete files and directories, and open a file in the editor.
#include "apps.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace desktop {
namespace {

struct Entry
{
    std::string Name;
    bool Directory = false;
    char Type = '-';
    unsigned long long Size = 0;
};

std::string Join(const std::string& directory, const std::string& name)
{
    return directory == "/" ? "/" + name : directory + "/" + name;
}

std::string Parent(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}

class Files final : public App
{
public:
    explicit Files(const std::string& directory) { Go(directory.empty() ? "/" : directory); }

    std::string Title() const override { return "Files - " + m_path; }
    ImVec2 DefaultSize() const override { return ImVec2(640.0f, 460.0f); }

    void Draw() override
    {
        if (ImGui::Button("Up")) Go(Parent(m_path));
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) Go(m_path);
        ImGui::SameLine();
        if (ImGui::Button("New folder")) OpenNamePopup(true);
        ImGui::SameLine();
        if (ImGui::Button("New file")) OpenNamePopup(false);
        ImGui::SameLine();
        ImGui::BeginDisabled(m_selected < 0);
        if (ImGui::Button("Delete")) ImGui::OpenPopup("Delete?");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Terminal here")) Launch("Terminal", m_path);

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##path", m_path_input, sizeof(m_path_input), ImGuiInputTextFlags_EnterReturnsTrue))
            Go(m_path_input);

        const float status_height = ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::BeginTable("entries", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_Resizable, ImVec2(0.0f, -status_height)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableHeadersRow();

            std::string open_directory, open_file;
            for (int i = 0; i < (int)m_entries.size(); i++)
            {
                const Entry& entry = m_entries[(size_t)i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                const std::string label = entry.Directory ? entry.Name + "/" : entry.Name;
                if (ImGui::Selectable(label.c_str(), m_selected == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_selected = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        (entry.Directory ? open_directory : open_file) = Join(m_path, entry.Name);
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(KindName(entry));
                ImGui::TableNextColumn();
                if (!entry.Directory) ImGui::TextUnformatted(FormatSize(entry.Size).c_str());
            }
            ImGui::EndTable();
            // After the table: changing directory replaces what it was listing.
            if (!open_directory.empty()) Go(open_directory);
            if (!open_file.empty()) Launch("Editor", open_file);
        }

        if (!m_message.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m_message.c_str());
        else ImGui::TextDisabled("%d items%s", (int)m_entries.size(), m_selected >= 0 ? " - double-click to open" : "");

        DrawNamePopup();
        DrawDeletePopup();
    }

private:
    static const char* KindName(const Entry& entry)
    {
        switch (entry.Type)
        {
        case 'd': return "folder";
        case 'c': return "device";
        case 's': return "socket";
        default:  return "file";
        }
    }

    void Go(const std::string& path)
    {
        DIR* dir = opendir(path.c_str());
        if (dir == nullptr)
        {
            m_message = path + ": " + std::strerror(errno);
            std::snprintf(m_path_input, sizeof(m_path_input), "%s", m_path.c_str());
            return;
        }
        m_path = path;
        while (m_path.size() > 1 && m_path.back() == '/') m_path.pop_back();
        std::snprintf(m_path_input, sizeof(m_path_input), "%s", m_path.c_str());
        m_entries.clear();
        m_selected = -1;
        m_message.clear();
        while (const dirent* item = readdir(dir))
        {
            if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0)
                continue;
            Entry entry;
            entry.Name = item->d_name;
            struct stat st {};
            if (stat(Join(m_path, entry.Name).c_str(), &st) == 0)
            {
                entry.Directory = S_ISDIR(st.st_mode);
                entry.Type = entry.Directory ? 'd' : S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) ? 'c' : S_ISSOCK(st.st_mode) ? 's' : '-';
                entry.Size = (unsigned long long)st.st_size;
            }
            m_entries.push_back(entry);
        }
        closedir(dir);
        std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
            return a.Directory != b.Directory ? a.Directory : a.Name < b.Name;
        });
    }

    void OpenNamePopup(bool folder)
    {
        m_new_is_folder = folder;
        m_new_name[0] = '\0';
        ImGui::OpenPopup("New");
    }

    void DrawNamePopup()
    {
        if (!ImGui::BeginPopupModal("New", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        ImGui::Text("Name of the new %s:", m_new_is_folder ? "folder" : "file");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool entered = ImGui::InputText("##name", m_new_name, sizeof(m_new_name), ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("Create") || entered) && m_new_name[0])
        {
            const std::string path = Join(m_path, m_new_name);
            int result;
            if (m_new_is_folder) result = mkdir(path.c_str(), 0755);
            else
            {
                result = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
                if (result >= 0) { close(result); result = 0; }
            }
            const std::string error = result != 0 ? std::string(m_new_name) + ": " + std::strerror(errno) : "";
            Go(m_path);
            m_message = error;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void DrawDeletePopup()
    {
        if (!ImGui::BeginPopupModal("Delete?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        if (m_selected < 0 || m_selected >= (int)m_entries.size()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        const Entry& entry = m_entries[(size_t)m_selected];
        ImGui::Text("Delete \"%s\"? This cannot be undone.", entry.Name.c_str());
        if (ImGui::Button("Delete"))
        {
            const std::string path = Join(m_path, entry.Name);
            const int result = entry.Directory ? rmdir(path.c_str()) : unlink(path.c_str());
            const std::string error = result != 0 ? entry.Name + ": " + std::strerror(errno) : "";
            Go(m_path);
            m_message = error;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    std::string m_path = "/";
    char m_path_input[512] = "/";
    std::vector<Entry> m_entries;
    int m_selected = -1;
    std::string m_message;
    bool m_new_is_folder = false;
    char m_new_name[128] = {};
};

} // namespace

App* CreateFiles(const std::string& directory) { return new Files(directory); }

std::string FormatSize(unsigned long long bytes)
{
    char text[32];
    if (bytes < 1024) std::snprintf(text, sizeof(text), "%llu bytes", bytes);
    else if (bytes < 1024 * 1024) std::snprintf(text, sizeof(text), "%.1f KiB", (double)bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024) std::snprintf(text, sizeof(text), "%.1f MiB", (double)bytes / (1024.0 * 1024.0));
    else std::snprintf(text, sizeof(text), "%.2f GiB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    return text;
}

} // namespace desktop
