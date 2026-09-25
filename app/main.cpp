#include <csignal>
#include <string_view>
#include <vector>

#include "matcher/app.h"
#include "matcher/io.h"

int main(int argc, char** argv) {
    // A closed stdout pipe must surface as EPIPE, not kill the process.
    // @spec OUT-ERR-004
    std::signal(SIGPIPE, SIG_IGN);

    std::vector<std::string_view> args(argv + 1, argv + argc);
    matcher::FdReader in(0);
    matcher::FdWriter out(1);
    matcher::FdWriter err(2);
    return matcher::run_app(args, in, out, err);
}
