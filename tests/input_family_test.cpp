// tests/input_family_test.cpp - task 6.7: the input family through the
// dispatcher with every platform seam INJECTED. No test installs a real
// low-level hook, touches X, or locks a desktop: the four factory-facing
// seams (environment probe, key-event source, capture sink, keyboard guard)
// carry fakes, so the lifecycle, refusals and structured outcomes are
// exercised without any system-wide effect.
//
// Scenarios (remote-commands delta + task text):
//  - the keylogger runs as a managed background job on the SHARED registry
//    slot "input": session stays usable while it runs; duplicate start is
//    the structured already_running refusal; stop delivers the accumulated
//    capture (the family lifecycle tests apply to the keylogger);
//  - configurable capture storage: the start parameter reaches the sink
//    factory and the default rides kDefaultCaptureStorage;
//  - "unsupported resolution never wedges the session": Wayland-class
//    environments refuse start/lock with the structured unsupported result
//    through the same session object, and supported commands keep working;
//  - the keyboard lock is family state with the structured
//    already_running / not_running refusals, engaging and releasing through
//    the injected guard only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "input.hpp"
#include "jobs.hpp"
#include "matrix.hpp"

#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tsys = tether::syscalls;
namespace fs = std::filesystem;

namespace {

// ---- the injected seams ---------------------------------------------------------

class FakeProbe final : public tsys::IInputEnvironmentProbe {
public:
    explicit FakeProbe(tsys::InputEnvironment environment)
        : environment_(std::move(environment)) {}
    tsys::InputEnvironment probe() override { return environment_; }

    void set_environment(tsys::InputEnvironment environment) { environment_ = environment; }

private:
    tsys::InputEnvironment environment_;
};

class FakeGuard final : public tsys::IKeyboardGuard {
public:
    void engage() override {
        if (refusal_ == "access_denied") {
            throw tsys::SyscallError("access_denied",
                                     "the fake guard refused the engage (as BlockInput can)");
        }
        if (refusal_ == "execute_failed") {
            throw tsys::SyscallError("execute_failed",
                                     "the fake guard failed the engage (as XGrabKeyboard can)");
        }
        ++engaged_count_;
    }

    void release() override { ++released_count_; }

    void set_refusal(const std::string& refusal) { refusal_ = refusal; }

    int engaged_count() const { return engaged_count_.load(); }
    int released_count() const { return released_count_.load(); }

private:
    std::atomic<int> engaged_count_{0};
    std::atomic<int> released_count_{0};
    std::string refusal_;
};

// The event source: a test-thread producer feeds lines the worker poll()s.
// The push/pop races with the worker thread, so the hand-off is mutex-ed.
class FakeSource final : public tsys::IKeyEventSource {
public:
    bool open() override {
        if (open_refusal_) {
            return false; // the platform refused the engine (start-then-fail)
        }
        opened_ = true;
        return true;
    }

    std::vector<std::string> poll() override {
        std::lock_guard<std::mutex> guard(mutex_);
        std::vector<std::string> batch;
        batch.swap(pending_);
        return batch;
    }

    void close() override {
        std::lock_guard<std::mutex> guard(mutex_);
        closed_ = true;
    }

    bool opened() const { return opened_.load(); }
    bool closed() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return closed_;
    }

    void emit(const std::string& line) {
        std::lock_guard<std::mutex> guard(mutex_);
        pending_.push_back(line);
    }

    void refuse_open() { open_refusal_ = true; }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> pending_;
    std::atomic<bool> opened_{false};
    std::atomic<bool> open_refusal_{false};
    bool closed_ = false;
};

// The capture sink: accumulates like the production file sink but in memory;
// one instance per start records the storage it was built for.
class FakeSink final : public tsys::IKeyCaptureSink {
public:
    FakeSink(std::string storage, std::function<void(FakeSink&)> on_built)
        : storage_(std::move(storage)), on_built_(std::move(on_built)) {
        on_built_(*this);
    }

    void append(const std::string& line) override {
        std::lock_guard<std::mutex> guard(mutex_);
        content_.push_back(line + '\n');
    }

    tsys::JobDeliverable take_result() override {
        std::lock_guard<std::mutex> guard(mutex_);
        tsys::JobDeliverable deliverable;
        deliverable.ready = ready_;
        deliverable.name = base_name();
        std::size_t total = 0;
        for (const std::string& piece : content_) {
            total += piece.size();
        }
        deliverable.bytes.reserve(total);
        for (const std::string& piece : content_) {
            deliverable.bytes.insert(deliverable.bytes.end(), piece.begin(), piece.end());
        }
        return deliverable;
    }

    void close() override { closed_ = true; }

    const std::string& storage() const { return storage_; }
    std::string delivered_name() const { return base_name(); }

    void make_undeliverable() { ready_ = false; }

private:
    std::string base_name() const {
        const std::size_t slash = storage_.find_last_of("/\\");
        return slash == std::string::npos ? storage_ : storage_.substr(slash + 1);
    }

    std::string storage_;
    std::function<void(FakeSink&)> on_built_;
    std::mutex mutex_;
    std::vector<std::string> content_;
    bool ready_ = true;
    bool closed_ = false;
};

// The test's per-case state: one registry, one of each seam. The registry
// is declared FIRST so its destructor (which joins running jobs) dies last.
struct Harness {
    std::shared_ptr<tsys::BackgroundJobRegistry> registry =
        std::make_shared<tsys::BackgroundJobRegistry>();
    std::shared_ptr<FakeProbe> probe = std::make_shared<FakeProbe>(
        tsys::InputEnvironment{true, false, "injected capable environment"});
    std::shared_ptr<FakeSource> source = std::make_shared<FakeSource>();
    std::vector<std::shared_ptr<FakeSink>> built_sinks;
    std::shared_ptr<FakeGuard> guard = std::make_shared<FakeGuard>();

    std::shared_ptr<tsys::InputControl> control() {
        tsys::InputControl::Bindings bindings;
        bindings.probe = [this]() -> std::shared_ptr<tsys::IInputEnvironmentProbe> { return probe; };
        bindings.source = [this]() -> std::shared_ptr<tsys::IKeyEventSource> { return source; };
        bindings.sink = [this](const std::string& storage) -> std::shared_ptr<tsys::IKeyCaptureSink> {
            // The production FileCaptureSink opens the storage eagerly and
            // refuses with the classified access_denied when it cannot; the
            // fake mirrors exactly that contract so the pre-seat refusal is
            // exercised through the family code (which never seats the job
            // for an unopenable storage).
            {
                std::ofstream probe(storage, std::ios::binary | std::ios::app);
                if (!probe.is_open()) {
                    throw tsys::SyscallError("access_denied",
                                             "cannot open the capture storage '" + storage +
                                                 "' for writing; start_keylogger was refused");
                }
            }
            auto sink =
                std::make_shared<FakeSink>(storage, [](FakeSink&) {});
            built_sinks.push_back(sink);
            return sink;
        };
        bindings.guard = [this]() -> std::shared_ptr<tsys::IKeyboardGuard> { return guard; };
        return std::make_shared<tsys::InputControl>(std::move(bindings), registry, [] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
    }
};

fs::path fresh_scratch_dir() {
    static int counter = 0;
    const fs::path dir =
        fs::temp_directory_path() / ("tether-input-tests-" + std::to_string(counter++));
    fs::create_directories(dir);
    return dir;
}

std::string as_text(const std::vector<unsigned char>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

bool contains_line(const std::vector<std::string>& lines, const std::string& needle) {
    for (const std::string& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("a started keylogger runs as a managed job on the shared registry input slot") {
    Harness harness;
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_input_control(harness.control());

    const tsys::Outcome started = dispatcher.dispatch("start_keylogger", {});
    REQUIRE(started.ok);
    CHECK(contains_line(started.lines, "keylogger job started"));
    CHECK(contains_line(started.lines,
                        "{\"tether_job_event\":{\"job\":\"input\",\"event\":\"started\"}}"));
    CHECK(harness.registry->running(tsys::BackgroundJobRegistry::kInputSlot));
    CHECK(harness.built_sinks.front()->storage() == tsys::kDefaultCaptureStorage);

    // While the job runs, the SAME session still serves commands.
    const tsys::Outcome served = dispatcher.dispatch("stop_keylogger", {});
    REQUIRE(served.ok);
    CHECK(!harness.registry->running(tsys::BackgroundJobRegistry::kInputSlot));
}

TEST_CASE("duplicate start is the structured already_running refusal") {
    Harness harness;
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_input_control(harness.control());

    REQUIRE(dispatcher.dispatch("start_keylogger", {}).ok);
    const std::size_t sinks_after_first = harness.built_sinks.size();

    // The duplicate is refused after the pre-seat sink build. The storage
    // path is plain data here: a RELATIVE one works on every platform
    // (an absolute spelling is meant for the unopenable-path scenario).
    const tsys::Outcome duplicate = dispatcher.dispatch("start_keylogger", {"other.log"});
    REQUIRE(!duplicate.ok);
    CHECK(duplicate.code == "already_running");
    CHECK(harness.registry->running(tsys::BackgroundJobRegistry::kInputSlot));
    // The seat count did not grow: the running incumbent was untouched.
    CHECK(harness.built_sinks.size() == sinks_after_first + 1); // the refused start's sink
    const tsys::Outcome stopped = dispatcher.dispatch("stop_keylogger", {});
    REQUIRE(stopped.ok); // the incumbent stops and delivers normally
}

TEST_CASE("stop delivers the accumulated capture (sink-driven path)") {
    Harness harness;
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_input_control(harness.control());

    REQUIRE(dispatcher.dispatch("start_keylogger", {"keys.log"}).ok);
    harness.source->emit("keydown a");
    harness.source->emit("keydown b");

    const tsys::Outcome stopped = dispatcher.dispatch("stop_keylogger", {});
    REQUIRE(stopped.ok);
    REQUIRE(stopped.file.has_value());
    CHECK(as_text(stopped.file->bytes) == "keydown a\nkeydown b\n");
    CHECK(stopped.file->name == "keys.log");
    CHECK(!harness.registry->running(tsys::BackgroundJobRegistry::kInputSlot));

    // Stop repeats refuse with the structured not-running code.
    const tsys::Outcome again = dispatcher.dispatch("stop_keylogger", {});
    CHECK(!again.ok);
    CHECK(again.code == "not_running");
}

TEST_CASE("an empty capture is an honest zero-byte deliverable") {
    Harness harness;
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_input_control(harness.control());

    REQUIRE(dispatcher.dispatch("start_keylogger", {"silent.log"}).ok);
    const tsys::Outcome stopped = dispatcher.dispatch("stop_keylogger", {});
    REQUIRE(stopped.ok);
    REQUIRE(stopped.file.has_value());
    CHECK(stopped.file->bytes.empty());
    CHECK(stopped.file->name == "silent.log");
}

TEST_CASE("stop when nothing is running is the structured not_running refusal") {
    Harness harness;
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_input_control(harness.control());
    dispatcher.set_process_control(tsys::make_process_control());

    const tsys::Outcome refused = dispatcher.dispatch("stop_keylogger", {});
    CHECK(!refused.ok);
    CHECK(refused.code == "not_running");
    // And the session keeps serving later commands.
    const tsys::Outcome still_alive = dispatcher.dispatch("list", {});
    CHECK(still_alive.ok);
    CHECK(!still_alive.lines.empty());
}

TEST_CASE("a capture engine that cannot open surfaces execute_failed at stop") {
    Harness harness;
    harness.source->refuse_open();
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_input_control(harness.control());

    const tsys::Outcome started = dispatcher.dispatch("start_keylogger", {"keys.log"});
    REQUIRE(started.ok); // the job seats, then fails (the start-then-fail shape)

    const tsys::Outcome stopped = dispatcher.dispatch("stop_keylogger", {});
    CHECK(!stopped.ok);
    CHECK(stopped.code == "execute_failed");
    CHECK(!harness.registry->running(tsys::BackgroundJobRegistry::kInputSlot));
}

TEST_CASE("an unopenable storage path is refused before any job is seated") {
    Harness harness;
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_input_control(harness.control());

    const tsys::Outcome refused =
        dispatcher.dispatch("start_keylogger", {"/tether-missing-dir/no-keylog.log"});
    CHECK(!refused.ok);
    CHECK(refused.code == "access_denied");
    CHECK(!harness.registry->running(tsys::BackgroundJobRegistry::kInputSlot));
}

TEST_CASE("capture storage is configurable on the start command") {
    Harness harness;
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_input_control(harness.control());

    // No parameter: the documented default storage reaches the sink factory.
    REQUIRE(dispatcher.dispatch("start_keylogger", {}).ok);
    REQUIRE(dispatcher.dispatch("stop_keylogger", {}).ok);
    REQUIRE(!harness.built_sinks.empty());
    CHECK(harness.built_sinks.back()->storage() == tsys::kDefaultCaptureStorage);

    // One parameter: the operator's path.
    REQUIRE(dispatcher.dispatch("start_keylogger", {"custom.log"}).ok);
    REQUIRE(dispatcher.dispatch("stop_keylogger", {}).ok);
    CHECK(harness.built_sinks.back()->storage() == "custom.log");

    // More than one is a malformed parameter shape.
    const tsys::Outcome bad = dispatcher.dispatch("start_keylogger", {"a.log", "b.log"});
    CHECK(!bad.ok);
    CHECK(bad.code == "invalid_args");
}

TEST_CASE("unsupported environments refuse the structured way and never wedge the session") {
    Harness harness;
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_input_control(harness.control());
    dispatcher.set_process_control(tsys::make_process_control());

    // The Wayland-class environment: no capture channel in this build.
    harness.probe->set_environment(
        tsys::InputEnvironment{false, true, "the session reports wayland via XDG_SESSION_TYPE"});

    const tsys::Outcome start = dispatcher.dispatch("start_keylogger", {"keys.log"});
    CHECK(!start.ok);
    CHECK(start.code == "unsupported_platform");
    CHECK(contains_line({start.message}, "start_keylogger"));
    CHECK(!harness.registry->running(tsys::BackgroundJobRegistry::kInputSlot));

    const tsys::Outcome lock = dispatcher.dispatch("lock_keyboard", {});
    CHECK(!lock.ok);
    CHECK(lock.code == "unsupported_platform");

    const tsys::Outcome stop = dispatcher.dispatch("stop_keylogger", {});
    CHECK(!stop.ok);
    CHECK(stop.code == "not_running"); // the nothing-running refusal, not stuck

    // The same session object resolves to success once the environment is
    // capable again: a refused command leaves NOTHING wedged.
    harness.probe->set_environment(
        tsys::InputEnvironment{true, false, "injected capable environment"});
    const tsys::Outcome recovered = dispatcher.dispatch("start_keylogger", {"keys.log"});
    CHECK(recovered.ok);
    CHECK(dispatcher.dispatch("stop_keylogger", {}).ok);
    const tsys::Outcome listing = dispatcher.dispatch("list", {});
    CHECK(listing.ok);
}

TEST_CASE("the keyboard lock is family state with structured refusals") {
    Harness harness;
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_input_control(harness.control());

    const tsys::Outcome locked = dispatcher.dispatch("lock_keyboard", {});
    REQUIRE(locked.ok);
    CHECK(contains_line(locked.lines, "keyboard locked"));
    REQUIRE(harness.guard->engaged_count() == 1);
    const tsys::Outcome duplicate = dispatcher.dispatch("lock_keyboard", {});
    CHECK(!duplicate.ok);
    CHECK(duplicate.code == "already_running");
    CHECK(harness.guard->engaged_count() == 1); // the guard was NOT re-engaged

    const tsys::Outcome unlocked = dispatcher.dispatch("unlock_keyboard", {});
    REQUIRE(unlocked.ok);
    CHECK(contains_line(unlocked.lines, "keyboard unlocked"));
    CHECK(harness.guard->released_count() == 1);

    const tsys::Outcome again = dispatcher.dispatch("unlock_keyboard", {});
    CHECK(!again.ok);
    CHECK(again.code == "not_running");
    CHECK(harness.guard->released_count() == 1);
}

TEST_CASE("a refused lock engagement leaves the family state unengaged") {
    Harness harness;
    harness.guard->set_refusal("access_denied");
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_input_control(harness.control());

    const tsys::Outcome refused = dispatcher.dispatch("lock_keyboard", {});
    CHECK(!refused.ok);
    CHECK(refused.code == "access_denied");
    CHECK(harness.control()->locked() == false);

    // The failure is contained: a later unlock is the not-running refusal,
    // and a working guard engages afterwards on the same session.
    CHECK(dispatcher.dispatch("unlock_keyboard", {}).code == "not_running");
    harness.guard->set_refusal("");
    CHECK(dispatcher.dispatch("lock_keyboard", {}).ok);
}

TEST_CASE("the lock works while a keylogger job runs (the session is not blocked)") {
    Harness harness;
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_input_control(harness.control());

    REQUIRE(dispatcher.dispatch("start_keylogger", {"keys.log"}).ok);
    // Background job running: further commands (including lifecycle ones on
    // a different mechanism) are served normally.
    CHECK(dispatcher.dispatch("lock_keyboard", {}).ok);
    CHECK(dispatcher.dispatch("unlock_keyboard", {}).ok);
    const tsys::Outcome stopped = dispatcher.dispatch("stop_keylogger", {});
    REQUIRE(stopped.ok);
    CHECK(!harness.registry->running(tsys::BackgroundJobRegistry::kInputSlot));
}

TEST_CASE("the file capture sink persists the configured storage across jobs") {
    const fs::path scratch = fresh_scratch_dir();
    const std::string storage = (scratch / "events.txt").string();

    tsys::InputControl::Bindings bindings;
    auto probe = std::make_shared<FakeProbe>(
        tsys::InputEnvironment{true, false, "capable"});
    bindings.probe = [&]() -> std::shared_ptr<tsys::IInputEnvironmentProbe> { return probe; };
    auto source = std::make_shared<FakeSource>();
    bindings.source = [&]() -> std::shared_ptr<tsys::IKeyEventSource> { return source; };
    bindings.sink = [](const std::string& path) -> std::shared_ptr<tsys::IKeyCaptureSink> {
        return tsys::make_file_capture_sink(path);
    };
    std::shared_ptr<tsys::InputControl> control = std::make_shared<tsys::InputControl>(
        std::move(bindings), std::make_shared<tsys::BackgroundJobRegistry>(),
        [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });

    tsys::CommandDispatcher dispatcher;
    dispatcher.set_input_control(control);

    // First job: two events land in the file, delivered under its basename.
    REQUIRE(dispatcher.dispatch("start_keylogger", {storage}).ok);
    source->emit("keydown p");
    source->emit("keydown q");
    const tsys::Outcome stopped = dispatcher.dispatch("stop_keylogger", {});
    REQUIRE(stopped.ok);
    REQUIRE(stopped.file.has_value());
    CHECK(stopped.file->name == "events.txt");
    CHECK(as_text(stopped.file->bytes) == "keydown p\nkeydown q\n");

    // Second job over the SAME storage: an append, not a truncate (the whole
    // file is the accumulated capture across cycles).
    REQUIRE(dispatcher.dispatch("start_keylogger", {storage}).ok);
    source->emit("keydown r");
    const tsys::Outcome stopped_again = dispatcher.dispatch("stop_keylogger", {});
    REQUIRE(stopped_again.ok);
    REQUIRE(stopped_again.file.has_value());
    CHECK(as_text(stopped_again.file->bytes) == "keydown p\nkeydown q\nkeydown r\n");

    std::error_code ec;
    fs::remove_all(scratch, ec);
}
