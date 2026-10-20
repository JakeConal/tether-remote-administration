// tests/probe_child.cpp - the tiny child process the process-family tests
// launch through the REAL capability layer (task 6.2). It exists so argv can
// be OBSERVED after a launch: the tests assert the launched child received
// its arguments verbatim - including the shell metacharacters the
// parameters-are-data requirement names (; | & >) - which no shell-spliced
// launch could ever guarantee.
//
// Modes (argv[1]):
//   argv <outfile> <arg...>     write one line per remaining arg (plus an
//                               "argv0" header line) to outfile, exit 0.
//   alive <outfile>             touch outfile, then run in a tiny-sleep loop
//                               until killed or a ~20 s self-limit expires
//                               (a stopped suite never leaks a live child).
//
// ASCII-only on purpose: Windows ANSI argv keeps metacharacters intact,
// which is exactly what the assertion needs; no wide/narrow conversion
// noise is added between the launcher and this reader.
#include <chrono>
#include <fstream>
#include <string>
#include <thread>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        return 2;
    }
    const std::string mode = argv[1];
    const std::string outfile = argv[2];

    if (mode == "argv") {
        std::ofstream out(outfile, std::ios::binary | std::ios::trunc);
        if (!out) {
            return 3;
        }
        out << "argv0\n"; // the program name is not asserted, only the data args
        for (int i = 3; i < argc; ++i) {
            out << argv[i] << '\n';
        }
        return 0;
    }
    if (mode == "alive") {
        {
            std::ofstream out(outfile, std::ios::binary | std::ios::trunc);
            if (!out) {
                return 3;
            }
            out << "alive\n";
        }
        // Bounded self-limit: tests stop this mode explicitly, but a failing
        // run must never leave the suite waiting on an immortal child.
        for (int i = 0; i < 200; ++i) { // 200 * 100ms = 20 s
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return 0;
    }
    return 2;
}
