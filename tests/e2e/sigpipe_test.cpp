// Runs the real binary with its stdout pipe already closed: it must exit with
// status 1 and a diagnostic, not die from SIGPIPE (which is what happens by
// default when writing to a pipe with no reader).
#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

namespace {

TEST(RealBinary, ClosedStdoutPipeIsAnErrorNotACrash) {
    int out[2], err[2];
    ASSERT_EQ(pipe(out), 0);
    ASSERT_EQ(pipe(err), 0);
    const int input = open(MATCHER_DATA_DIR "/golden/worked_example.in", O_RDONLY);
    ASSERT_GE(input, 0);
    close(out[0]);  // nobody will ever read stdout

    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        dup2(input, 0);
        dup2(out[1], 1);
        dup2(err[1], 2);
        execl(MATCHER_BINARY, MATCHER_BINARY, static_cast<char*>(nullptr));
        _exit(127);
    }
    close(input);
    close(out[1]);
    close(err[1]);

    std::string diagnostics;
    char buf[256];
    for (ssize_t n; (n = read(err[0], buf, sizeof buf)) > 0;) diagnostics.append(buf, static_cast<std::size_t>(n));
    close(err[0]);

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_FALSE(WIFSIGNALED(status)) << "killed by signal " << WTERMSIG(status);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 1);
    EXPECT_NE(diagnostics.find("stdout write failed: Broken pipe"), std::string::npos) << diagnostics;
}

}  // namespace
