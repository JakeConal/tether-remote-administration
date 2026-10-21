// console/tcp_console.hpp - the real IOperConsole backend (task 5.5 path):
// the TCP console transport plus the protocol AgentRegistry underneath.
// Production (main.cpp) hands over an owned, started transport; the Phase 5
// integration test borrows its own transport so the fake agent can dial it
// before run_cli ever constructs the console.
#pragma once

#include "cli.hpp"
#include "tcp.hpp"

#include <functional>
#include <memory>

namespace tether::cli {

class TcpOperConsole final : public IOperConsole {
public:
    // Wall clock for the dispatch pump loop (injected in tests).
    using Clock = std::function<transport::TransportMillis()>;

    // Taking OWNERSHIP of a started transport (production path).
    explicit TcpOperConsole(std::unique_ptr<transport::TcpConsoleTransport> owned,
                            Clock clock = transport::monotonic_now_ms,
                            std::function<void()> side_pump = nullptr);
    // Borrowing a transport the caller keeps alive (tests).
    explicit TcpOperConsole(transport::TcpConsoleTransport& borrowed,
                            Clock clock = transport::monotonic_now_ms,
                            std::function<void()> side_pump = nullptr);

    std::vector<protocol::RegistryEntry> list_agents() override;
    bool has_agent(const std::string& identity) override;
    std::optional<protocol::CommandView> dispatch(const std::string& agent,
                                                  const protocol::Cmd& command,
                                                  const protocol::ResultHook& hook) override;
    CancelOutcome cancel(const std::string& agent, std::uint32_t seq) override;

    // Upper wall-clock bound for one dispatch wait; the session's own
    // per-command timeout normally fires first.
    void set_dispatch_budget_ms(std::int64_t budget) { dispatch_budget_ms_ = budget; }

    // Bounded dial-in wait for the one-invocation-one-verb runtime shape
    // (console/main.cpp): a verb's ephemeral listener exists only inside its
    // invocation, while the agent dials out forever on a bounded backoff
    // (500 ms doubling to a 10 s cap). has_agent/list_agents pump the
    // transport for up to this budget so the agent's next dial lands inside
    // the verb instead of the operator needing an impossible timing dance;
    // the budget then expires into the same unknown-agent refusal as before
    // (structured error, stderr, exit 3, nothing dispatched). Default 0 =
    // no wait (tests pre-dial their agents; production wires the config
    // field `agent_dial_wait_ms`).
    void set_agent_dial_budget_ms(std::int64_t budget) { agent_dial_budget_ms_ = budget; }

private:
    transport::TcpConsoleTransport* transport_ = nullptr;
    std::unique_ptr<transport::TcpConsoleTransport> owned_;
    Clock clock_;
    std::function<void()> side_pump_;
    std::int64_t dispatch_budget_ms_ = 65'000;
    std::int64_t agent_dial_budget_ms_ = 0;

    // Pumps the transport (accept + session handshakes + registry
    // registration) until `ready` turns true or the dial budget expires.
    // No-op without a budget, so preset tests keep their exact timing.
    void wait_for_dial(const std::function<bool()>& ready);
};

} // namespace tether::cli
