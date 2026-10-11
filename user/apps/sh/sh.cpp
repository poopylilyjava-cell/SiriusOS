// sh: the Sirius command shell, and its commands.
//
// A small shell -- words, quotes, $VARIABLES, pipes, < > >> redirection, ;
// and && -- with the everyday commands (ls, cat, cp, ps, ...) compiled into
// it, busybox-style: a static program is a megabyte of C library, and twenty
// of them would be twenty megabytes of the same code. A command that is not
// built in is looked for in /bin and executed.
//
//   sh            read commands from standard input
//   sh -i         the same, with a prompt even when input is not a terminal
//                 (the desktop's terminal talks to it through pipes)
//   sh -c 'cmd'   run one command line
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sirius/system.hpp>

extern char** environ;

namespace {

using Words = std::vector<std::string>;

int g_last_status = 0;
std::vector<pid_t> g_foreground;   // the pipeline being waited for: what Ctrl+C is aimed at

// --- commands -----------------------------------------------------------------------
// Each runs in a child of the shell, with the pipeline's redirections in
// place, and returns its exit status.

int fail(const char* command, const std::string& what) {
    std::fprintf(stderr, "%s: %s: %s\n", command, what.c_str(), std::strerror(errno));
    return 1;
}

int cmd_echo(const Words& args) {
    size_t first = 1;
    const bool newline = !(args.size() > 1 && args[1] == "-n");
    if (!newline) first = 2;
    for (size_t i = first; i < args.size(); i++) std::printf("%s%s", i > first ? " " : "", args[i].c_str());
    if (newline) std::putchar('\n');
    return 0;
}

int copy_stream(int from, int to) {
    char buffer[8192];
    for (;;) {
        const ssize_t n = read(from, buffer, sizeof(buffer));
        if (n < 0) return -1;
        if (n == 0) return 0;
        for (ssize_t done = 0; done < n;) {
            const ssize_t written = write(to, buffer + done, static_cast<size_t>(n - done));
            if (written < 0) return -1;
            done += written;
        }
    }
}

int cmd_cat(const Words& args) {
    std::fflush(stdout);
    if (args.size() == 1) return copy_stream(STDIN_FILENO, STDOUT_FILENO) < 0 ? fail("cat", "stdin") : 0;
    int status = 0;
    for (size_t i = 1; i < args.size(); i++) {
        const int fd = open(args[i].c_str(), O_RDONLY);
        if (fd < 0 || copy_stream(fd, STDOUT_FILENO) < 0) status = fail("cat", args[i]);
        if (fd >= 0) close(fd);
    }
    return status;
}

char type_letter(mode_t mode) {
    if (S_ISDIR(mode)) return 'd';
    if (S_ISCHR(mode)) return 'c';
    if (S_ISBLK(mode)) return 'b';
    if (S_ISSOCK(mode)) return 's';
    if (S_ISFIFO(mode)) return 'p';
    return '-';
}

int cmd_ls(const Words& args) {
    bool details = false, all = false;
    Words paths;
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i].size() > 1 && args[i][0] == '-') {
            for (char flag : args[i].substr(1)) { if (flag == 'l') details = true; else if (flag == 'a') all = true; }
        } else {
            paths.push_back(args[i]);
        }
    }
    if (paths.empty()) paths.push_back(".");

    int status = 0;
    for (const std::string& path : paths) {
        struct stat st {};
        if (stat(path.c_str(), &st) != 0) { status = fail("ls", path); continue; }

        Words names;
        if (S_ISDIR(st.st_mode)) {
            DIR* dir = opendir(path.c_str());
            if (dir == nullptr) { status = fail("ls", path); continue; }
            while (const dirent* entry = readdir(dir))
                if (all || entry->d_name[0] != '.') names.emplace_back(entry->d_name);
            closedir(dir);
            for (size_t i = 1; i < names.size(); i++)   // insertion sort: directories are small
                for (size_t j = i; j > 0 && names[j] < names[j - 1]; j--) std::swap(names[j], names[j - 1]);
            if (paths.size() > 1) std::printf("%s:\n", path.c_str());
        } else {
            names.push_back(path);
        }

        for (const std::string& name : names) {
            const std::string full = S_ISDIR(st.st_mode) ? path + "/" + name : name;
            struct stat entry {};
            const bool known = stat(full.c_str(), &entry) == 0;
            if (details)
                std::printf("%c %10lld  %s%s\n", known ? type_letter(entry.st_mode) : '?',
                            known ? static_cast<long long>(entry.st_size) : 0LL, name.c_str(),
                            known && S_ISDIR(entry.st_mode) ? "/" : "");
            else
                std::printf("%s%s\n", name.c_str(), known && S_ISDIR(entry.st_mode) ? "/" : "");
        }
    }
    return status;
}

int cmd_pwd(const Words&) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) return fail("pwd", ".");
    std::puts(cwd);
    return 0;
}

int cmd_mkdir(const Words& args) {
    int status = 0;
    for (size_t i = 1; i < args.size(); i++)
        if (mkdir(args[i].c_str(), 0755) != 0) status = fail("mkdir", args[i]);
    return args.size() > 1 ? status : (std::fprintf(stderr, "usage: mkdir DIRECTORY...\n"), 1);
}

int cmd_rmdir(const Words& args) {
    int status = 0;
    for (size_t i = 1; i < args.size(); i++)
        if (rmdir(args[i].c_str()) != 0) status = fail("rmdir", args[i]);
    return status;
}

int remove_tree(const std::string& path, bool recursive) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path.c_str());
    if (!recursive) { errno = EISDIR; return -1; }
    Words names;
    if (DIR* dir = opendir(path.c_str())) {
        while (const dirent* entry = readdir(dir))
            if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) names.emplace_back(entry->d_name);
        closedir(dir);
    }
    for (const std::string& name : names)
        if (remove_tree(path + "/" + name, true) != 0) return -1;
    return rmdir(path.c_str());
}

int cmd_rm(const Words& args) {
    bool recursive = false, force = false;
    int status = 0;
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i] == "-r" || args[i] == "-rf" || args[i] == "-fr") { recursive = true; force = force || args[i] != "-r"; continue; }
        if (args[i] == "-f") { force = true; continue; }
        if (remove_tree(args[i], recursive) != 0 && !(force && errno == ENOENT)) status = fail("rm", args[i]);
    }
    return status;
}

std::string base_name(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

int copy_file(const std::string& from, std::string to) {
    struct stat st {};
    if (stat(to.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) to += "/" + base_name(from);
    const int in = open(from.c_str(), O_RDONLY);
    if (in < 0) return -1;
    const int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { const int saved = errno; close(in); errno = saved; return -1; }
    const int result = copy_stream(in, out);
    const int saved = errno;
    close(in);
    close(out);
    errno = saved;
    return result;
}

int cmd_cp(const Words& args) {
    if (args.size() != 3) { std::fprintf(stderr, "usage: cp SOURCE DESTINATION\n"); return 1; }
    return copy_file(args[1], args[2]) != 0 ? fail("cp", args[1]) : 0;
}

// There is no rename system call yet: a move is a copy and a delete, which
// is also what it is across filesystems anywhere.
int cmd_mv(const Words& args) {
    if (args.size() != 3) { std::fprintf(stderr, "usage: mv SOURCE DESTINATION\n"); return 1; }
    if (copy_file(args[1], args[2]) != 0) return fail("mv", args[1]);
    return unlink(args[1].c_str()) != 0 ? fail("mv", args[1]) : 0;
}

int cmd_touch(const Words& args) {
    int status = 0;
    for (size_t i = 1; i < args.size(); i++) {
        const int fd = open(args[i].c_str(), O_WRONLY | O_CREAT, 0644);
        if (fd < 0) status = fail("touch", args[i]); else close(fd);
    }
    return status;
}

int cmd_wc(const Words& args) {
    std::FILE* in = args.size() > 1 ? std::fopen(args[1].c_str(), "r") : stdin;
    if (in == nullptr) return fail("wc", args[1]);
    long lines = 0, words = 0, bytes = 0;
    bool in_word = false;
    for (int c; (c = std::fgetc(in)) != EOF;) {
        bytes++;
        if (c == '\n') lines++;
        const bool space = c == ' ' || c == '\n' || c == '\t';
        if (!space && !in_word) words++;
        in_word = !space;
    }
    std::printf("%ld %ld %ld%s%s\n", lines, words, bytes, args.size() > 1 ? " " : "", args.size() > 1 ? args[1].c_str() : "");
    return 0;
}

int cmd_head(const Words& args) {
    long limit = 10;
    const char* path = nullptr;
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i] == "-n" && i + 1 < args.size()) limit = std::atol(args[++i].c_str());
        else path = args[i].c_str();
    }
    std::FILE* in = path ? std::fopen(path, "r") : stdin;
    if (in == nullptr) return fail("head", path);
    char line[1024];
    for (long n = 0; n < limit && std::fgets(line, sizeof(line), in); n++) std::fputs(line, stdout);
    return 0;
}

int cmd_grep(const Words& args) {
    if (args.size() < 2) { std::fprintf(stderr, "usage: grep TEXT [FILE]\n"); return 2; }
    std::FILE* in = args.size() > 2 ? std::fopen(args[2].c_str(), "r") : stdin;
    if (in == nullptr) return fail("grep", args[2]);
    char line[2048];
    bool found = false;
    while (std::fgets(line, sizeof(line), in))
        if (std::strstr(line, args[1].c_str())) { std::fputs(line, stdout); found = true; }
    return found ? 0 : 1;
}

int cmd_uname(const Words& args) {
    utsname name {};
    uname(&name);
    if (args.size() > 1 && args[1] == "-a")
        std::printf("%s %s %s %s %s\n", name.sysname, name.nodename, name.release, name.version, name.machine);
    else
        std::puts(name.sysname);
    return 0;
}

int cmd_date(const Words&) {
    const time_t now = std::time(nullptr);
    char text[64];
    std::strftime(text, sizeof(text), "%a %Y-%m-%d %H:%M:%S", std::gmtime(&now));
    std::puts(text);
    return 0;
}

int cmd_uptime(const Words&) {
    sirius::SystemInfo info;
    if (!sirius::system_info(info)) return fail("uptime", "sysinfo");
    const long seconds = static_cast<long>(info.uptime_seconds);
    std::printf("up %ld:%02ld:%02ld, %u processes, %zu cpus\n", seconds / 3600, seconds / 60 % 60, seconds % 60,
                info.process_count, info.cpu_ticks.size());
    return 0;
}

int cmd_free(const Words&) {
    sirius::SystemInfo info;
    if (!sirius::system_info(info)) return fail("free", "sysinfo");
    const unsigned long long mib = 1024 * 1024;
    std::printf("        total     used     free\nmemory %5llu M  %5llu M  %5llu M\n", info.memory_total / mib,
                (info.memory_total - info.memory_free) / mib, info.memory_free / mib);
    return 0;
}

int cmd_ps(const Words&) {
    sirius::SystemInfo info;
    sirius::system_info(info);
    std::printf("  PID  PPID CPU STATE     TIME  NAME\n");
    for (const sirius::ProcessInfo& process : sirius::processes()) {
        const char* state = process.state == sirius::ProcessState::Running ? "running"
                          : process.state == sirius::ProcessState::Sleeping ? "sleeping" : "zombie";
        std::printf("%5u %5u %3u %-8s %5.2f  %s\n", process.pid, process.parent, process.cpu, state,
                    static_cast<double>(process.cpu_ticks) / info.tick_hz, process.name.c_str());
    }
    return 0;
}

int cmd_kill(const Words& args) {
    int sig = SIGTERM, status = 0;
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i][0] == '-' && args[i].size() > 1) { sig = std::atoi(args[i].c_str() + 1); continue; }
        if (kill(static_cast<pid_t>(std::atoi(args[i].c_str())), sig) != 0) status = fail("kill", args[i]);
    }
    return status;
}

int cmd_sleep(const Words& args) {
    const double seconds = args.size() > 1 ? std::atof(args[1].c_str()) : 0;
    timespec request{ static_cast<time_t>(seconds), static_cast<long>((seconds - static_cast<double>(static_cast<long>(seconds))) * 1e9) };
    nanosleep(&request, nullptr);
    return 0;
}

int cmd_env(const Words&) {
    for (char** entry = environ; entry && *entry; entry++) std::puts(*entry);
    return 0;
}

// The terminal understands exactly this much of ANSI.
int cmd_clear(const Words&) {
    std::fputs("\033[2J\033[H", stdout);
    return 0;
}

int cmd_true(const Words&) { return 0; }
int cmd_false(const Words&) { return 1; }
int cmd_help(const Words&);

struct Command { const char* name; int (*run)(const Words&); const char* summary; };

const Command kCommands[] = {
    { "cat", cmd_cat, "print files, or standard input" },
    { "clear", cmd_clear, "clear the terminal" },
    { "cp", cmd_cp, "copy a file" },
    { "date", cmd_date, "the date and time" },
    { "echo", cmd_echo, "print its arguments" },
    { "env", cmd_env, "print the environment" },
    { "false", cmd_false, "fail" },
    { "free", cmd_free, "memory in use" },
    { "grep", cmd_grep, "print the lines containing a text" },
    { "head", cmd_head, "the first lines of a file (-n COUNT)" },
    { "help", cmd_help, "this list" },
    { "kill", cmd_kill, "signal a process (-SIGNAL PID)" },
    { "ls", cmd_ls, "list a directory (-l for sizes, -a for hidden names)" },
    { "mkdir", cmd_mkdir, "make directories" },
    { "mv", cmd_mv, "move a file" },
    { "ps", cmd_ps, "list processes" },
    { "pwd", cmd_pwd, "print the working directory" },
    { "rm", cmd_rm, "remove files (-r for directories)" },
    { "rmdir", cmd_rmdir, "remove empty directories" },
    { "sleep", cmd_sleep, "wait some seconds" },
    { "touch", cmd_touch, "create an empty file" },
    { "true", cmd_true, "succeed" },
    { "uname", cmd_uname, "name the system (-a for everything)" },
    { "uptime", cmd_uptime, "how long the system has been up" },
    { "wc", cmd_wc, "count lines, words and bytes" },
};

int cmd_help(const Words&) {
    std::puts("Built into the shell itself: cd, exit, export NAME=VALUE, unset NAME.");
    std::puts("Commands:");
    for (const Command& command : kCommands) std::printf("  %-7s %s\n", command.name, command.summary);
    std::puts("Anything else is run from /bin. Commands combine with |, <, >, >>, ; and &&.");
    return 0;
}

const Command* find_command(const std::string& name) {
    for (const Command& command : kCommands)
        if (name == command.name) return &command;
    return nullptr;
}

// --- parsing ------------------------------------------------------------------------

struct Stage {              // one command of a pipeline
    Words words;
    std::string input, output;
    bool append = false;
};

struct Pipeline {
    std::vector<Stage> stages;
    bool only_if_previous_succeeded = false;   // it followed "&&"
};

enum class Token { Word, Pipe, In, Out, Append, Semicolon, And, End };

// Split a line into words and operators. Quotes group, a backslash escapes,
// and $NAME and $? are replaced -- everywhere but inside single quotes.
bool tokenize(const std::string& line, std::vector<std::pair<Token, std::string>>& out) {
    size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '#') break;
        if (c == '|') { out.push_back({ Token::Pipe, "" }); i++; continue; }
        if (c == ';') { out.push_back({ Token::Semicolon, "" }); i++; continue; }
        if (c == '<') { out.push_back({ Token::In, "" }); i++; continue; }
        if (c == '>') {
            if (i + 1 < line.size() && line[i + 1] == '>') { out.push_back({ Token::Append, "" }); i += 2; }
            else { out.push_back({ Token::Out, "" }); i++; }
            continue;
        }
        if (c == '&' && i + 1 < line.size() && line[i + 1] == '&') { out.push_back({ Token::And, "" }); i += 2; continue; }

        std::string word;
        char quote = 0;
        for (; i < line.size(); i++) {
            const char ch = line[i];
            if (quote) {
                if (ch == quote) { quote = 0; continue; }
                if (quote == '\'') { word += ch; continue; }
            } else {
                if (ch == '\'' || ch == '"') { quote = ch; continue; }
                if (std::strchr(" \t|;<>", ch) || (ch == '&' && i + 1 < line.size() && line[i + 1] == '&')) break;
            }
            if (ch == '\\' && i + 1 < line.size()) { word += line[++i]; continue; }
            if (ch == '$' && i + 1 < line.size()) {
                if (line[i + 1] == '?') { word += std::to_string(g_last_status); i++; continue; }
                size_t end = i + 1;
                while (end < line.size() && (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_')) end++;
                if (end > i + 1) {
                    if (const char* value = std::getenv(line.substr(i + 1, end - i - 1).c_str())) word += value;
                    i = end - 1;
                    continue;
                }
            }
            word += ch;
        }
        if (quote) { std::fprintf(stderr, "sh: unterminated quote\n"); return false; }
        out.push_back({ Token::Word, word });
    }
    out.push_back({ Token::End, "" });
    return true;
}

bool parse(const std::string& line, std::vector<Pipeline>& out) {
    std::vector<std::pair<Token, std::string>> tokens;
    if (!tokenize(line, tokens)) return false;

    Pipeline pipeline;
    Stage stage;
    auto end_stage = [&]() {
        if (stage.words.empty()) return false;
        pipeline.stages.push_back(stage);
        stage = Stage();
        return true;
    };
    for (size_t i = 0; i < tokens.size(); i++) {
        const Token token = tokens[i].first;
        switch (token) {
        case Token::Word:
            stage.words.push_back(tokens[i].second);
            break;
        case Token::In: case Token::Out: case Token::Append:
            if (i + 1 >= tokens.size() || tokens[i + 1].first != Token::Word) { std::fprintf(stderr, "sh: a redirection needs a file name\n"); return false; }
            if (token == Token::In) stage.input = tokens[i + 1].second;
            else { stage.output = tokens[i + 1].second; stage.append = token == Token::Append; }
            i++;
            break;
        case Token::Pipe:
            if (!end_stage()) { std::fprintf(stderr, "sh: nothing before |\n"); return false; }
            break;
        case Token::Semicolon: case Token::And: case Token::End: {
            const bool had_stage = end_stage();
            if (!had_stage && !pipeline.stages.empty()) { std::fprintf(stderr, "sh: nothing after |\n"); return false; }
            if (!pipeline.stages.empty()) out.push_back(pipeline);
            else if (token == Token::And) { std::fprintf(stderr, "sh: nothing before &&\n"); return false; }
            pipeline = Pipeline();
            pipeline.only_if_previous_succeeded = token == Token::And;
            break;
        }
        }
    }
    return true;
}

// --- running ------------------------------------------------------------------------

// The commands that must change the shell itself, and so cannot run in a child.
bool run_in_shell(const Words& words, int& status) {
    const std::string& name = words[0];
    if (name == "cd") {
        const char* target = words.size() > 1 ? words[1].c_str() : "/";
        status = chdir(target) == 0 ? 0 : fail("cd", target);
        return true;
    }
    if (name == "exit") {
        std::fflush(nullptr);
        std::exit(words.size() > 1 ? std::atoi(words[1].c_str()) : g_last_status);
    }
    if (name == "export") {
        status = 0;
        for (size_t i = 1; i < words.size(); i++) {
            const size_t equals = words[i].find('=');
            if (equals == std::string::npos || equals == 0) { std::fprintf(stderr, "export: expected NAME=VALUE\n"); status = 1; continue; }
            setenv(words[i].substr(0, equals).c_str(), words[i].substr(equals + 1).c_str(), 1);
        }
        return true;
    }
    if (name == "unset") {
        for (size_t i = 1; i < words.size(); i++) unsetenv(words[i].c_str());
        status = 0;
        return true;
    }
    return false;
}

// In the child: become the command.
[[noreturn]] void run_stage(const Stage& stage) {
    if (!stage.input.empty()) {
        const int fd = open(stage.input.c_str(), O_RDONLY);
        if (fd < 0) { fail("sh", stage.input); std::exit(1); }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (!stage.output.empty()) {
        const int fd = open(stage.output.c_str(), O_WRONLY | O_CREAT | (stage.append ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) { fail("sh", stage.output); std::exit(1); }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    signal(SIGINT, SIG_DFL);

    if (const Command* command = find_command(stage.words[0])) {
        const int status = command->run(stage.words);
        std::fflush(nullptr);
        std::exit(status);
    }

    std::vector<char*> argv;
    for (const std::string& word : stage.words) argv.push_back(const_cast<char*>(word.c_str()));
    argv.push_back(nullptr);
    const std::string path = stage.words[0].find('/') != std::string::npos ? stage.words[0] : "/bin/" + stage.words[0];
    execve(path.c_str(), argv.data(), environ);
    std::fprintf(stderr, "sh: %s: %s\n", stage.words[0].c_str(), errno == ENOENT ? "command not found" : std::strerror(errno));
    std::exit(127);
}

int run_pipeline(const Pipeline& pipeline) {
    int status = 0;
    if (pipeline.stages.size() == 1 && pipeline.stages[0].input.empty() && pipeline.stages[0].output.empty() &&
        run_in_shell(pipeline.stages[0].words, status))
        return status;

    std::fflush(nullptr);   // or each child would inherit, and print, what is still buffered
    g_foreground.clear();
    int previous_read = -1;
    for (size_t i = 0; i < pipeline.stages.size(); i++) {
        int link[2] = { -1, -1 };
        const bool last = i + 1 == pipeline.stages.size();
        if (!last && pipe(link) != 0) { fail("sh", "pipe"); break; }

        const pid_t pid = fork();
        if (pid < 0) { fail("sh", "fork"); break; }
        if (pid == 0) {
            if (previous_read >= 0) { dup2(previous_read, STDIN_FILENO); close(previous_read); }
            if (!last) { dup2(link[1], STDOUT_FILENO); close(link[0]); close(link[1]); }
            run_stage(pipeline.stages[i]);
        }
        g_foreground.push_back(pid);
        if (previous_read >= 0) close(previous_read);
        if (!last) close(link[1]);
        previous_read = last ? -1 : link[0];
    }
    if (previous_read >= 0) close(previous_read);

    // The pipeline's status is its last command's.
    const std::vector<pid_t> children = g_foreground;
    for (size_t i = 0; i < children.size(); i++) {
        int child_status = 0;
        while (waitpid(children[i], &child_status, 0) < 0 && errno == EINTR) {}
        if (i + 1 == children.size()) {
            if (WIFEXITED(child_status)) status = WEXITSTATUS(child_status);
            else {
                status = 128 + WTERMSIG(child_status);
                if (WTERMSIG(child_status) != SIGINT) std::fprintf(stderr, "sh: killed by signal %d\n", WTERMSIG(child_status));
            }
        }
    }
    g_foreground.clear();
    return status;
}

// A line is run one pipeline at a time, each parsed only when its turn
// comes: "false; echo $?" must see the status of the command before it.
void run_line(const std::string& line) {
    size_t start = 0;
    bool conditional = false;
    char quote = 0;
    for (size_t i = 0; i <= line.size(); i++) {
        const char c = i < line.size() ? line[i] : ';';
        if (i < line.size()) {
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == '\'' || c == '"') { quote = c; continue; }
            if (c == '\\') { i++; continue; }
            if (c == '#') { i = line.size() - 1; continue; }   // the rest is a comment; end the segment after it
        }
        const bool and_then = c == '&' && i + 1 < line.size() && line[i + 1] == '&';
        if (c != ';' && !and_then) continue;

        const std::string segment = line.substr(start, i - start);
        if (!(conditional && g_last_status != 0)) {
            std::vector<Pipeline> pipelines;
            if (!parse(segment, pipelines)) { g_last_status = 2; return; }
            for (const Pipeline& pipeline : pipelines) g_last_status = run_pipeline(pipeline);
        }
        conditional = and_then;
        if (and_then) i++;
        start = i + 1;
    }
}

// Ctrl+C is for whatever is running, not for the shell: pass it on.
void on_interrupt(int) {
    for (const pid_t pid : g_foreground) kill(pid, SIGINT);
}

void prompt() {
    char cwd[512];
    std::printf("sirius:%s$ ", getcwd(cwd, sizeof(cwd)) ? cwd : "?");
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    bool interactive = isatty(STDIN_FILENO);
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-i") == 0) interactive = true;
        else if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) { run_line(argv[i + 1]); return g_last_status; }
        else { std::fprintf(stderr, "usage: sh [-i] [-c COMMAND]\n"); return 2; }
    }

    struct sigaction action {};
    action.sa_handler = on_interrupt;
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, nullptr);
    if (std::getenv("PATH") == nullptr) setenv("PATH", "/bin", 1);
    // Unbuffered: whatever the shell had read ahead, every child it forks
    // would inherit a copy of -- and a command reading its standard input
    // would find the rest of the script there.
    setvbuf(stdin, nullptr, _IONBF, 0);

    if (interactive) std::puts("Sirius OS shell. Type \"help\" for the commands.");
    std::string line;
    for (;;) {
        if (interactive) prompt();
        line.clear();
        int c;
        while ((c = std::fgetc(stdin)) != EOF && c != '\n') line += static_cast<char>(c);
        if (c == EOF && line.empty()) break;
        run_line(line);
    }
    if (interactive) std::puts("");
    return g_last_status;
}
