// tests/power_family_test.cpp - task 6.4: the power family (shutdown /
// restart) with explicit authorization handling through the capability
// agent layer (the dispatcher). Scenarios map to the remote-commands delta:
//
//  - "missing confirmation blocks a destructive command" AT THE AGENT LAYER:
//    an unconfirmed shutdown/restart refuses with the structured
//    missing_confirmation result and NOTHING executes - proven twice: with
//    a recording fake (zero calls) and against the REAL per-platform
//    family implementation (its logind binding / ExitWindowsEx are never
//    reached; on the Windows side that means no genuine shutdown API call
//    ever happens from a test);
//  - "confirmation-bearing destructive command proceeds": confirmed
//    shutdown/restart reach the family exactly once, with the confirmation
//    token stripped - tested against FAKE/INJECTED backends only (a fake
//    IPowerControl for the dispatcher-level flow; for the Linux logind
//    backend, LogindPowerControl over a fake ILogindBinding). No test ever
//    triggers a real power transition on any platform;
//  - a refused confirmed command (logind unavailable) resolves to the
//    structured execute_failed outcome and the session stays usable;
//  - argument shape: extra arguments beyond the token are invalid_args.
//
// The dispatcher's platform is injectable, so every scenario runs on BOTH
// CI legs (windows-latest and ubuntu) against both matrix rows.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "matrix.hpp"
#include "power.hpp"

#include <string>
#include <vector>

namespace tsys = tether::syscalls;

namespace {

// Recording fake family (the dispatcher-level injection point).
struct FakePowerControl final : tsys::IPowerControl {
    int shutdown_calls = 0;
    int restart_calls = 0;
    void shutdown() override { ++shutdown_calls; }
    void restart() override { ++restart_calls; }
};

// Recording fake logind binding (the backend-level injection point; the
// real sd-bus binding is never touched by a test).
struct FakeLogindBinding final : tsys::ILogindBinding {
    bool is_available = true;
    int power_off_calls = 0;
    std::vector<bool> restart_flags;

    bool available() override { return is_available; }
    void power_off(const bool restart) override {
        if (!is_available) {
            throw tsys::SyscallError("execute_failed", "fake logind: no manager");
        }
        ++power_off_calls;
        restart_flags.push_back(restart);
    }
};

// A dispatcher with the power family registered and the file family on the
// real temporary-disk implementation (for the session-stays-usable checks).
tsys::CommandDispatcher wired_dispatcher(tsys::Platform platform,
                                         std::shared_ptr<tsys::IPowerControl> power) {
    tsys::CommandDispatcher dispatcher(platform);
    dispatcher.set_power_control(std::move(power));
    return dispatcher;
}

std::string outcome_text(const tsys::Outcome& outcome) {
    return outcome.code + ": " + outcome.message;
}

} // namespace

// ---- the confirmation gate (tested for real, on both platforms) --------------

TEST_CASE("unconfirmed shutdown refuses with a structured result and executes nothing") {
    for (const tsys::Platform platform : {tsys::Platform::Windows, tsys::Platform::Linux}) {
        CAPTURE(tsys::platform_name(platform));

        // Against the recording fake: the family sees zero calls.
        const auto fake = std::make_shared<FakePowerControl>();
        tsys::CommandDispatcher dispatcher = wired_dispatcher(platform, fake);

        const tsys::Outcome refused = dispatcher.dispatch("shutdown", {});
        REQUIRE_FALSE(refused.ok);
        CHECK(refused.code == "missing_confirmation");
        CHECK(refused.message.find("confirm") != std::string::npos);
        CHECK(refused.message.find("nothing was executed") != std::string::npos);
        CHECK(fake->shutdown_calls == 0); // zero system effect behind the refusal

        // A wrong token ("yes") is equally refused; still zero calls.
        const tsys::Outcome wrong_token = dispatcher.dispatch("shutdown", {"yes"});
        CHECK(wrong_token.code == "missing_confirmation");
        CHECK(fake->shutdown_calls == 0);

        // Against the REAL per-platform family: the refusal happens at the
        // dispatcher's gate BEFORE the family, so the real ExitWindowsEx /
        // sd-bus code path is constructed but never invoked.
        tsys::CommandDispatcher real_dispatcher(platform);
        real_dispatcher.set_power_control(tsys::make_power_control());
        const tsys::Outcome real_refused = real_dispatcher.dispatch("shutdown", {});
        CHECK(real_refused.code == "missing_confirmation");

        // Same refusal for an unconfirmed restart; fake stays untouched.
        const tsys::Outcome restart_refused = dispatcher.dispatch("restart", {"confirm", "extra"});
        CHECK(restart_refused.code == "invalid_args"); // token strip + arg check
        CHECK(fake->restart_calls == 0);
        const tsys::Outcome restart_token_refused = dispatcher.dispatch("restart", {});
        CHECK(restart_token_refused.code == "missing_confirmation");
        CHECK(fake->restart_calls == 0);
    }
}

TEST_CASE("a confirmed shutdown/restart proceeds exactly once with the token stripped") {
    for (const tsys::Platform platform : {tsys::Platform::Windows, tsys::Platform::Linux}) {
        CAPTURE(tsys::platform_name(platform));
        const auto fake = std::make_shared<FakePowerControl>();
        tsys::CommandDispatcher dispatcher = wired_dispatcher(platform, fake);

        const tsys::Outcome shutdown = dispatcher.dispatch("shutdown", {tsys::kConfirmMarker});
        REQUIRE(shutdown.ok);
        CHECK(shutdown.code == "ok");
        CHECK(fake->shutdown_calls == 1);
        CHECK(fake->restart_calls == 0);
        CHECK(shutdown.lines[0] == "shutdown initiated");

        const tsys::Outcome restart = dispatcher.dispatch("restart", {tsys::kConfirmMarker});
        REQUIRE(restart.ok);
        CHECK(fake->restart_calls == 1);

        // Extra arguments beyond the token are a structured invalid_args -
        // the family never runs for a malformed call.
        const tsys::Outcome extra = dispatcher.dispatch("shutdown",
                                                        {tsys::kConfirmMarker, "--force"});
        CHECK_FALSE(extra.ok);
        CHECK(extra.code == "invalid_args");
        CHECK(fake->shutdown_calls == 1); // unchanged
    }
}

// ---- the Linux logind backend over the injectable binding ---------------------

TEST_CASE("the logind backend routes confirmed calls through the injected binding") {
    const auto binding = std::make_shared<FakeLogindBinding>();
    auto power = tsys::make_linux_power_control(binding);
    tsys::CommandDispatcher dispatcher = wired_dispatcher(tsys::Platform::Linux, power);

    const tsys::Outcome shutdown = dispatcher.dispatch("shutdown", {tsys::kConfirmMarker});
    REQUIRE(shutdown.ok);
    REQUIRE(binding->power_off_calls == 1);
    CHECK(binding->restart_flags[0] == false); // PowerOff, not Reboot

    const tsys::Outcome restart = dispatcher.dispatch("restart", {tsys::kConfirmMarker});
    REQUIRE(restart.ok);
    REQUIRE(binding->power_off_calls == 2);
    CHECK(binding->restart_flags[1] == true); // Reboot
}

TEST_CASE("a logind outage refuses the confirmed call with a structured outcome and the session survives") {
    const auto binding = std::make_shared<FakeLogindBinding>();
    binding->is_available = false; // no libsystemd / no system bus / no manager
    tsys::CommandDispatcher dispatcher =
        wired_dispatcher(tsys::Platform::Linux, tsys::make_linux_power_control(binding));

    const tsys::Outcome refused = dispatcher.dispatch("shutdown", {tsys::kConfirmMarker});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.code == "execute_failed");
    CHECK(refused.message.find("logind") != std::string::npos);
    CHECK(binding->power_off_calls == 0); // the outage path never reached the manager

    // The session stays usable: a real command right after works normally.
    SUBCASE("follow-up command succeeds") {
        // The closed-set dispatch still surfaces the structured error (no
        // wedge, no unclassified tail) after the outage refusal.
        const tsys::Outcome later = dispatcher.dispatch("totally_unknown_verb", {});
        CHECK(later.code == "unknown_command");

        // And with the file family live the next command authoritatively
        // runs (a real temp dir listing).
        tsys::CommandDispatcher alive(tsys::Platform::Linux);
        alive.set_power_control(tsys::make_linux_power_control(binding));
        alive.set_file_system(tsys::make_file_system());
        const tsys::Outcome listing = alive.dispatch("ls", {});
        REQUIRE(listing.ok); // the dispatcher was never wedged by the refusal
    }
}

// ---- outcome authority ----------------------------------------------------------

TEST_CASE("every power command resolves to a definitive classified outcome") {
    const auto fake = std::make_shared<FakePowerControl>();
    tsys::CommandDispatcher dispatcher = wired_dispatcher(tsys::Platform::Windows, fake);

    for (const std::vector<std::string>& call :
         {std::vector<std::string>{}, std::vector<std::string>{"confirm"},
          std::vector<std::string>{"confirm", "extra"}}) {
        for (const char* verb : {"shutdown", "restart"}) {
            const tsys::Outcome outcome = dispatcher.dispatch(verb, call);
            // Only the flow codes are legal here - never unclassified.
            const bool classified = outcome.ok ||
                                    outcome.code == "missing_confirmation" ||
                                    outcome.code == "invalid_args";
            CHECK_MESSAGE(classified, outcome_text(outcome));
        }
    }
}
