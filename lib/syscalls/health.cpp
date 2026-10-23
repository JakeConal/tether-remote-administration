// lib/syscalls/health.cpp - the health sampler engine (add-health-telemetry,
// tasks 2.1/2.2). Pure interval/aggregate math plus the numeric-only
// snapshot JSON builder; see the header for the contracts.
#include "health.hpp"

#include "errors.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace tether::syscalls {
namespace {

// All snapshot numbers are integers or 0..100 percentages; the JSON has no
// strings, so no escaping is needed anywhere. Stream rendering keeps the
// documented single-line shape byte-reproducible.
void append_double(std::string& out, double value) {
    std::ostringstream rendered;
    rendered << value;
    out += rendered.str();
}

} // namespace

HealthState::HealthState(int interval_s, int window_s, HealthMillis started_at)
    : interval_s_(interval_s < 1 ? 1 : interval_s),
      window_s_(window_s < interval_s_ ? interval_s_ : window_s),
      started_at_(started_at) {}

void HealthState::note_established(HealthMillis /*now*/) {
    if (established_) {
        ++reconnects_;
    } else {
        established_ = true;
    }
}

std::size_t HealthState::expected_ticks() const {
    const std::size_t expected =
        static_cast<std::size_t>(window_s_ / interval_s_);
    return expected == 0 ? 1 : expected;
}

void HealthState::tick(HealthMillis now, const HostReading& reading) {
    Sample sample;
    sample.wall_ms = now;
    sample.memory_used = reading.memory_used;
    sample.memory_total = reading.memory_total;
    sample.processes = reading.processes;
    sample.threads = reading.threads;

    if (!has_counter_base_) {
        has_counter_base_ = true; // baseline-only: no CPU value, no fabrication
    } else {
        const std::uint64_t busy = reading.cpu_busy;
        const std::uint64_t total = reading.cpu_total;
        if (total > base_total_ && busy >= base_busy_) {
            const double fraction = static_cast<double>(busy - base_busy_) /
                                    static_cast<double>(total - base_total_);
            sample.cpu_percent = std::clamp(fraction * 100.0, 0.0, 100.0);
        } else {
            // Counters went backwards or stalled: original progress breaks,
            // then the base counters below are refreshed from this reading;
            // the NEXT tick diffs against the new baseline (the sample here
            // stays CPU-less rather than dividing by nonsense).
        }
    }
    base_busy_ = reading.cpu_busy;
    base_total_ = reading.cpu_total;

    window_.push_back(std::move(sample));
    // Tick-indexed window: keep the newest `expected_ticks()` samples, so a
    // full window always spans the configured cadence (~span seconds) and a
    // delayed sampler simply holds fewer real ticks (honest coverage).
    while (window_.size() > expected_ticks()) {
        window_.pop_front();
    }
}

HealthSnapshot HealthState::snapshot(HealthMillis now) const {
    HealthSnapshot snap;
    snap.interval_s = interval_s_;
    snap.span_seconds = window_s_;
    snap.uptime_seconds =
        now > started_at_
            ? static_cast<std::uint64_t>((now - started_at_) / 1000)
            : 0;
    snap.reconnects = reconnects_;
    snap.ticks = window_.size();

    // Aggregates over the stored span; coverage compares against the
    // EXPECTED tick count, never a wall-clock extrapolation.
    double cpu_sum = 0.0;
    std::size_t cpu_ticks = 0;
    std::uint64_t memory_used_max = 0;
    for (const Sample& sample : window_) {
        if (sample.cpu_percent.has_value()) {
            ++cpu_ticks;
            cpu_sum += *sample.cpu_percent;
            if (!snap.cpu_percent_max.has_value() || *sample.cpu_percent > *snap.cpu_percent_max) {
                snap.cpu_percent_max = *sample.cpu_percent;
            }
        }
        if (sample.memory_used > memory_used_max) {
            memory_used_max = sample.memory_used;
        }
    }
    if (cpu_ticks > 0) {
        snap.cpu_percent_avg = cpu_sum / static_cast<double>(cpu_ticks);
    }
    if (memory_used_max > 0) {
        snap.memory_used_max = memory_used_max;
    }
    const double expected = static_cast<double>(expected_ticks());
    snap.window_coverage = std::min(1.0, static_cast<double>(window_.size()) / expected);

    if (!window_.empty()) {
        const Sample& latest = window_.back();
        snap.has_latest = true;
        snap.cpu_percent = latest.cpu_percent;
        snap.memory_used = latest.memory_used;
        snap.memory_total = latest.memory_total;
        snap.processes = latest.processes;
        snap.threads = latest.threads;
        snap.sampled_at_ms = latest.wall_ms;
        snap.sample_age_ms = now > latest.wall_ms ? now - latest.wall_ms : 0;
    }
    return snap;
}

std::string HealthSnapshot::json() const {
    std::string out;
    out += "{\"uptime_seconds\":";
    out += std::to_string(uptime_seconds);
    out += ",\"reconnects\":";
    out += std::to_string(reconnects);
    if (has_latest) {
        out += ",\"latest\":{\"cpu_percent\":";
        if (cpu_percent.has_value()) {
            append_double(out, *cpu_percent);
            out += ",\"memory_used\":";
        } else {
            // Baseline-only latest (reachable when a command lands before
            // the first post-baseline tick): the explicit null is the
            // honest "not measured yet" signal, never a fabricated 0%.
            out += "null,\"memory_used\":";
        }
        out += std::to_string(memory_used);
        out += ",\"memory_total\":";
        out += std::to_string(memory_total);
        out += ",\"processes\":";
        out += std::to_string(processes);
        out += ",\"threads\":";
        out += std::to_string(threads);
        out += ",\"sampled_at_ms\":";
        out += std::to_string(sampled_at_ms);
        out += "}";
    }
    out += ",\"window\":{\"span_seconds\":";
    out += std::to_string(span_seconds);
    out += ",\"ticks\":";
    out += std::to_string(ticks);
    if (cpu_percent_avg.has_value()) {
        out += ",\"cpu_percent_avg\":";
        append_double(out, *cpu_percent_avg);
        out += ",\"cpu_percent_max\":";
        append_double(out, cpu_percent_max.value_or(0.0));
    }
    if (memory_used_max.has_value()) {
        out += ",\"memory_used_max\":";
        out += std::to_string(*memory_used_max);
    }
    out += ",\"window_coverage\":";
    append_double(out, window_coverage);
    out += "},\"sample_age_ms\":";
    out += std::to_string(sample_age_ms);
    out += ",\"interval_s\":";
    out += std::to_string(interval_s);
    out += "}";
    return out;
}

} // namespace tether::syscalls

// ---- Per-OS host probes (phase 3, tasks 3.1/3.2) ----------------------------
//
// One file holds both seams, the same pattern as capture.cpp/input.cpp. The
// probe is deliberately OUTSIDE the pure engine above: HealthState stays
// fake-injectable, and the smoke tests exercise the real platform reads per
// OS build.

#if defined(_WIN32)

#include <windows.h>
#include <tlhelp32.h>

namespace tether::syscalls {
namespace {

std::uint64_t filetime_to_ulonglong(const FILETIME& ft) {
    ULARGE_INTEGER cast{};
    cast.LowPart = ft.dwLowDateTime;
    cast.HighPart = ft.dwHighDateTime;
    return cast.QuadPart;
}

std::uint32_t count_toolhelp_entries(DWORD snapshot_flags) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(snapshot_flags, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        throw SyscallError("sample_unavailable",
                           "host snapshot failed (GetLastError()=" +
                               std::to_string(GetLastError()) + ")");
    }
    std::uint32_t count = 0;
    if (snapshot_flags == TH32CS_SNAPPROCESS) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ProcessID != 0) { // pid 0 == the idle anchor, matching `list`
                    ++count;
                }
            } while (Process32NextW(snapshot, &entry));
        }
    } else {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry)) {
            do {
                ++count;
            } while (Thread32Next(snapshot, &entry));
        }
    }
    CloseHandle(snapshot);
    return count;
}

} // namespace

class WindowsHealthHostProbe final : public IHealthHostProbe {
public:
    HostReading read() override {
        HostReading r;

        // Cumulative processor times (FILETIME 100 ns units; counter
        // granularity is HealthState's diff concern). Kernel time INCLUDES
        // idle time, so busy = kernel + user - idle.
        FILETIME idle{}, kernel{}, user{};
        if (!GetSystemTimes(&idle, &kernel, &user)) {
            throw SyscallError("sample_unavailable",
                               "GetSystemTimes failed (GetLastError()=" +
                                   std::to_string(GetLastError()) + ")");
        }
        const std::uint64_t idle_time = filetime_to_ulonglong(idle);
        const std::uint64_t kernel_time = filetime_to_ulonglong(kernel);
        const std::uint64_t user_time = filetime_to_ulonglong(user);
        const std::uint64_t total_time = kernel_time + user_time;
        if (total_time < idle_time) {
            throw SyscallError("sample_unavailable", "GetSystemTimes counters inverted");
        }
        r.cpu_total = total_time;
        r.cpu_busy = total_time - idle_time;

        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (!GlobalMemoryStatusEx(&memory)) {
            throw SyscallError("sample_unavailable",
                               "GlobalMemoryStatusEx failed (GetLastError()=" +
                                   std::to_string(GetLastError()) + ")");
        }
        r.memory_total = memory.ullTotalPhys;
        r.memory_used = memory.ullTotalPhys - memory.ullAvailPhys;

        r.processes = count_toolhelp_entries(TH32CS_SNAPPROCESS);
        r.threads = count_toolhelp_entries(TH32CS_SNAPTHREAD);
        return r;
    }
};

std::unique_ptr<IHealthHostProbe> make_health_host_probe() {
    return std::make_unique<WindowsHealthHostProbe>();
}

} // namespace tether::syscalls

#elif defined(__linux__)

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace tether::syscalls {
namespace {

// Exception-free non-negative integer parsing (strtoull, end-pointer check),
// so malformed /proc content can only fail loudly into SyscallError - never
// into an unclassified std exception walking past the dispatcher.
bool token_u64(const char* begin, std::uint64_t& out) {
    if (begin == nullptr) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(begin, &end, 10);
    if (end == begin) {
        return false;
    }
    out = static_cast<std::uint64_t>(value);
    return true;
}

// Finds the first digit run at-or-after `search_from`, parses it, and returns
// the index one PAST the number's last digit (npos when absent). Returning
// the end position is what keeps field indexing correct across /proc/stat's
// variable-width, possibly multi-space separated columns.
std::size_t parse_digit_token(const std::string& text, std::size_t search_from,
                              std::uint64_t& out) {
    const std::size_t digit = text.find_first_of("0123456789", search_from);
    if (digit == std::string::npos) {
        return std::string::npos;
    }
    const char* begin = text.c_str() + digit;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(begin, &end, 10);
    if (end == begin) {
        return std::string::npos;
    }
    out = static_cast<std::uint64_t>(value);
    return static_cast<std::size_t>(end - text.c_str());
}

// /proc/stat's first line: "cpu  <user> <nice> <system> <idle> <iowait>
// <irq> <softirq> <steal> <guest> <guest_nice>" in jiffies. Guest time is
// already accounted inside user/nice on modern kernels, so it is EXCLUDED
// from the total to avoid double counting; busy = total - idle_all.
void parse_proc_stat(std::uint64_t& busy, std::uint64_t& total) {
    std::ifstream stat("/proc/stat");
    std::string line;
    if (!stat || !std::getline(stat, line) || line.rfind("cpu ", 0) != 0) {
        throw SyscallError("sample_unavailable", "/proc/stat cpu line unreadable");
    }
    std::uint64_t fields[8] = {}; // user, nice, system, idle, iowait, irq, softirq, steal
    std::size_t scan = 4;         // skip the "cpu " prefix
    std::size_t present = 0;
    while (present < 8 && scan != std::string::npos) {
        scan = parse_digit_token(line, scan, fields[present]);
        if (scan != std::string::npos) {
            ++present;
        }
    }
    if (present < 4) {
        throw SyscallError("sample_unavailable", "/proc/stat cpu line malformed");
    }
    const std::uint64_t idle_all =
        fields[3] + (present > 4 ? fields[4] : 0); // idle + iowait (may be absent on ancient kernels)
    total = 0;
    for (std::size_t index = 0; index < present; ++index) {
        total += fields[index];
    }
    if (total == 0) {
        throw SyscallError("sample_unavailable", "/proc/stat pulse is empty");
    }
    busy = total > idle_all ? total - idle_all : 0;
}

std::uint64_t parse_meminfo_kb(const char* key) {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo) {
        throw SyscallError("sample_unavailable", "/proc/meminfo unreadable");
    }
    const std::string needle = key; // e.g. "MemTotal:"
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.compare(0, needle.size(), needle) == 0) {
            std::uint64_t kb = 0;
            if (parse_digit_token(line, needle.size(), kb) != std::string::npos) {
                return kb;
            }
        }
    }
    return 0;
}

} // namespace

class LinuxHealthHostProbe final : public IHealthHostProbe {
public:
    HostReading read() override {
        HostReading r;
        parse_proc_stat(r.cpu_busy, r.cpu_total);

        // kB columns scaled to bytes; used = total - available (available is
        // reclaimable-including; kernels without MemAvailable fall back to
        // MemFree, a documented degradation, not a fabrication).
        const std::uint64_t total_kb = parse_meminfo_kb("MemTotal:");
        std::uint64_t available_kb = parse_meminfo_kb("MemAvailable:");
        if (available_kb == 0) {
            available_kb = parse_meminfo_kb("MemFree:");
        }
        if (total_kb == 0) {
            throw SyscallError("sample_unavailable", "/proc/meminfo totals empty");
        }
        r.memory_total = total_kb * 1024;
        r.memory_used = total_kb > available_kb ? (total_kb - available_kb) * 1024 : 0;

        // /proc/loadavg's 4th field: "running/total" system-wide thread
        // count (cheap; summing per-process Threads_ rows would walk every
        // PID every tick for the same number).
        std::ifstream loadavg("/proc/loadavg");
        std::string line;
        if (!loadavg || !std::getline(loadavg, line)) {
            throw SyscallError("sample_unavailable", "/proc/loadavg unreadable");
        }
        const std::size_t slash = line.find('/');
        if (slash == std::string::npos) {
            throw SyscallError("sample_unavailable", "/proc/loadavg malformed");
        }
        std::uint64_t threads_total = 0;
        if (!token_u64(line.c_str() + slash + 1, threads_total)) {
            throw SyscallError("sample_unavailable", "/proc/loadavg thread count malformed");
        }
        r.threads = static_cast<std::uint32_t>(threads_total);

        // Processes: every pid-numeric directory of /proc (the idle anchor
        // has no directory there; there is nothing to exclude).
        std::uint32_t count = 0;
        std::error_code walk_error;
        std::filesystem::directory_iterator it("/proc", std::filesystem::directory_options::none,
                                               walk_error);
        if (walk_error) {
            throw SyscallError("sample_unavailable", "/proc walk failed: " + walk_error.message());
        }
        for (const std::filesystem::directory_entry& entry : it) {
            const std::string name = entry.path().filename().string();
            if (!name.empty() && name.find_first_not_of("0123456789") == std::string::npos) {
                ++count;
            }
        }
        r.processes = count;
        return r;
    }
};

std::unique_ptr<IHealthHostProbe> make_health_host_probe() {
    return std::make_unique<LinuxHealthHostProbe>();
}

} // namespace tether::syscalls

#else

namespace tether::syscalls {

class UnsupportedHealthHostProbe final : public IHealthHostProbe {
public:
    HostReading read() override {
        throw SyscallError("unsupported_platform",
                           "host health sampling is not implemented on this OS");
    }
};

std::unique_ptr<IHealthHostProbe> make_health_host_probe() {
    return std::make_unique<UnsupportedHealthHostProbe>();
}

} // namespace tether::syscalls

#endif
