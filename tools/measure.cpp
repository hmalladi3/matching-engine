// Runs a command and records its wall time and peak resident memory.
//
// Usage: measure <stats-file> <command> [args...]
//
// stdin, stdout and stderr pass straight through to the command. After it
// exits, "<wall_seconds> <peak_rss_kib>" is written to <stats-file>, and
// measure exits with the command's status (128 + signal if it was killed).
// Uses wait4(), so it behaves the same on Linux and macOS, unlike GNU and BSD
// time(1).
// @spec DLV-STRESS-001, DLV-STRESS-002, DLV-STRESS-004
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: measure <stats-file> <command> [args...]\n");
        return 2;
    }
    const auto start = std::chrono::steady_clock::now();
    const pid_t pid = fork();
    if (pid < 0) return 2;
    if (pid == 0) {
        execvp(argv[2], argv + 2);
        std::perror("measure: exec");
        _exit(127);
    }

    int status = 0;
    rusage usage{};
    if (wait4(pid, &status, 0, &usage) != pid) return 2;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
#if defined(__APPLE__)
    const long rss_kib = usage.ru_maxrss / 1024;  // bytes on macOS
#else
    const long rss_kib = usage.ru_maxrss;  // KiB on Linux
#endif
    if (FILE* out = std::fopen(argv[1], "w")) {
        std::fprintf(out, "%.3f %ld\n", seconds, rss_kib);
        std::fclose(out);
    }
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return WEXITSTATUS(status);
}
