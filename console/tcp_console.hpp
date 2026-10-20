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

private:
    transport::TcpConsoleTransport* transport_ = nullptr;
    std::unique_ptr<transport::TcpConsoleTransport> owned_;
    Clock clock_;
    std::function<void()> side_pump_;
    std::int64_t dispatch_budget_ms_ = 65'000;
};

} // namespace tether::cli
