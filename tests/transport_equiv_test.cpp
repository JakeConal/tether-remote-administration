// tests/transport_equiv_test.cpp - task 4.4: both transports live behind ONE
// interface (transport.hpp), and the transport that carried a command is
// invisible to the command/result semantics:
//  - the SAME command issued over a live loopback TCP session and through the
//    Gmail adapter (mocked client; network never touched) yields the same
//    routed agent, the same result content, and per-command seq attribution
//  - GmailTransport::issue stays an explicit interface error (mail issuance is
//    the adapter's only inbound path), never a silent no-op
//  - a TCP-only console config constructs/polls no Gmail adapter at all (the
//    client factory is just never invoked) while TCP keeps working
//  - a gmail-enabled config builds the adapter exactly once, and the
//    production dispatcher seam routes mail-issued commands into the TCP
//    transport, where the stub agent answers and the result email carries the
//    real session seq and the identical result text
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "base64.hpp"
#include "config.hpp"
#include "gmail.hpp"
#include "session.hpp"
#include "tcp.hpp"
#include "transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace tt = tether::transport;
namespace tp = tether::protocol;
namespace tc = tether::common;

namespace {

tt::TransportMillis now_ms() {
    return std::int64_t{std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count()};
}

tp::SessionConfig loopback_config() {
    tp::SessionConfig config;
    config.command_timeout = 3000;
    config.auth_timeout = 2000;
    config.keepalive_interval = 50;
    config.keepalive_window = 150;
    return config;
}

tp::TokenResolver resolver_from(std::map<std::string, std::string> tokens) {
    return [tokens = std::move(tokens)](const std::string& identity) {
        const auto it = tokens.find(identity);
        return it == tokens.end() ? std::string{} : it->second;
    };
}

tt::ConsoleTransportOptions loopback_console_options(
    const std::map<std::string, std::string>& tokens, tp::SessionConfig session) {
    tt::ConsoleTransportOptions options;
    options.listen_address = "127.0.0.1";
    options.port = 0; // ephemeral
    options.session = std::move(session);
    options.resolve_token = resolver_from(tokens);
    return options;
}

std::string result_text(const tp::CommandView& view) {
    std::string text;
    for (const tp::Message& piece : view.pieces) {
        if (std::holds_alternative<tp::ResultText>(piece)) {
            text += std::get<tp::ResultText>(piece).text;
        }
    }
    return text;
}

bool established(const tt::TcpConsoleTransport& console, const std::string& identity) {
    const auto session = console.session_for(identity);
    return session && session->state() == tp::SessionState::Established;
}

// Drives console+agent pumps until the P3 handshake completes (real wall now;
// framing rides real async sockets, so deadline-based with small sleeps).
bool handshake_until_established(tt::TcpConsoleTransport& console,
                                 std::unique_ptr<tt::TcpAgentLink>& agent,
                                 const std::string& identity,
                                 tt::TransportMillis budget_ms = 1500) {
    agent->begin_handshake(now_ms());
    const tt::TransportMillis deadline = now_ms() + budget_ms;
    for (;;) {
        const tt::TransportMillis now = now_ms();
        console.pump(now);
        agent->pump(now);
        if (agent->established() && established(console, identity)) {
            return true;
        }
        if (now >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Drives console+agent pumps until the given seq is no longer pending on the
// session the registry holds for `identity`. Real sockets deliver
// asynchronously, so deadline-based with small sleeps rather than a pump count.
void pump_until_terminal(tt::TcpConsoleTransport& console,
                         std::unique_ptr<tt::TcpAgentLink>& agent, const std::string& identity,
                         std::uint32_t seq, int budget_ms = 400) {
    const tt::TransportMillis deadline = now_ms() + budget_ms;
    for (;;) {
        const auto view = console.session_for(identity)->command(seq);
        if (view && view->status != tp::CommandStatus::Pending) {
            return;
        }
        if (now_ms() >= deadline) {
            return;
        }
        const tt::TransportMillis now = now_ms();
        agent->pump(now);
        console.pump(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// ---- Gmail-side doubles -------------------------------------------------------------

struct FakeGmailClient final : tt::GmailClient {
    std::map<std::string, tt::MailMessage> inbox;
    std::vector<std::string> newest_first; // API order: most recent first
    std::vector<std::string> sent_raw;
    int list_calls = 0;

    std::vector<std::string> list_inbox_ids(std::size_t max_results) override {
        ++list_calls;
        std::vector<std::string> ids;
        for (const std::string& id : newest_first) {
            if (ids.size() >= max_results) {
                break;
            }
            ids.push_back(id);
        }
        return ids;
    }

    tt::MailMessage fetch(const std::string& id) override { return inbox.at(id); }

    std::string send_raw(const std::string& mail) override {
        sent_raw.push_back(mail);
        return "sent-" + std::to_string(sent_raw.size());
    }
};

// Records mail-issued commands; results can be completed later and in any
// order, matching a real dispatcher's out-of-order result delivery.
struct RecordingDispatcher {
    struct Issued {
        std::string agent;
        tp::Cmd command;
        tp::ResultHook hook;
    };
    std::vector<Issued> issued;
    std::uint32_t next_seq = 1;

    std::uint32_t dispatch(const std::string& agent, const tp::Cmd& command,
                           const tp::ResultHook& hook, tt::TransportMillis) {
        issued.push_back(Issued{agent, command, hook});
        return next_seq++;
    }

    void complete_text(std::uint32_t seq, const std::string& text) {
        issued[seq - 1].hook(seq, tp::ResultText{text}, true);
    }
};

std::string mail_body_command(const tp::Cmd& command) {
    std::string body = "{\"command\":\"" + command.command + "\",\"args\":[";
    for (std::size_t i = 0; i < command.args.size(); ++i) {
        if (i != 0) {
            body += ",";
        }
        body += "\"" + command.args[i] + "\"";
    }
    body += "]}";
    return tc::base64_encode_string(body);
}

tt::MailMessage command_mail(const std::string& id, const std::string& from, const tp::Cmd& command) {
    tt::MailMessage message;
    message.id = id;
    message.from = from;
    message.to = "relay@example.com";
    message.subject = "tether command " + id;
    message.body_text = mail_body_command(command);
    return message;
}

std::string decode_mail_text_body(const std::string& raw) {
    // Single text/plain part: everything after the blank line is the base64
    // body (result mails from the adapter are never multipart for text).
    const std::size_t split = raw.find("\r\n\r\n");
    REQUIRE(split != std::string::npos);
    return tc::base64_decode_string(raw.substr(split + 4));
}

std::string header_value(const std::string& raw, const std::string& header) {
    const std::size_t at = raw.find(header + ": ");
    if (at == std::string::npos) {
        return {};
    }
    const std::size_t begin = at + header.size() + 2;
    const std::size_t end = raw.find("\r\n", begin);
    REQUIRE(end != std::string::npos);
    return raw.substr(begin, end - begin);
}

tt::GmailTransportOptions gmail_options() {
    tt::GmailTransportOptions options;
    options.poll_interval_ms = 1000;
    options.backoff_initial_ms = 1000;
    options.backoff_max_ms = 10000;
    options.admin_list = {"admin@example.com"};
    options.adapter_account = "relay@example.com";
    options.default_agent = "agent-1";
    return options;
}

// A console config with the TCP-only shape (no gmail section at all).
tc::ConsoleConfig tcp_only_config() {
    tc::ConsoleConfig config;
    config.listen_address = "127.0.0.1";
    config.port = 0; // ephemeral listener for tests
    config.token_store_path = "tokens.json";
    return config;
}

// Writes a valid-shaped but FAKE credentials file (no real content, never in
// the repo) and returns its path.
std::string temp_credentials_path() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "tether-equiv-tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "gmail-credentials.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"({"account":"relay@example.com","client_id":"fake-id",)"
               R"("client_secret":"fake-secret","refresh_token":"fake-token"})";
    }
    return path.string();
}

tc::ConsoleConfig gmail_config(const std::string& credentials_path) {
    tc::ConsoleConfig config = tcp_only_config();
    tc::GmailSectionConfig gmail;
    gmail.enabled = true;
    gmail.credentials_path = credentials_path;
    gmail.poll_interval_seconds = 30;
    gmail.admin_list = {"admin@example.com"};
    gmail.default_agent = "agent-1";
    config.gmail = gmail;
    return config;
}

} // namespace

// ---- equivalence: the interface hides the transport ----------------------------

TEST_CASE("the same command over TCP and over mail yields equal semantics") {
    const tp::Cmd command{"exec", {"ls"}};
    const std::string shared_answer = "stub-responder:exec";

    // Leg A: the direct TCP transport, behind the interface.
    tt::TcpConsoleTransport console(loopback_console_options({{"agent-1", "tok-1"}},
                                                             loopback_config()));
    console.start();
    REQUIRE(console.listening_port() != 0);
    std::unique_ptr<tt::TcpAgentLink> agent = tt::TcpAgentLink::connect(
        {"127.0.0.1", console.listening_port()}, {"agent-1", "host-1"}, "tok-1",
        loopback_config());
    REQUIRE(handshake_until_established(console, agent, "agent-1"));
    agent->set_command_handler(
        [&](std::uint32_t seq, const tp::Cmd&, const std::function<bool()>&) {
            agent->send_result(seq, tp::ResultText{shared_answer});
        });

    std::vector<std::string> tcp_pieces;
    const tp::ResultHook tcp_hook = [&](std::uint32_t, const tp::Message& piece, bool) {
        if (std::holds_alternative<tp::ResultText>(piece)) {
            tcp_pieces.push_back(std::get<tp::ResultText>(piece).text);
        }
    };
    tt::ITransport& tcp_transport = console;
    const std::uint32_t tcp_seq = tcp_transport.issue("agent-1", command, tcp_hook, now_ms());
    pump_until_terminal(console, agent, "agent-1", tcp_seq);
    const auto tcp_view = console.session_for("agent-1")->command(tcp_seq);
    REQUIRE(tcp_view.has_value());
    REQUIRE(tcp_view->status == tp::CommandStatus::Completed);
    CHECK(result_text(*tcp_view) == shared_answer);
    CHECK(tcp_pieces == std::vector<std::string>{shared_answer});

    // Leg B: the SAME command arrives as admin mail through the Gmail adapter
    // (mocked client), also consumed behind the ITransport interface.
    auto client = std::make_shared<FakeGmailClient>();
    client->inbox.emplace("m1", command_mail("m1", "admin@example.com", command));
    client->newest_first = {"m1"};
    RecordingDispatcher dispatcher;
    tt::GmailTransport gmail(client, gmail_options(),
                             [&dispatcher](const std::string& routed, const tp::Cmd& cmd,
                                           const tp::ResultHook& hook, tt::TransportMillis now) {
                                 return dispatcher.dispatch(routed, cmd, hook, now);
                             });
    tt::ITransport& mail_transport = gmail;
    mail_transport.pump(0);

    REQUIRE(dispatcher.issued.size() == 1);
    // The mail carried exactly what TCP carried - same agent, same command.
    CHECK(dispatcher.issued[0].agent == "agent-1");
    CHECK(dispatcher.issued[0].command.command == command.command);
    CHECK(dispatcher.issued[0].command.args == command.args);

    // The response content equals the TCP leg's response text, attributed to
    // the command's seq (the dispatcher's returned seq, in the mail header).
    dispatcher.complete_text(1, shared_answer);
    REQUIRE(client->sent_raw.size() == 1);
    const std::string& mail = client->sent_raw[0];
    CHECK(header_value(mail, "X-Tether-Seq") == "1");
    CHECK(header_value(mail, "To") == "admin@example.com");
    CHECK(decode_mail_text_body(mail) == shared_answer);

    // Gmail and TCP agree on the issuer-visible result: the mail body equals
    // the pieces the TCP hook observed.
    CHECK(decode_mail_text_body(mail) == tcp_pieces.front());
}

TEST_CASE("gmail issue() stays an explicit interface error") {
    auto client = std::make_shared<FakeGmailClient>();
    RecordingDispatcher dispatcher;
    tt::GmailTransport gmail(client, gmail_options(),
                             [&dispatcher](const std::string& routed, const tp::Cmd& cmd,
                                           const tp::ResultHook& hook, tt::TransportMillis now) {
                                 return dispatcher.dispatch(routed, cmd, hook, now);
                             });
    tt::ITransport& mail_transport = gmail;
    CHECK_THROWS_AS(mail_transport.issue("agent-1", tp::Cmd{"exec", {}}, nullptr, 0),
                    tt::TransportError);
    CHECK(dispatcher.issued.empty()); // the programmatic issue ran nothing
}

TEST_CASE("mail results stay attributed per seq regardless of delivery order") {
    auto client = std::make_shared<FakeGmailClient>();
    client->inbox.emplace("m1", command_mail("m1", "admin@example.com", tp::Cmd{"exec", {"ls"}}));
    client->inbox.emplace("m2", command_mail("m2", "boss@example.com", tp::Cmd{"who", {}}));
    client->newest_first = {"m2", "m1"};
    RecordingDispatcher dispatcher;

    tt::GmailTransportOptions options = gmail_options();
    options.admin_list = {"admin@example.com", "boss@example.com"};
    tt::GmailTransport gmail(client, options,
                             [&dispatcher](const std::string& routed, const tp::Cmd& cmd,
                                           const tp::ResultHook& hook, tt::TransportMillis now) {
                                 return dispatcher.dispatch(routed, cmd, hook, now);
                             });

    gmail.pump(0);
    REQUIRE(dispatcher.issued.size() == 2);
    // Mail issuance order equals inbox age order (oldest command first), and
    // selector-less mail routes to the configured default agent.
    CHECK(dispatcher.issued[0].agent == "agent-1");
    CHECK(dispatcher.issued[0].command.command == "exec");
    CHECK(dispatcher.issued[1].agent == "agent-1");
    CHECK(dispatcher.issued[1].command.command == "who");

    // Results complete out of order (seq 2 before seq 1): every result mail
    // must carry its own seq, its own admin, and its own content.
    dispatcher.complete_text(2, "from-b");
    dispatcher.complete_text(1, "from-a");
    REQUIRE(client->sent_raw.size() == 2);

    bool saw_a = false;
    bool saw_b = false;
    for (const std::string& mail : client->sent_raw) {
        const std::string body = decode_mail_text_body(mail);
        const std::string seq = header_value(mail, "X-Tether-Seq");
        if (seq == "1") {
            CHECK(header_value(mail, "To") == "admin@example.com");
            CHECK(body == "from-a");
            saw_a = true;
        } else if (seq == "2") {
            CHECK(header_value(mail, "To") == "boss@example.com");
            CHECK(body == "from-b");
            saw_b = true;
        } else {
            CHECK_MESSAGE(false, "a result mail carried no owning seq: attribution would swap");
        }
    }
    CHECK(saw_a);
    CHECK(saw_b);
}

// ---- configuration enablement (the TCP-only scenario) --------------------------

TEST_CASE("a TCP-only config never constructs or polls the gmail adapter") {
    int factory_calls = 0;
    const tc::ConsoleConfig config = tcp_only_config();

    const tt::ConsoleTransportSet set = tt::build_console_transports(
        config, loopback_config(), resolver_from({{"agent-1", "tok-1"}}),
        [&factory_calls]() -> std::unique_ptr<tt::GmailClient> {
            ++factory_calls;
            return nullptr; // the adapter construction would reject this
        });

    CHECK(factory_calls == 0);   // the client factory was never invoked
    CHECK(set.gmail == nullptr); // no adapter exists, so nothing can poll
    CHECK_FALSE(set.gmail_enabled);

    // And the TCP transport inside the set is fully functional through the
    // interface: a command round-trips the live ephemeral listener.
    auto& console = static_cast<tt::TcpConsoleTransport&>(*set.tcp);
    console.start();
    REQUIRE(console.listening_port() != 0);

    std::unique_ptr<tt::TcpAgentLink> agent = tt::TcpAgentLink::connect(
        {"127.0.0.1", console.listening_port()}, {"agent-1", "host-1"}, "tok-1",
        loopback_config());
    REQUIRE(handshake_until_established(console, agent, "agent-1"));
    agent->set_command_handler(
        [&](std::uint32_t seq, const tp::Cmd&, const std::function<bool()>&) {
            agent->send_result(seq, tp::ResultText{"tcp-only"});
        });

    std::vector<std::string> pieces;
    const tp::ResultHook hook = [&](std::uint32_t, const tp::Message& piece, bool) {
        if (std::holds_alternative<tp::ResultText>(piece)) {
            pieces.push_back(std::get<tp::ResultText>(piece).text);
        }
    };
    tt::ITransport& tcp = *set.tcp;
    const std::uint32_t seq = tcp.issue("agent-1", tp::Cmd{"exec", {}}, hook, now_ms());
    pump_until_terminal(console, agent, "agent-1", seq);
    const auto view = console.session_for("agent-1")->command(seq);
    REQUIRE(view.has_value());
    CHECK(view->status == tp::CommandStatus::Completed);
    CHECK(pieces == std::vector<std::string>{"tcp-only"});
}

TEST_CASE("a gmail-enabled config builds the adapter once and mail rides the TCP seam") {
    int factory_calls = 0;
    FakeGmailClient* fake_client = nullptr; // the SAME client the adapter keeps
    const tc::ConsoleConfig config = gmail_config(temp_credentials_path());

    tt::ConsoleTransportSet set = tt::build_console_transports(
        config, loopback_config(), resolver_from({{"agent-1", "tok-1"}}),
        [&]() -> std::unique_ptr<tt::GmailClient> {
            ++factory_calls;
            auto client = std::make_unique<FakeGmailClient>();
            fake_client = client.get();
            return client;
        });

    REQUIRE(factory_calls == 1);
    REQUIRE(set.gmail != nullptr);
    CHECK(set.gmail_enabled);
    REQUIRE(fake_client != nullptr);

    // The TCP transport still behaves identically with the adapter alongside:
    // an agent dials out, authenticates, and answers commands as usual.
    auto& console = static_cast<tt::TcpConsoleTransport&>(*set.tcp);
    console.start();
    REQUIRE(console.listening_port() != 0);
    std::unique_ptr<tt::TcpAgentLink> agent = tt::TcpAgentLink::connect(
        {"127.0.0.1", console.listening_port()}, {"agent-1", "host-1"}, "tok-1",
        loopback_config());
    REQUIRE(handshake_until_established(console, agent, "agent-1"));
    agent->set_command_handler(
        [&](std::uint32_t seq, const tp::Cmd&, const std::function<bool()>&) {
            agent->send_result(seq, tp::ResultText{"stub-responder:exec"});
        });

    // Production shape: the factory wired mail-issued commands into the TCP
    // transport. An admin command mail arrives; pumping the adapter dispatches
    // it into the live session, where the stub agent answers and the reply
    // mail carries the real session seq and the identical result text.
    fake_client->inbox.emplace("m1",
                               command_mail("m1", "admin@example.com", tp::Cmd{"exec", {"ls"}}));
    fake_client->newest_first = {"m1"};

    set.gmail->pump(now_ms()); // poll cycle (caller-supplied monotonic clock
                               // shared with the TCP transport, as production
                               // does): dispatches into the live TCP session
    REQUIRE(console.has_agent("agent-1"));

    // The mail-issued command is the session's single outstanding command at
    // this point (the stub agent has not been pumped yet).
    const auto session = console.session_for("agent-1");
    REQUIRE(session != nullptr);
    const std::vector<std::uint32_t> outstanding = session->outstanding_seqs();
    REQUIRE(outstanding.size() == 1);
    const std::uint32_t mail_seq = outstanding.front();
    pump_until_terminal(console, agent, "agent-1", mail_seq);

    const auto mail_view = session->command(mail_seq);
    REQUIRE(mail_view.has_value());
    REQUIRE_MESSAGE(mail_view->status == tp::CommandStatus::Completed,
                    "piece: " << result_text(*mail_view));
    const std::string delivered = result_text(*mail_view);
    CHECK(delivered == "stub-responder:exec");

    // The result went back as a reply mail carrying the real session seq and
    // the same text - transport equivalence on the production path.
    REQUIRE(fake_client->sent_raw.size() == 1);
    const std::string& result_mail = fake_client->sent_raw[0];
    CHECK(header_value(result_mail, "X-Tether-Seq") == std::to_string(mail_seq));
    CHECK(header_value(result_mail, "To") == "admin@example.com");
    CHECK(decode_mail_text_body(result_mail) == delivered);
}
