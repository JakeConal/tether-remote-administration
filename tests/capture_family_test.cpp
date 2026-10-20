// tests/capture_family_test.cpp - task 6.5: the capture family through the
// capability agent layer, proving a produced screenshot is a DECODABLE
// image file (cv::imdecode back into real pixels), plus the structured
// refusal paths. Scenarios map to the remote-commands delta:
//
//  - "every defined command resolves to a definitive outcome": screenshot
//    success (byte-complete, named PNG) on the injected fake source; and,
//    on THIS platform, the REAL per-OS implementation (Windows GDI /
//    Linux X11) runs whenever a desktop or $DISPLAY is reachable (the CI
//    matrix runs the Linux leg under xvfb) - its bytes are decode-checked.
//    The real-implementations of the OTHER platform are constructed but
//    never invoked here;
//  - "out-of-matrix capture command returns a structured refusal":
//    constructed with the OTHER platform's matrix row, a supported-family
//    verb resolves to the structured unsupported result and the session
//    stays usable;
//  - failure surfaces: a grab that cannot reach a Display/server raises
//    the classified execute_failed outcome; extra arguments are invalid_args.
//
// The decode proof is the spec's "usable as a file of that format": PNG
// signature + cv::imdecode -> non-empty BGR-8U Frame of plausible size.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "capture.hpp"
#include "image_png.hpp"
#include "matrix.hpp"

#include <memory>
#include <string>
#include <vector>

namespace tsys = tether::syscalls;

namespace {

// A fake screen grabber returning a REAL, decodable PNG (the decode proof
// exercises the same Outcome::Deliverable->file-pieces flow the agent uses).
struct FakeScreenCapture final : tsys::IScreenCapture {
    int grab_calls = 0;
    tsys::Outcome::Deliverable grab() override {
        ++grab_calls;
        tsys::Outcome::Deliverable deliverable;
        deliverable.name = "screenshot.png";
        const tsys::Frame frame{4, 4,
                                {0, 128, 255, 10, 20, 30, 0, 128, 255, 10, 20, 30,
                                 0, 128, 255, 10, 20, 30, 0, 128, 255, 10, 20, 30,
                                 0, 128, 255, 10, 20, 30, 0, 128, 255, 10, 20, 30,
                                 0, 128, 255, 10, 20, 30, 0, 128, 255, 10, 20, 30}};
        deliverable.bytes = tsys::encode_png_bgr(frame);
        return deliverable;
    }
};

tsys::CommandDispatcher dispatcher_with(tsys::Platform platform,
                                        std::shared_ptr<tsys::IScreenCapture> capture) {
    tsys::CommandDispatcher dispatcher(platform);
    dispatcher.set_screen_capture(std::move(capture));
    return dispatcher;
}

} // namespace

TEST_CASE("screenshot produces a byte-complete, decodable PNG deliverable") {
    const auto fake = std::make_shared<FakeScreenCapture>();
    tsys::CommandDispatcher dispatcher =
        dispatcher_with(tsys::Platform::Windows, fake); // platform-injectable: both rows run on both legs

    const tsys::Outcome outcome = dispatcher.dispatch("screenshot", {});
    REQUIRE(outcome.ok);
    CHECK(outcome.code == "ok");

    // The deliverable is set with its name; the session layer streams it
    // through FILE_META{name} + FILE_CHUNKs + FILE_END.
    REQUIRE(outcome.file.has_value());
    CHECK(outcome.file->name == "screenshot.png");
    CHECK(tsys::looks_like_png(outcome.file->bytes));

    // The decode proof: signature + real pixels with the exact dims/pixels
    // the grab produced (not just non-empty bytes).
    const tsys::Frame decoded = tsys::decode_png(outcome.file->bytes);
    CHECK(decoded.width == 4);
    CHECK(decoded.height == 4);
    REQUIRE(decoded.bgr.size() == 4 * 4 * 3);
    CHECK(decoded.bgr[0] == 0);    // blue
    CHECK(decoded.bgr[1] == 128);  // green
    CHECK(decoded.bgr[2] == 255);  // red
    CHECK(fake->grab_calls == 1);  // exactly one grab per screenshot command
}

TEST_CASE("extra arguments to screenshot are a structured invalid_args") {
    const auto fake = std::make_shared<FakeScreenCapture>();
    tsys::CommandDispatcher dispatcher = dispatcher_with(tsys::Platform::Windows, fake);

    const tsys::Outcome outcome = dispatcher.dispatch("screenshot", {"--full"});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.code == "invalid_args");
    CHECK(fake->grab_calls == 0); // malformed params never reach the family
}

TEST_CASE("out-of-matrix capture on the other platform refuses structurally and the session continues") {
    const auto fake = std::make_shared<FakeScreenCapture>();
    // Pin the matrix row the scenario needs (the platform flag swap lives
    // in the suite's table, not the global command set).
    tsys::CommandSpec patched = *tsys::find_command("screenshot");
    patched.on_linux = false; // out of the Linux matrix (as on a headless-host design row)
    std::vector<tsys::CommandSpec> patched_table;
    for (const tsys::CommandSpec& spec : tsys::command_set()) {
        if (spec.family == tsys::Family::Capture) {
            patched_table.push_back(patched);
        } else {
            patched_table.push_back(spec);
        }
    }
    tsys::CommandDispatcher gated(tsys::Platform::Linux, patched_table);
    gated.set_screen_capture(fake);

    const tsys::Outcome refused = gated.dispatch("screenshot", {});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.code == "unsupported_platform");
    CHECK(refused.message.find("linux") != std::string::npos);
    CHECK(refused.message.find("capture") != std::string::npos);
    CHECK(fake->grab_calls == 0);

    // The session stays usable (matrix_test depth-checks the mechanism;
    // here the closed set keeps rejecting/accepting as expected).
    CHECK(gated.dispatch("screenshot", {}).code == "unsupported_platform");
}

TEST_CASE("a capture source that cannot reach the display fails with a classified outcome") {
    struct HeadlessCapture final : tsys::IScreenCapture {
        tsys::Outcome::Deliverable grab() override {
            throw tsys::SyscallError("execute_failed",
                                     "no X display is reachable (is DISPLAY set?): the capture "
                                     "family is X11 on Linux");
        }
    };
    tsys::CommandDispatcher dispatcher =
        dispatcher_with(tsys::Platform::Linux, std::make_shared<HeadlessCapture>());

    const tsys::Outcome outcome = dispatcher.dispatch("screenshot", {});
    REQUIRE_FALSE(outcome.ok);
    CHECK(outcome.code == "execute_failed");
}

// ---- the REAL implementation (exercised on the platform each leg runs) -------

TEST_CASE("the real per-OS screenshot implementation produces a decodable image on this platform") {
    // NOT a fake: the GDI/X11 path itself. CI gives Windows a desktop
    // session and Linux an X display (xvfb); WSL runs under WSLg. A
    // headless state refuses with the classified result instead of
    // crashing - both branches are legal here.
    tsys::CommandDispatcher dispatcher(
#if defined(_WIN32)
        tsys::Platform::Windows
#else
        tsys::Platform::Linux
#endif
    );
    dispatcher.set_screen_capture(tsys::make_screen_capture());

    const tsys::Outcome outcome = dispatcher.dispatch("screenshot", {});
    if (outcome.ok) {
        REQUIRE(outcome.file.has_value());
        CHECK(outcome.file->name == "screenshot.png");
        CHECK(tsys::looks_like_png(outcome.file->bytes));

        // The decode + dims are the real-pixel proof (a solid-color xvfb
        // root decodes to uniform pixels legitimately).
        const tsys::Frame decoded = tsys::decode_png(outcome.file->bytes);
        CHECK(decoded.width > 100);
        CHECK(decoded.height > 100);
        REQUIRE(decoded.bgr.size() == static_cast<size_t>(decoded.width) * decoded.height * 3);
        INFO("decodable screenshot: " << decoded.width << "x" << decoded.height);
    } else {
        // Headless state: the classified refusal, never a wedge.
        CHECK(outcome.code == "execute_failed");
        CHECK(outcome.message.find("DISPLAY") != std::string::npos);
    }
}

TEST_CASE("the real implementation factory exists in this binary") {
    // Its per-OS twin is exercised on ITS CI leg; building and linking the
    // other branch everywhere keeps a regression in either branch visible
    // on every leg (design D6: "Windows behavior must not regress").
    auto capture = tsys::make_screen_capture();
    REQUIRE(capture != nullptr);
    (void)capture;
}
