#include "util/Subprocess.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char** environ;
#endif
#endif

namespace tcng::util {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxOutput = 16 * 1024 * 1024;  // per stream; the rest is dropped

std::vector<std::string> splitList(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const auto end = s.find(sep, start);
        const auto item = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!item.empty())
            out.push_back(item);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return out;
}

void appendCapped(std::string& dst, const char* data, std::size_t n) {
    if (dst.size() < kMaxOutput)
        dst.append(data, std::min(n, kMaxOutput - dst.size()));
}

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::filesystem::path fsPath(const std::string& s) {
    return std::filesystem::path(widen(s));
}

std::string getEnv(const char* name) {
    const wchar_t* v = _wgetenv(widen(name).c_str());
    return v ? narrow(v) : std::string();
}

std::string lastErrorMessage(const char* what) {
    return std::string(what) + " failed: " + std::system_category().message(static_cast<int>(GetLastError()));
}

struct CaseInsensitiveLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

// The inherited environment with `overrides` applied, as a CREATE_UNICODE_ENVIRONMENT block.
std::wstring environmentBlock(const std::map<std::string, std::string>& overrides) {
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> vars;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
            const std::wstring entry(p);
            const auto eq = entry.find(L'=', 1);  // "=C:=C:\dir" entries start with '='
            if (eq != std::wstring::npos)
                vars[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
        FreeEnvironmentStringsW(env);
    }
    for (const auto& [k, v] : overrides)
        vars[widen(k)] = widen(v);
    std::wstring block;
    for (const auto& [k, v] : vars) {
        block += k + L"=" + v;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

#else

std::filesystem::path fsPath(const std::string& s) {
    return std::filesystem::path(s);
}

std::string getEnv(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

void setCloexec(int fd) {
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

void setNonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

void closeFd(int& fd) {
    if (fd >= 0)
        ::close(fd);
    fd = -1;
}

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // macOS: SO_NOSIGPIPE on the socket instead
#endif

#endif

bool isExecutableFile(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec))
        return false;
#ifdef _WIN32
    return true;
#else
    return ::access(p.c_str(), X_OK) == 0;
#endif
}

}  // namespace

char Subprocess::pathListSeparator() {
#ifdef _WIN32
    return ';';
#else
    return ':';
#endif
}

std::optional<std::string> Subprocess::findExecutable(const std::string& name) {
    if (name.empty())
        return std::nullopt;
#ifdef _WIN32
    std::vector<std::string> exts{""};
    for (const auto& e : splitList(getEnv("PATHEXT").empty() ? ".COM;.EXE;.BAT;.CMD" : getEnv("PATHEXT"), ';'))
        exts.push_back(e);
    const bool hasDir = name.find_first_of("\\/:") != std::string::npos;
#else
    const std::vector<std::string> exts{""};
    const bool hasDir = name.find('/') != std::string::npos;
#endif
    if (hasDir) {
        for (const auto& ext : exts)
            if (isExecutableFile(fsPath(name + ext)))
                return name + ext;
        return std::nullopt;
    }
    for (const auto& dir : splitList(getEnv("PATH"), pathListSeparator())) {
        for (const auto& ext : exts) {
            const auto candidate = fsPath(dir) / fsPath(name + ext);
            if (isExecutableFile(candidate)) {
#ifdef _WIN32
                return narrow(candidate.wstring());
#else
                return candidate.string();
#endif
            }
        }
    }
    return std::nullopt;
}

std::string Subprocess::quoteWindowsArg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
        return arg;
    std::string out = "\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, '\\');  // they precede the closing quote
            break;
        }
        if (arg[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back('"');
    return out;
}

#ifdef _WIN32

Subprocess::Result Subprocess::run(const Options& o) {
    Result r;
    const auto program = findExecutable(o.program);
    if (!program) {
        r.error = "executable not found: " + o.program;
        return r;
    }
    std::string cmdline = quoteWindowsArg(*program);
    for (const auto& a : o.args)
        cmdline += " " + quoteWindowsArg(a);
    std::wstring wcmd = widen(cmdline);
    std::wstring env = environmentBlock(o.env);

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    auto closeH = [](HANDLE& h) {
        if (h)
            CloseHandle(h);
        h = nullptr;
    };
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
        r.error = lastErrorMessage("CreatePipe");
        for (HANDLE* h : {&inR, &inW, &outR, &outW, &errR, &errW})
            closeH(*h);
        return r;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    // Only the three child ends are inherited, even if other threads spawn concurrently.
    HANDLE inherited[3] = {inR, outW, errW};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inR;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = errW;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    const std::wstring app = widen(*program);
    const BOOL ok = CreateProcessW(app.c_str(), wcmd.data(), nullptr, nullptr, TRUE,
                                   EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                   env.data(), nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    closeH(inR);
    closeH(outW);
    closeH(errW);
    if (!ok) {
        r.error = lastErrorMessage("CreateProcess");
        closeH(inW);
        closeH(outR);
        closeH(errR);
        return r;
    }
    CloseHandle(pi.hThread);
    r.started = true;

    std::thread writer([&] {
        std::size_t done = 0;
        while (done < o.input.size()) {
            DWORD n = 0;
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(o.input.size() - done, 64 * 1024));
            if (!WriteFile(inW, o.input.data() + done, chunk, &n, nullptr))
                break;
            done += n;
        }
        closeH(inW);
    });
    auto reader = [](HANDLE h, std::string& dst) {
        char buf[16 * 1024];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
            appendCapped(dst, buf, n);
    };
    std::thread outReader(reader, outR, std::ref(r.out));
    std::thread errReader(reader, errR, std::ref(r.err));

    const auto deadline = Clock::now() + o.timeout;
    bool killed = false;
    while (WaitForSingleObject(pi.hProcess, 50) == WAIT_TIMEOUT) {
        if (cancelled_ || Clock::now() >= deadline) {
            r.cancelled = cancelled_;
            r.timedOut = !r.cancelled;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
            killed = true;
            break;
        }
    }
    DWORD code = 0;
    if (!killed && GetExitCodeProcess(pi.hProcess, &code))
        r.exitCode = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    writer.join();
    outReader.join();
    errReader.join();
    closeH(outR);
    closeH(errR);
    return r;
}

int Subprocess::runInteractive(const std::string& programName, const std::vector<std::string>& args,
                               const std::map<std::string, std::string>& envOverrides, std::string& error) {
    const auto program = findExecutable(programName);
    if (!program) {
        error = "executable not found: " + programName;
        return -1;
    }
    std::string cmdline = quoteWindowsArg(*program);
    for (const auto& a : args)
        cmdline += " " + quoteWindowsArg(a);
    std::wstring wcmd = widen(cmdline);
    std::wstring env = environmentBlock(envOverrides);
    STARTUPINFOW si{};
    si.cb = sizeof(si);  // no STARTF_USESTDHANDLES: the console's handles are inherited
    PROCESS_INFORMATION pi{};
    const std::wstring app = widen(*program);
    if (!CreateProcessW(app.c_str(), wcmd.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT, env.data(),
                        nullptr, &si, &pi)) {
        error = lastErrorMessage("CreateProcess");
        return -1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    const bool ok = GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return ok ? static_cast<int>(code) : -1;
}

#else

// The inherited environment with `overrides` added or replaced, as "K=V" entries.
static std::vector<std::string> mergedEnvironment(const std::map<std::string, std::string>& overrides) {
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        const std::string entry(*e);
        const auto eq = entry.find('=');
        if (eq == std::string::npos || !overrides.count(entry.substr(0, eq)))
            env.push_back(entry);
    }
    for (const auto& [k, v] : overrides)
        env.push_back(k + "=" + v);
    return env;
}

static void childSignals(posix_spawnattr_t& attr) {
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);  // TCNG ignores SIGPIPE; the child gets the default back
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;  // macOS: inherit nothing but 0/1/2
#endif
    posix_spawnattr_setflags(&attr, flags);
}

int Subprocess::runInteractive(const std::string& programName, const std::vector<std::string>& args,
                               const std::map<std::string, std::string>& envOverrides, std::string& error) {
    const auto program = findExecutable(programName);
    if (!program) {
        error = "executable not found: " + programName;
        return -1;
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    childSignals(attr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
#ifdef __APPLE__
    // POSIX_SPAWN_CLOEXEC_DEFAULT closes everything not named here: keep the terminal.
    for (int fd : {0, 1, 2})
        posix_spawn_file_actions_addinherit_np(&actions, fd);
#endif
    std::vector<std::string> argStore{*program};
    argStore.insert(argStore.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : argStore)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<std::string> envStore = mergedEnvironment(envOverrides);
    std::vector<char*> envp;
    for (auto& e : envStore)
        envp.push_back(e.data());
    envp.push_back(nullptr);
    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, program->c_str(), &actions, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        error = "cannot start " + *program + ": " + std::strerror(rc);
        return -1;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            error = std::string("waitpid: ") + std::strerror(errno);
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

Subprocess::Result Subprocess::run(const Options& o) {
    Result r;
    const auto program = findExecutable(o.program);
    if (!program) {
        r.error = "executable not found: " + o.program;
        return r;
    }
    // stdin is a socketpair so writes can opt out of SIGPIPE when the child exits early.
    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, in) != 0 || ::pipe(out) != 0 || ::pipe(err) != 0) {
        r.error = std::string("pipe failed: ") + std::strerror(errno);
        for (int* fd : {&in[0], &in[1], &out[0], &out[1], &err[0], &err[1]})
            closeFd(*fd);
        return r;
    }
    for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]})
        setCloexec(fd);
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(in[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[1], 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_adddup2(&actions, err[1], 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    childSignals(attr);

    std::vector<std::string> argStore{*program};
    argStore.insert(argStore.end(), o.args.begin(), o.args.end());
    std::vector<char*> argv;
    for (auto& a : argStore)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    std::vector<std::string> envStore = mergedEnvironment(o.env);
    std::vector<char*> envp;
    for (auto& e : envStore)
        envp.push_back(e.data());
    envp.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, program->c_str(), &actions, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    closeFd(in[1]);
    closeFd(out[1]);
    closeFd(err[1]);
    if (rc != 0) {
        r.error = "cannot start " + *program + ": " + std::strerror(rc);
        closeFd(in[0]);
        closeFd(out[0]);
        closeFd(err[0]);
        return r;
    }
    r.started = true;

    int inFd = in[0], outFd = out[0], errFd = err[0];
    for (int fd : {inFd, outFd, errFd})
        setNonblock(fd);
    std::size_t written = 0;
    if (o.input.empty())
        closeFd(inFd);

    const auto deadline = Clock::now() + o.timeout;
    bool killed = false;
    char buf[16 * 1024];
    while (outFd >= 0 || errFd >= 0) {
        if (cancelled_ || Clock::now() >= deadline) {
            r.cancelled = cancelled_;
            r.timedOut = !r.cancelled;
            ::kill(pid, SIGKILL);
            killed = true;
            break;
        }
        pollfd fds[3];
        nfds_t n = 0;
        if (inFd >= 0)
            fds[n++] = {inFd, POLLOUT, 0};
        if (outFd >= 0)
            fds[n++] = {outFd, POLLIN, 0};
        if (errFd >= 0)
            fds[n++] = {errFd, POLLIN, 0};
        if (::poll(fds, n, 50) <= 0)
            continue;
        for (nfds_t i = 0; i < n; ++i) {
            if (!fds[i].revents)
                continue;
            if (fds[i].fd == inFd) {
                const auto chunk = std::min<std::size_t>(o.input.size() - written, 64 * 1024);
                const ssize_t w = ::send(inFd, o.input.data() + written, chunk, MSG_NOSIGNAL);
                if (w > 0)
                    written += static_cast<std::size_t>(w);
                if ((w < 0 && errno != EAGAIN && errno != EINTR) || written == o.input.size())
                    closeFd(inFd);  // done, or the child stopped reading
                continue;
            }
            int& fd = fds[i].fd == outFd ? outFd : errFd;
            std::string& dst = fds[i].fd == outFd ? r.out : r.err;
            const ssize_t got = ::read(fd, buf, sizeof(buf));
            if (got > 0)
                appendCapped(dst, buf, static_cast<std::size_t>(got));
            else if (got == 0 || (errno != EAGAIN && errno != EINTR))
                closeFd(fd);
        }
    }
    closeFd(inFd);
    closeFd(outFd);
    closeFd(errFd);
    // The child may outlive its pipes: keep honouring the deadline and cancel().
    int status = 0;
    for (;;) {
        const pid_t w = ::waitpid(pid, &status, killed ? 0 : WNOHANG);
        if (w == pid || (w < 0 && errno != EINTR))
            break;
        if (w == 0 && (cancelled_ || Clock::now() >= deadline)) {
            r.cancelled = cancelled_;
            r.timedOut = !r.cancelled;
            ::kill(pid, SIGKILL);
            killed = true;
        } else if (w == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    if (!killed && WIFEXITED(status))
        r.exitCode = WEXITSTATUS(status);
    return r;
}

#endif

}  // namespace tcng::util
