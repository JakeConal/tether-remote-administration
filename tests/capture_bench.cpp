// tests/capture_bench.cpp - task 6.5's benchmark front end (NOT a ctest:
// run manually on a desktop/WSLg session; results land in
// docs/capture-notes.md).
//
// It compares the engineer-authored X11 capture implementation (XOpenDisplay
// + XGetImage + BGR normalize + opencv PNG encode; the production path in
// lib/syscalls/capture.cpp) against shelling to the existing utilities the
// change's design asks about (`scrot`, `xwd`), producing the measured
// numbers recorded in docs/capture-notes.md.
//
// Usage: ./tether-capture-bench [iterations]
#include "capture.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace tsys = tether::syscalls;

using Clock = std::chrono::steady_clock;

template <typename Operation>
std::pair<std::vector<int>, std::string> time_n(int iterations, const std::string& label,
                                                const Operation& operation) {
    std::vector<int> ms;
    ms.reserve(static_cast<std::size_t>(iterations));
    for (int i = 0; i < iterations; ++i) {
        const Clock::time_point start = Clock::now();
        operation();
        ms.push_back(
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 Clock::now() - start)
                                 .count()));
    }
    std::sort(ms.begin(), ms.end());
    const int total = static_cast<int>(ms.size());
    const int min = ms.front();
    const int median = ms[total / 2];
    const int max = ms.back();
    const std::string report =
        label + ": min " + std::to_string(min) + " ms | median " + std::to_string(median) +
        " ms | max " + std::to_string(max) + " ms  (" + std::to_string(total) + " runs)";
    return {ms, report};
}

int run_system(const std::string& command) { return std::system(command.c_str()) == 0 ? 0 : 1; }

} // namespace

int main(int argc, char* argv[]) {
    int iterations = 12;
    if (argc >= 2) {
        iterations = std::max(3, std::atoi(argv[1]));
    }

    std::cout << "capture benchmark: " << iterations << " iterations per candidate\n";

    // Candidate A: the own-library implementation (lib/syscalls/capture.cpp).
    const auto own = tsys::make_screen_capture();
    const std::string own_report = time_n(iterations, "own-lib (X11 XGetImage + opencv png)",
                                          [&own] {
                                              own->grab(); // raises on failure; the bench aborts then
                                          })
                                       .second;
    std::cout << own_report << "\n";

    // Candidate B: shelling to `scrot` (its own PNG encode, writes to disk).
    const std::string scrot_target = "/tmp/tether-bench-scrot.png";
    std::remove(scrot_target.c_str());
    const int scrot_probe = run_system("scrot -o " + scrot_target + " >/dev/null 2>&1");
    if (scrot_probe == 0) {
        std::cout << time_n(iterations, "shell scrot (imlib2 -> png on disk)",
                             [&scrot_target] {
                                 std::remove(scrot_target.c_str());
                                 run_system("scrot -o " + scrot_target + " >/dev/null 2>&1");
                             })
                         .second
              << "\n";
    } else {
        std::cout << "shell scrot: UNAVAILABLE in this environment\n";
    }

    // Candidate C: shelling to `xwd` (raw dump, no PNG encode; the classic
    // pipeline would add xwdtopnm|pnmtopng which needs netpbm - reported
    // separately below when present).
    const std::string xwd_target = "/tmp/tether-bench-root.xwd";
    std::remove(xwd_target.c_str());
    const int xwd_probe =
        run_system("xwd -root -silent -out " + xwd_target + " >/dev/null 2>&1");
    if (xwd_probe == 0) {
        std::cout << time_n(iterations, "shell xwd -root (raw dump, no encode)",
                             [&xwd_target] {
                                 std::remove(xwd_target.c_str());
                                 run_system("xwd -root -silent -out " + xwd_target +
                                            " >/dev/null 2>&1");
                             })
                         .second
              << "\n";
    } else {
        std::cout << "shell xwd: UNAVAILABLE in this environment\n";
    }

    std::cout << "note: the own-lib path hands BYTE-BACKED deliverables in-process (no temp file);\n"
              << "the shell paths write to /tmp and are measured including their disk write.\n";
    return 0;
}
