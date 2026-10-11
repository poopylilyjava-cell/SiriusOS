// System Monitor: what the machine is doing -- each CPU's load, memory in
// use, and the processes, with a way to end one.
#include "apps.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <map>

#include <sirius/system.hpp>

namespace desktop {
namespace {

constexpr int kHistory = 120;           // samples kept for the graphs
constexpr double kInterval = 0.5;       // seconds between samples

class Monitor final : public App
{
public:
    std::string Title() const override { return "System Monitor"; }
    ImVec2 DefaultSize() const override { return ImVec2(620.0f, 520.0f); }

    void Update() override
    {
        const double now = ImGui::GetTime();
        if (now - m_sampled_at < kInterval && m_sampled_at != 0.0)
            return;
        Sample(now - m_sampled_at);
        m_sampled_at = now;
    }

    void Draw() override
    {
        const long seconds = (long)m_info.uptime_seconds;
        ImGui::Text("Up %ld:%02ld:%02ld    %u processes    %zu CPUs", seconds / 3600, seconds / 60 % 60, seconds % 60,
                    m_info.process_count, m_info.cpu_ticks.size());
        ImGui::SeparatorText("Processors");
        for (size_t cpu = 0; cpu < m_load.size(); cpu++)
        {
            char label[32], overlay[32];
            std::snprintf(label, sizeof(label), "##cpu%zu", cpu);
            std::snprintf(overlay, sizeof(overlay), "CPU %zu  %3.0f%%", cpu, (double)m_load[cpu].back() * 100.0);
            ImGui::PlotLines(label, m_load[cpu].data(), kHistory, 0, overlay, 0.0f, 1.0f, ImVec2(-FLT_MIN, 36.0f));
        }

        ImGui::SeparatorText("Memory");
        const unsigned long long used = m_info.memory_total - m_info.memory_free;
        const std::string text = FormatSize(used) + " of " + FormatSize(m_info.memory_total);
        ImGui::ProgressBar(m_info.memory_total ? (float)((double)used / (double)m_info.memory_total) : 0.0f, ImVec2(-FLT_MIN, 0.0f), text.c_str());
        ImGui::PlotLines("##memory", m_memory.data(), kHistory, 0, nullptr, 0.0f, 1.0f, ImVec2(-FLT_MIN, 36.0f));

        ImGui::SeparatorText("Processes");
        ImGui::BeginDisabled(m_selected_pid == 0);
        if (ImGui::Button("End process") && m_selected_pid != 0)
            kill((pid_t)m_selected_pid, SIGTERM);
        ImGui::SameLine();
        if (ImGui::Button("Kill") && m_selected_pid != 0)
            kill((pid_t)m_selected_pid, SIGKILL);
        ImGui::EndDisabled();

        if (ImGui::BeginTable("processes", 6, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();
            for (const Row& row : m_rows)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char pid[16];
                std::snprintf(pid, sizeof(pid), "%u", row.Info.pid);
                if (ImGui::Selectable(pid, m_selected_pid == row.Info.pid, ImGuiSelectableFlags_SpanAllColumns))
                    m_selected_pid = row.Info.pid;
                ImGui::TableNextColumn(); ImGui::TextUnformatted(row.Info.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.Info.state == sirius::ProcessState::Running ? "running"
                                     : row.Info.state == sirius::ProcessState::Sleeping ? "sleeping" : "zombie");
                ImGui::TableNextColumn(); ImGui::Text("%u", row.Info.cpu);
                ImGui::TableNextColumn(); ImGui::Text("%3.0f%%", (double)row.Load * 100.0);
                ImGui::TableNextColumn(); ImGui::Text("%.2f s", (double)row.Info.cpu_ticks / m_info.tick_hz);
            }
            ImGui::EndTable();
        }
    }

private:
    struct Row { sirius::ProcessInfo Info; float Load; };

    static void Push(std::vector<float>& history, float value)
    {
        if (history.size() != (size_t)kHistory) history.assign(kHistory, 0.0f);
        history.erase(history.begin());
        history.push_back(value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value));
    }

    void Sample(double elapsed)
    {
        sirius::SystemInfo info;
        if (!sirius::system_info(info))
            return;

        // Load is the share of the ticks since the last sample that did not
        // find the CPU idle.
        m_load.resize(info.cpu_ticks.size());
        for (size_t cpu = 0; cpu < info.cpu_ticks.size(); cpu++)
        {
            float load = 0.0f;
            if (cpu < m_info.cpu_ticks.size() && info.cpu_ticks[cpu] > m_info.cpu_ticks[cpu])
            {
                const double ticks = (double)(info.cpu_ticks[cpu] - m_info.cpu_ticks[cpu]);
                const double idle = (double)(info.cpu_idle_ticks[cpu] - m_info.cpu_idle_ticks[cpu]);
                load = (float)(1.0 - idle / ticks);
            }
            Push(m_load[cpu], load);
        }
        Push(m_memory, info.memory_total ? (float)((double)(info.memory_total - info.memory_free) / (double)info.memory_total) : 0.0f);

        std::map<unsigned, unsigned long long> previous;
        for (const Row& row : m_rows) previous[row.Info.pid] = row.Info.cpu_ticks;
        m_rows.clear();
        bool selection_alive = false;
        for (const sirius::ProcessInfo& process : sirius::processes())
        {
            float load = 0.0f;
            const auto before = previous.find(process.pid);
            if (before != previous.end() && elapsed > 0.0 && process.cpu_ticks >= before->second)
                load = (float)((double)(process.cpu_ticks - before->second) / info.tick_hz / elapsed);
            m_rows.push_back({ process, load > 1.0f ? 1.0f : load });
            if (process.pid == m_selected_pid) selection_alive = true;
        }
        std::sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) { return a.Info.pid < b.Info.pid; });
        if (!selection_alive) m_selected_pid = 0;
        m_info = info;
    }

    sirius::SystemInfo m_info;
    double m_sampled_at = 0.0;
    std::vector<std::vector<float>> m_load;   // per CPU
    std::vector<float> m_memory = std::vector<float>((size_t)kHistory, 0.0f);
    std::vector<Row> m_rows;
    unsigned m_selected_pid = 0;
};

} // namespace

App* CreateMonitor(const std::string&) { return new Monitor(); }

} // namespace desktop
