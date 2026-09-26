// Copies stdin to stdout slowly: small reads with a pause after every few, so
// a producer writing into the pipe repeatedly fills it and blocks. Used by the
// stress suite to check output integrity under backpressure.
//
// Usage: producer | slow_reader [chunk_bytes] [pause_every_n_reads] [pause_us]
// @spec DLV-STRESS-005
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <vector>

int main(int argc, char** argv) {
    const long chunk = argc > 1 ? std::atol(argv[1]) : 4096;
    const long every = argc > 2 ? std::atol(argv[2]) : 16;
    const long pause_us = argc > 3 ? std::atol(argv[3]) : 1000;
    std::vector<char> buffer(static_cast<std::size_t>(chunk > 0 ? chunk : 4096));

    for (long reads = 1;; ++reads) {
        const ssize_t n = read(0, buffer.data(), buffer.size());
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        for (ssize_t done = 0; done < n;) {
            const ssize_t w = write(1, buffer.data() + done, static_cast<std::size_t>(n - done));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return 1;
            done += w;
        }
        if (every > 0 && reads % every == 0) {
            const timespec pause{0, pause_us * 1000};
            nanosleep(&pause, nullptr);
        }
    }
}
