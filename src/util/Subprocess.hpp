#pragma once
// TCNG: runs a child process to completion, feeding it stdin and collecting stdout/stderr
// (used by PythonTidalFetchTask). posix_spawn on Linux/macOS, CreateProcessW on Windows.
// One run() per object; cancel() may be called from any thread and kills the child.

#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tcng::util {

class Subprocess {
public:
    struct Options {
        std::string program;                     // a path, or a bare name looked up on PATH
        std::vector<std::string> args;           // argv[1..]
        std::map<std::string, std::string> env;  // added to / overriding the inherited environment
        std::string input;                       // written to stdin, which is then closed
        std::chrono::milliseconds timeout{30000};
    };

    struct Result {
        bool started = false;    // false: `error` says why the child could not be started
        int exitCode = -1;       // -1 if killed (timeout, cancel, signal)
        bool timedOut = false;
        bool cancelled = false;
        std::string out;
        std::string err;
        std::string error;
    };

    Result run(const Options& options);
    void cancel() { cancelled_ = true; }

    // Runs `program args...` with this process's stdin, stdout and stderr inherited (for an
    // interactive child, e.g. a login that prints a link and reads the reply) and waits for
    // it. Returns the exit code, or -1 with `error` set when it could not be started.
    static int runInteractive(const std::string& program, const std::vector<std::string>& args,
                              const std::map<std::string, std::string>& env, std::string& error);

    // `name` resolved against PATH (and PATHEXT on Windows); nullopt if not found. A name
    // with a directory separator is returned as is when it exists.
    static std::optional<std::string> findExecutable(const std::string& name);
    // PATH separator: ':' on POSIX, ';' on Windows.
    static char pathListSeparator();
    // One argument quoted for a Windows command line (CommandLineToArgvW rules).
    static std::string quoteWindowsArg(const std::string& arg);

private:
    std::atomic<bool> cancelled_{false};
};

}  // namespace tcng::util
