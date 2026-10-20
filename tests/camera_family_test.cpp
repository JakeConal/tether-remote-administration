// tests/camera_family_test.cpp - task 6.6: the camera family over OpenCV
// with the SHARED managed background-job lifecycle, through the capability
// agent layer. Scenarios map to the remote-commands delta's job requirement
// ("background jobs have a managed lifecycle"):
//
//  - "background job runs without blocking the session": a start_webcam
//    succeeds and FURTHER COMMANDS (including the eventual stop) are served
//    normally WHILE the capture keeps running in the background - proven
//    over a REAL session pair over in-memory wires (no sockets);
//  - "duplicate start is refused with a structured result": the second
//    start_webcam returns the already-running refusal and the incumbent job
//    is untouched (it still delivers its capture at stop);
//  - "job stop releases and delivers the accumulated capture": stop_webcam
//    terminates the job and the accumulated file result arrives as a
//    named, byte-complete, DECODABLE PNG; afterwards the slot is released
//    (start works again) and a stop with nothing running is the structured
//    not_running refusal;
//  - failures surface classified: a device that cannot open / that stops
//    delivering yields execute_failed at stop, never a wedged session and
//    never an unclassified outcome.
//
// ALL capture is FAKE source (test-injected IFrameSource). CI runners have
// no webcams; no test requires a physical device. One probe exercises the
// production cv::VideoCapture path where a device is absent - the
// structured execute_failed is the documented no-device behavior.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "camera.hpp"
#include "command_handler.hpp"
#include "image_png.hpp"
#include "jobs.hpp"
#include "matrix.hpp"
#include "session.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace tp = tether::protocol;
namespace tsys = tether::syscalls;

namespace {

// A deterministic frame: every pixel triple (b,g,r) = ((x*3 + y*7 + p) % 251).
tsys::Frame make_frame(const int width, const int height) {
    tsys::Frame frame;
    frame.width = width;
    frame.height = height;
    frame.bgr.resize(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 3;
            for (int p = 0; p < 3; ++p) {
                frame.bgr[offset + p] = static_cast<unsigned char>((x * 3 + y * 7 + p) % 251);
            }
        }
    }
    return frame;
}

bool frames_equal(const tsys::Frame& a, const tsys::Frame& b) {
    return a.width == b.width && a.height == b.height && a.bgr == b.bgr;
}

// The shared fake: publishes its reads through the probe (test-side gap
// gate); `reads_before_failure` drives the "device dies" variant (0 = never).
struct FakeFrameSource final : tsys::IFrameSource {
    explicit FakeFrameSource(std::atomic<int>& probe, const int reads_before_failure = 0)
        : probe_(probe), reads_before_failure_(reads_before_failure) {}

    bool open() override { return true; }

    bool read(tsys::Frame& out) override {
        const int reads = reads_.fetch_add(1) + 1;
        if (reads_before_failure_ > 0 && reads > reads_before_failure_) {
            probe_.store(-1); // failed past the last good frame
            return false;
        }
        if (reads == 1) {
            probe_.store(1); // the gate: the first read has begun
        }
        out = make_frame(8, 6);
        return true;
    }

    void close() override {}

private:
    std::atomic<int>& probe_;
    int reads_before_failure_;
    std::atomic<int> reads_{0};
};

// A harness camera: production flavour (registry + worker thread + PNG
// encode/publish) over a fake device and a no-real-sleep waiter.
struct TestCamera {
    std::shared_ptr<tsys::BackgroundJobRegistry> registry;
    std::shared_ptr<tsys::VideoCameraCapture> family;

    explicit TestCamera(std::function<std::shared_ptr<tsys::IFrameSource>()> factory)
        : registry(std::make_shared<tsys::BackgroundJobRegistry>()),
          family(std::make_shared<tsys::VideoCameraCapture>(
              std::move(factory), registry, []() { std::this_thread::yield(); })) {}
};

// A source whose open() fails (no device connected / busy / denied).
struct UnopenedCameraSource final : tsys::IFrameSource {
    bool open() override { return false; }
    bool read(tsys::Frame&) override { return false; }
    void close() override {}
};

std::atomic<int> g_probe{0};

tsys::CommandDispatcher dispatcher_with(tsys::Platform platform,
                                        std::shared_ptr<tsys::ICameraCapture> camera) {
    tsys::CommandDispatcher dispatcher(platform);
    dispatcher.set_camera_capture(std::move(camera));
    return dispatcher;
}

// The delivered capture: named + decodable + the exact fake-frame pixels.
tsys::Frame delivered_frame(const tsys::Outcome& outcome) {
    REQUIRE(outcome.ok);
    REQUIRE(outcome.file.has_value());
    CHECK(outcome.file->name == "webcam.png"); // the delivered capture is named
    CHECK(tsys::looks_like_png(outcome.file->bytes)); // a real PNG file
    return tsys::decode_png(outcome.file->bytes); // decodable with real pixels
}

// Waits until the probe reads `expected` (bounded: no long real sleeps).
void await_probe(const std::atomic<int>& probe, const int expected) {
    for (int micros = 0; micros < 2'000'000 && probe.load() != expected; ++micros) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

} // namespace

// ---- dispatcher-level lifecycle ------------------------------------------------

TEST_CASE("start_webcam succeeds while other commands are served; duplicate start is refused; stop delivers the capture") {
    for (const tsys::Platform platform : {tsys::Platform::Windows, tsys::Platform::Linux}) {
        CAPTURE(tsys::platform_name(platform));

        auto camera = std::make_shared<TestCamera>(
            []() -> std::shared_ptr<tsys::IFrameSource> {
                return std::make_shared<FakeFrameSource>(g_probe);
            });
        tsys::CommandDispatcher dispatcher = dispatcher_with(platform, camera->family);

        // 1. Start: a successful background job (the call returns at once).
        const tsys::Outcome started = dispatcher.dispatch("start_webcam", {});
        REQUIRE(started.ok);
        CHECK(started.code == "ok");
        CHECK(camera->registry->running(tsys::BackgroundJobRegistry::kCameraSlot));

        // 2. The capture continues in the background while the call is idle;
        // waiting here is test-side synchronization, not a command stall.
        await_probe(g_probe, 1);

        // 3. Duplicate start: the structured already-running refusal, and
        // the incumbent job is untouched (its slot remains occupied).
        const tsys::Outcome duplicated = dispatcher.dispatch("start_webcam", {});
        REQUIRE_FALSE(duplicated.ok);
        CHECK(duplicated.code == "already_running");
        CHECK(camera->registry->running(tsys::BackgroundJobRegistry::kCameraSlot));

        // 4. Stop: terminates the job and the accumulated result arrives as
        // a byte-complete, named, decodable PNG with the fake's pixels.
        const tsys::Outcome stopped = dispatcher.dispatch("stop_webcam", {});
        REQUIRE(stopped.ok);
        CHECK(stopped.code == "ok");
        const tsys::Frame delivered = delivered_frame(stopped);
        CHECK(frames_equal(delivered, make_frame(8, 6)));
        CHECK_FALSE(
            camera->registry->running(tsys::BackgroundJobRegistry::kCameraSlot)); // slot released

        // 5. Post-stop lifecycle: nothing running -> the not_running refusal,
        // and the freed slot starts a fresh job that stops cleanly again.
        const tsys::Outcome stop_again = dispatcher.dispatch("stop_webcam", {});
        REQUIRE_FALSE(stop_again.ok);
        CHECK(stop_again.code == "not_running");

        g_probe.store(0);
        const tsys::Outcome restarted = dispatcher.dispatch("start_webcam", {});
        REQUIRE(restarted.ok);
        await_probe(g_probe, 1);
        const tsys::Outcome stopped_again = dispatcher.dispatch("stop_webcam", {});
        REQUIRE(stopped_again.ok);
        (void)delivered_frame(stopped_again);
        CHECK_FALSE(camera->registry->running(tsys::BackgroundJobRegistry::kCameraSlot));
    }
}

TEST_CASE("extra arguments are invalid_args without reaching the job registry") {
    auto camera = std::make_shared<TestCamera>(
        []() -> std::shared_ptr<tsys::IFrameSource> {
            return std::make_shared<FakeFrameSource>(g_probe);
        });
    tsys::CommandDispatcher dispatcher = dispatcher_with(tsys::Platform::Windows, camera->family);

    const tsys::Outcome start_bad = dispatcher.dispatch("start_webcam", {"hd"});
    CHECK_FALSE(start_bad.ok);
    CHECK(start_bad.code == "invalid_args");
    CHECK_FALSE(camera->registry->running(tsys::BackgroundJobRegistry::kCameraSlot));

    const tsys::Outcome stop_bad = dispatcher.dispatch("stop_webcam", {"now"});
    CHECK_FALSE(stop_bad.ok);
    CHECK(stop_bad.code == "invalid_args");
    CHECK_FALSE(camera->registry->running(tsys::BackgroundJobRegistry::kCameraSlot));
}

TEST_CASE("a camera device that cannot be opened surfaces as a classified execute_failed at stop") {
    auto camera = std::make_shared<TestCamera>(
        []() -> std::shared_ptr<tsys::IFrameSource> {
            return std::make_shared<UnopenedCameraSource>();
        });
    tsys::CommandDispatcher dispatcher = dispatcher_with(tsys::Platform::Linux, camera->family);

    // Start reports the job seated (an async probe), the failure surfaces
    // through the structured stop outcome, and the registry slot is clean.
    const tsys::Outcome started = dispatcher.dispatch("start_webcam", {});
    REQUIRE(started.ok);

    const tsys::Outcome stopped = dispatcher.dispatch("stop_webcam", {});
    REQUIRE_FALSE(stopped.ok);
    CHECK(stopped.code == "execute_failed");
    CHECK(stopped.message.find("could not be opened") != std::string::npos);
    CHECK_FALSE(camera->registry->running(tsys::BackgroundJobRegistry::kCameraSlot));
}

TEST_CASE("a camera device that stops delivering frames surfaces a classified failure at stop") {
    auto camera = std::make_shared<TestCamera>(
        []() -> std::shared_ptr<tsys::IFrameSource> {
            return std::make_shared<FakeFrameSource>(g_probe, 2 /*two good frames, then gone*/);
        });
    tsys::CommandDispatcher dispatcher = dispatcher_with(tsys::Platform::Windows, camera->family);

    REQUIRE(dispatcher.dispatch("start_webcam", {}).ok);
    await_probe(g_probe, -1); // the device reached its failure (test-side gate)
    const tsys::Outcome stopped = dispatcher.dispatch("stop_webcam", {});
    REQUIRE_FALSE(stopped.ok);
    CHECK(stopped.code == "execute_failed");
    CHECK(stopped.message.find("stopped delivering") != std::string::npos);
    CHECK_FALSE(camera->registry->running(tsys::BackgroundJobRegistry::kCameraSlot));
}

TEST_CASE("the production cv::VideoCapture source settles a device-less host with a classified outcome") {
    // CI runners have no webcams (nor does WSL under WSLg). The production
    // factory resolves the no-device story into the structured failure at
    // stop, never an unclassified outcome. Where a device DOES exist (a
    // developer workstation), the same test proves the full real lifecycle.
    tsys::CommandDispatcher dispatcher(tsys::current_platform());
    dispatcher.set_camera_capture(tsys::make_camera_capture());

    const tsys::Outcome started = dispatcher.dispatch("start_webcam", {});
    REQUIRE(started.ok); // the job seats; the device probe runs async

    const tsys::Outcome stopped = dispatcher.dispatch("stop_webcam", {});
    if (!stopped.ok) {
        CHECK(stopped.code == "execute_failed");
        CHECK(stopped.message.find("camera") != std::string::npos);
    } else {
        // A device existed: the deliverable is a decodable PNG (real path).
        REQUIRE(stopped.file.has_value());
        CHECK(stopped.file->name == "webcam.png");
        CHECK(tsys::looks_like_png(stopped.file->bytes));
        INFO("webcam present: " << stopped.file->bytes.size() << " encoded bytes");
    }
}

// ---- session-level: "further commands including a stop are served normally" ----

namespace {

tp::SessionConfig session_config() { return tp::SessionConfig{100, 200, 100, 300}; }

tp::TokenResolver resolver_for(const std::string& token) {
    return [token](const std::string&) { return token; };
}

// The same handshaken in-memory session pair harness command_session_test
// uses, with the camera family FAKE-injected on the agent side. The
// dispatcher is a harness member so it outlives the installed handler.
struct SessionHarness {
    tp::InMemoryConnection wires;
    std::shared_ptr<tp::ConsoleSession> console;
    std::unique_ptr<tp::AgentSession> agent;
    std::shared_ptr<tsys::BackgroundJobRegistry> registry;
    tsys::CommandDispatcher dispatcher; // outlives the installed handler

    static std::unique_ptr<SessionHarness> connect() {
        auto harness = std::make_unique<SessionHarness>();
        harness->registry = std::make_shared<tsys::BackgroundJobRegistry>();
        auto camera = std::make_shared<tsys::VideoCameraCapture>(
            []() -> std::shared_ptr<tsys::IFrameSource> {
                return std::make_shared<FakeFrameSource>(g_probe);
            },
            harness->registry, []() { std::this_thread::yield(); });
        harness->dispatcher.set_camera_capture(std::move(camera));
        harness->console = std::make_shared<tp::ConsoleSession>(
            harness->wires.to_console, harness->wires.to_agent, session_config(),
            resolver_for("tok"));
        harness->agent = std::make_unique<tp::AgentSession>(
            harness->wires.to_agent, harness->wires.to_console,
            tp::AgentIdentity{"agent-1", "h1"}, "tok", session_config());
        tether::agent::install_commands(harness->dispatcher, *harness->agent);
        harness->agent->begin(0);
        harness->console->pump(1);
        harness->agent->pump(2);
        harness->console->pump(3);
        REQUIRE(harness->console->state() == tp::SessionState::Established);
        return harness;
    }

    // Assembles the file pieces of a result view into one byte vector.
    std::vector<unsigned char> file_bytes_of(const tp::CommandView& view) const {
        std::vector<unsigned char> bytes;
        bool saw_meta = false;
        std::uint64_t declared = 0;
        for (const tp::Message& piece : view.pieces) {
            if (const auto* meta = std::get_if<tp::FileMeta>(&piece)) {
                saw_meta = true;
                declared = meta->size;
                CHECK(meta->name == "webcam.png");
            } else if (const auto* chunk = std::get_if<tp::FileChunk>(&piece)) {
                bytes.insert(bytes.end(), chunk->bytes.begin(), chunk->bytes.end());
            } else if (std::get_if<tp::FileEnd>(&piece) != nullptr) {
                // the stream terminated with a FILE_END
            }
        }
        REQUIRE(saw_meta);
        CHECK(bytes.size() == declared); // the size the meta promised
        return bytes;
    }

    tp::CommandView issue(const tp::Cmd& command) {
        const std::uint32_t seq = console->start(command, 10);
        agent->pump(11);
        console->pump(12);
        const auto view = console->command(seq);
        REQUIRE(view.has_value());
        REQUIRE(view->status != tp::CommandStatus::Pending); // sync within one pump
        return *view;
    }
};

} // namespace

TEST_CASE("background camera job over the wire: further commands are served while it runs") {
    auto harness = SessionHarness::connect();

    // Start over the wire; the worker runs on the background thread.
    const tp::CommandView started = harness->issue(tp::Cmd{"start_webcam", {}});
    CHECK(started.status == tp::CommandStatus::Completed);

    // The capture continues in the background; commands keep being served
    // normally WHILE the job is alive (a closed-set rejection rides the
    // same session here).
    await_probe(g_probe, 1);
    const tp::CommandView unrelated = harness->issue(tp::Cmd{"totally_unknown", {}});
    CHECK(unrelated.status == tp::CommandStatus::Failed);

    // Stop over the wire: the accumulated capture streams as file pieces.
    const tp::CommandView stopped = harness->issue(tp::Cmd{"stop_webcam", {}});
    CHECK(stopped.status == tp::CommandStatus::Completed);
    const std::vector<unsigned char> bytes = harness->file_bytes_of(stopped);
    const tsys::Frame delivered = tsys::decode_png(bytes);
    CHECK(frames_equal(delivered, make_frame(8, 6)));
    CHECK_FALSE(harness->registry->running(tsys::BackgroundJobRegistry::kCameraSlot));
}
