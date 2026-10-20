// console/tcp_console.cpp - the real backend behind run_cli (tasks 5.2/5.5):
// registry listing straight from the protocol AgentRegistry (5.1's derived
// live/stale/down states), command dispatch over the session stack, CANCEL
// routing, and the pump-until-terminal streaming loop.
//
// Concurrency model: single-threaded poll() like the transports - dispatch
// pumps the console transport each iteration and honors `side_pump_`, the
// hook the Phase 5 integration test uses to drive the fake agent's link so
// both stack halves advance without threads. Production leaves it empty.
#include "tcp_console.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace tether::cli {

TcpOperConsole::TcpOperConsole(std::unique_ptr<transport::TcpConsoleTransport> owned,
                               Clock clock, std::function<void()> side_pump)
    : transport_(owned.get()),
      owned_(std::move(owned)),
      clock_(std::move(clock)),
      side_pump_(std::move(side_pump)) {}

TcpOperConsole::TcpOperConsole(transport::TcpConsoleTransport& borrowed, Clock clock,
                               std::function<void()> side_pump)
    : transport_(&borrowed), clock_(std::move(clock)), side_pump_(std::move(side_pump)) {}

std::vector<protocol::RegistryEntry> TcpOperConsole::list_agents() {
    return transport_->registry_entries();
}

bool TcpOperConsole::has_agent(const std::string& identity) {
    return transport_->session_for(identity) != nullptr;
}

std::optional<protocol::CommandView> TcpOperConsole::dispatch(
    const std::string& agent, const protocol::Cmd& command, const protocol::ResultHook& hook) {
    // Unknown/non-eligible agents stay a verb-layer concern (exit 3); this
    // speaks TransportError when eligibility races away from the check.
    const std::uint32_t seq = transport_->issue(agent, command, hook, clock_());

    const transport::TransportMillis deadline = clock_() + dispatch_budget_ms_;
    for (;;) {
        const transport::TransportMillis now = clock_();
        transport_->pump(now);
        if (side_pump_) {
            side_pump_(); // tests drive the fake agent's link here
        }
        const auto session = transport_->session_for(agent);
        if (!session) {
            // The identity was re-registered/vanished mid-command; the view
            // is lost with it and the verb layer reports a runtime failure.
            return std::nullopt;
        }
        const std::optional<protocol::CommandView> view = session->command(seq);
        if (view && view->status != protocol::CommandStatus::Pending) {
            return view;
        }
        if (session->state() == protocol::SessionState::Down) {
            // Died while the command was still pending: no terminal view will
            // ever arrive on this session.
            return std::nullopt;
        }
        if (now >= deadline) {
            // Escape hatch - the session's own command timeout should always
            // fire first; this just keeps the loop honest about termination.
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

CancelOutcome TcpOperConsole::cancel(const std::string& agent, std::uint32_t seq) {
    const auto session = transport_->session_for(agent);
    if (!session || session->state() != protocol::SessionState::Established) {
        return CancelOutcome::NotEligible;
    }
    try {
        return session->cancel(seq) ? CancelOutcome::Sent : CancelOutcome::NotPending;
    } catch (const protocol::SessionError&) {
        return CancelOutcome::NotEligible;
    }
}

} // namespace tether::cli
