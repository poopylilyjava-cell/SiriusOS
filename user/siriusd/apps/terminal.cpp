// Terminal: a window onto a shell.
//
// There are no terminals in the kernel -- no ttys, no line discipline -- so
// this is not a terminal emulator in the VT100 sense. It runs /bin/sh with
// pipes for its standard streams, shows what comes out of them, and sends
// what the user types a line at a time. That is enough for a command line:
// editing happens here, in the input field, before the shell sees anything.
// What it cannot host is a program that wants to draw on the screen itself.
#include "apps.h"

#include <cerrno>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace desktop {
namespace {

constexpr size_t kScrollback = 2000;   // lines kept

class Terminal final : public App
{
public:
    explicit Terminal(const std::string& directory) { Spawn(directory); }

    ~Terminal() override
    {
        if (m_to_shell >= 0) close(m_to_shell);
        if (m_from_shell >= 0) close(m_from_shell);
        if (m_pid > 0)
        {
            // Closing its input ends an idle shell; the signal ends one that
            // is busy. Either way it is reaped, here or by the desktop.
            kill(m_pid, SIGHUP);
            ReapLater(m_pid);
        }
    }

    std::string Title() const override { return m_exited ? "Terminal (finished)" : "Terminal"; }
    ImVec2 DefaultSize() const override { return ImVec2(720.0f, 440.0f); }

    void CollectFds(std::vector<int>& fds) override
    {
        if (m_from_shell >= 0) fds.push_back(m_from_shell);
    }

    void Update() override
    {
        if (m_from_shell < 0)
            return;
        char buffer[4096];
        for (;;)
        {
            const ssize_t n = read(m_from_shell, buffer, sizeof(buffer));
            if (n > 0) { Feed(buffer, (size_t)n); continue; }
            if (n == 0)
            {
                // Every writer is gone: the shell, and whatever it started.
                close(m_from_shell);
                m_from_shell = -1;
                if (m_to_shell >= 0) { close(m_to_shell); m_to_shell = -1; }
                if (m_pid > 0) { ReapLater(m_pid); m_pid = -1; }
                m_exited = true;
                Feed("\n[the shell has exited]\n", 24);
            }
            break;   // nothing more for now (EAGAIN), or an error
        }
    }

    void Draw() override
    {
        // The output, with the line being written -- usually the prompt --
        // last, and the input field directly under it.
        const float input_height = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("output", ImVec2(0.0f, -input_height), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        ImGuiListClipper clipper;
        clipper.Begin((int)m_lines.size());
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
                ImGui::TextUnformatted(m_lines[(size_t)i].c_str(), m_lines[(size_t)i].c_str() + m_lines[(size_t)i].size());
        if (m_scroll_to_end || ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
        m_scroll_to_end = false;
        ImGui::EndChild();

        ImGui::BeginDisabled(m_exited);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory;
        if (ImGui::InputTextWithHint("##input", m_exited ? "" : "type a command", m_input, sizeof(m_input), flags, &Terminal::InputCallback, this))
        {
            Submit();
            m_refocus = true;
        }
        const bool input_active = ImGui::IsItemActive();
        ImGui::EndDisabled();

        // Typing goes to the command line whenever the window has the focus.
        if (m_refocus || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsWindowAppearing()))
            ImGui::SetKeyboardFocusHere(-1);
        m_refocus = false;
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !input_active && !m_exited &&
            !ImGui::IsAnyItemActive() && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            m_refocus = true;

        if (input_active && !m_exited && ImGui::GetIO().KeyCtrl)
        {
            // Ctrl+C interrupts what is running; Ctrl+D on an empty line
            // ends the shell's input, as it would on a real terminal.
            if (ImGui::IsKeyPressed(ImGuiKey_C, false) && m_pid > 0)
            {
                kill(m_pid, SIGINT);
                Feed("^C\n", 3);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_D, false) && m_input[0] == '\0' && m_to_shell >= 0)
            {
                close(m_to_shell);
                m_to_shell = -1;
            }
        }
    }

private:
    void Spawn(const std::string& directory)
    {
        int to_shell[2], from_shell[2];
        if (pipe(to_shell) != 0) { Fail("pipe"); return; }
        if (pipe(from_shell) != 0) { Fail("pipe"); close(to_shell[0]); close(to_shell[1]); return; }

        const pid_t pid = fork();
        if (pid < 0)
        {
            Fail("fork");
            close(to_shell[0]); close(to_shell[1]); close(from_shell[0]); close(from_shell[1]);
            return;
        }
        if (pid == 0)
        {
            dup2(to_shell[0], STDIN_FILENO);
            dup2(from_shell[1], STDOUT_FILENO);
            dup2(from_shell[1], STDERR_FILENO);
            // Nothing of the desktop's goes along: not the display, not the
            // input device, not other terminals' pipes.
            for (int fd = 3; fd < 256; fd++) close(fd);
            signal(SIGPIPE, SIG_DFL);
            if (!directory.empty() && chdir(directory.c_str()) != 0) chdir("/");
            setenv("TERM", "dumb", 1);
            setenv("PATH", "/bin", 0);
            setenv("HOME", "/", 0);
            execl("/bin/sh", "sh", "-i", (char*)nullptr);
            const char message[] = "cannot run /bin/sh\n";
            (void)!write(STDOUT_FILENO, message, sizeof(message) - 1);
            _exit(127);
        }

        close(to_shell[0]);
        close(from_shell[1]);
        m_pid = pid;
        m_to_shell = to_shell[1];
        m_from_shell = from_shell[0];
        fcntl(m_from_shell, F_SETFL, O_NONBLOCK);
        fcntl(m_to_shell, F_SETFD, FD_CLOEXEC);
        fcntl(m_from_shell, F_SETFD, FD_CLOEXEC);
    }

    void Fail(const char* what)
    {
        const std::string message = std::string("cannot start a shell: ") + what + ": " + std::strerror(errno) + "\n";
        Feed(message.c_str(), message.size());
        m_exited = true;
    }

    // The shell does not echo: it is not on a terminal. Show the line where
    // it was typed, after the prompt, then send it.
    void Submit()
    {
        const std::string line = m_input;
        Feed(line.c_str(), line.size());
        Feed("\n", 1);
        if (!line.empty() && (m_history.empty() || m_history.back() != line))
            m_history.push_back(line);
        m_history_position = -1;
        m_input[0] = '\0';
        m_scroll_to_end = true;

        if (m_to_shell >= 0)
        {
            const std::string bytes = line + "\n";
            if (write(m_to_shell, bytes.c_str(), bytes.size()) < 0)
                Feed("[the shell is not listening]\n", 29);
        }
    }

    // Output, a byte at a time: just enough of a terminal to show a shell.
    void Feed(const char* data, size_t length)
    {
        if (m_lines.empty()) m_lines.emplace_back();
        for (size_t i = 0; i < length; i++)
        {
            const unsigned char c = (unsigned char)data[i];
            if (m_escape)
            {
                // ESC [ parameters letter. "2J" clears; everything else --
                // colours, cursor movement -- is swallowed.
                if (m_escape == 1) { m_escape = c == '[' ? 2 : 0; m_escape_text.clear(); continue; }
                if (c >= 0x40 && c <= 0x7e)
                {
                    if (c == 'J' && m_escape_text == "2") { m_lines.clear(); m_lines.emplace_back(); m_column = 0; }
                    m_escape = 0;
                }
                else m_escape_text += (char)c;
                continue;
            }
            std::string& line = m_lines.back();
            switch (c)
            {
            case '\033': m_escape = 1; break;
            case '\n':
                m_lines.emplace_back();
                m_column = 0;
                if (m_lines.size() > kScrollback) m_lines.erase(m_lines.begin(), m_lines.begin() + (long)(m_lines.size() - kScrollback));
                break;
            case '\r': m_column = 0; break;
            case '\b': if (m_column > 0) m_column--; break;
            case '\t': do Put(m_lines.back(), ' '); while (m_column % 8); break;
            case '\a': break;
            default:   if (c >= 0x20) Put(line, (char)c); break;
            }
        }
    }

    void Put(std::string& line, char c)
    {
        if (m_column < line.size()) line[m_column] = c;   // after a carriage return: overwrite
        else line += c;
        m_column++;
    }

    // Up and Down walk the commands typed before.
    static int InputCallback(ImGuiInputTextCallbackData* data)
    {
        auto* self = (Terminal*)data->UserData;
        if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || self->m_history.empty())
            return 0;
        const int count = (int)self->m_history.size();
        int position = self->m_history_position;
        if (data->EventKey == ImGuiKey_UpArrow)
            position = position < 0 ? count - 1 : (position > 0 ? position - 1 : 0);
        else if (data->EventKey == ImGuiKey_DownArrow)
            position = position < 0 ? -1 : (position + 1 < count ? position + 1 : -1);
        if (position != self->m_history_position)
        {
            self->m_history_position = position;
            data->DeleteChars(0, data->BufTextLen);
            if (position >= 0) data->InsertChars(0, self->m_history[(size_t)position].c_str());
        }
        return 0;
    }

    pid_t m_pid = -1;
    int m_to_shell = -1, m_from_shell = -1;
    bool m_exited = false;

    std::vector<std::string> m_lines;
    size_t m_column = 0;            // where the next character goes in the last line
    int m_escape = 0;               // 0 text, 1 after ESC, 2 inside ESC [
    std::string m_escape_text;
    bool m_scroll_to_end = true;

    char m_input[512] = {};
    bool m_refocus = true;
    std::vector<std::string> m_history;
    int m_history_position = -1;
};

} // namespace

App* CreateTerminal(const std::string& directory) { return new Terminal(directory); }

} // namespace desktop
