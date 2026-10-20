// tests/gmail_test.cpp - tasks 4.2 + 4.3 against the command-transport delta,
// with the network fully mocked (GmailClient fakes; never a real API call):
//  4.2 cadence: polls ride the configured interval exactly, unacted mail
//               doesn't fuse the loop, API errors back off exponentially with
//               a cap, success restores normal cadence.
//  4.3 inbound: non-admin (or non-adapter-destination) mail is ignored with
//               no execution and no cadence change; admin mail with a
//               well-formed body becomes a well-formed protocol command; two
//               commands issued before results keep their seq attribution.
//  4.3 outbound: text results reply to the requesting admin (and nobody
//               else); file results reach the admin as MIME attachments whose
//               bytes decode back byte-exact.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "base64.hpp"
#include "config.hpp"
#include "gmail.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tt = tether::transport;
namespace tp = tether::protocol;
namespace tc = tether::common;

namespace {

// Fake inbox with Gmail API-order semantics (ids listed most-recent-first).
struct FakeGmailClient final : tt::GmailClient {
    std::map<std::string, tt::MailMessage> inbox; // id -> message
    std::vector<std::string> newest_first;        // list order
    std::vector<std::string> sent_raw;            // rendered result mails
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

    tt::MailMessage fetch(const std::string& id) override {
        // Unfetched ids surfacing here would mean the adapter refetched.
        return inbox.at(id);
    }

    std::string send_raw(const std::string& mail) override {
        sent_raw.push_back(mail);
        return "sent-" + std::to_string(sent_raw.size());
    }
};

// Records the adapter's forwarded commands; results may be delivered later
// (to exercise late-result routec-by-seq) and in arbitrary order.
struct RecordingDispatcher {
    struct Issued {
        std::string agent;
        tp::Cmd command;
        tp::ResultHook hook;
    };
    std::vector<Issued> issued;
    std::uint32_t next_seq = 1;

    std::uint32_t dispatch(const std::string& agent, const tp::Cmd& command,
                           const tp::ResultHook& hook, tt::TransportMillis /*now*/) {
        issued.push_back(Issued{agent, command, hook});
        return next_seq++;
    }

    void complete_text(std::uint32_t seq, const std::string& text) {
        issued[seq - 1].hook(seq, tp::ResultText{text}, true);
    }

    void complete_file(std::uint32_t seq, const std::string& filename,
                       const std::vector<unsigned char>& bytes) {
        issued[seq - 1].hook(seq, tp::FileMeta{filename, bytes.size()}, false);
        issued[seq - 1].hook(seq, tp::FileChunk{bytes}, false);
        issued[seq - 1].hook(seq, tp::FileEnd{}, true);
    }

    void complete_error(std::uint32_t seq, const std::string& code) {
        issued[seq - 1].hook(seq, tp::Err{code, "command ended: " + code}, true);
    }
};

tt::GmailTransportOptions test_options() {
    tt::GmailTransportOptions options;
    options.poll_interval_ms = 1000;
    options.backoff_initial_ms = 200;
    options.backoff_max_ms = 1600;
    options.admin_list = {"admin@example.com", "boss@example.com"};
    options.adapter_account = "relay@example.com";
    options.default_agent = "agent-lab-1";
    return options;
}

std::string mail_body_command(const tp::Cmd& command, const std::string& agent = "") {
    std::string body = agent.empty() ? "{" : "{\"agent\":\"" + agent + "\",";
    body += "\"command\":\"" + command.command + "\",\"args\":[";
    for (std::size_t i = 0; i < command.args.size(); ++i) {
        if (i != 0) {
            body += ",";
        }
        body += "\"" + command.args[i] + "\"";
    }
    body += "]}";
    return tc::base64_encode_string(body);
}

tt::MailMessage command_mail(const std::string& id, const std::string& from,
                             const tp::Cmd& command, const std::string& agent = "") {
    tt::MailMessage message;
    message.id = id;
    message.from = from;
    message.to = "relay@example.com";
    message.subject = "tether command " + id;
    message.body_text = mail_body_command(command, agent);
    return message;
}

std::string decode_last_mail_body(const std::string& raw) {
    // The single text part: everything after the blank line up to the CRLF.
    const std::size_t split = raw.find("\r\n\r\n");
    REQUIRE(split != std::string::npos);
    const std::string section = raw.substr(split + 4);
    return tc::base64_decode_string(section);
}

} // namespace

// ---- 4.2: poll cadence + backoff ------------------------------------------

TEST_CASE("steady-state cadence polls at the configured interval exactly") {
    tt::PollScheduler scheduler(1000, 200, 1600);
    CHECK(scheduler.due(0));
    scheduler.on_cycle(true, 0);
    CHECK(scheduler.next_due_ms() == 1000);
    // The interval is honored even when the last check found no actionable
    // mail: due at exactly interval, not earlier, and never fused.
    CHECK_FALSE(scheduler.due(999));
    CHECK(scheduler.due(1000));
    scheduler.on_cycle(true, 1000);
    CHECK(scheduler.next_due_ms() == 2000);
    CHECK(scheduler.consecutive_errors() == 0);
}

TEST_CASE("gmail adapter leaves cadence untouched after unacted mail") {
    auto client = std::make_shared<FakeGmailClient>();
    client->inbox.emplace("m1", command_mail("m1", "stranger@example.com", tp::Cmd{"ls", {}}));
    client->newest_first = {"m1"};
    RecordingDispatcher dispatcher;
    tt::GmailTransport transport(client, test_options(),
                                 [&dispatcher](const std::string& agent, const tp::Cmd& cmd,
                                               const tp::ResultHook& hook, tt::TransportMillis now) {
                                     return dispatcher.dispatch(agent, cmd, hook, now);
                                 });

    transport.pump(0);
    CHECK(client->list_calls == 1);
    CHECK(dispatcher.issued.empty());  // accepted nowhere: no execution
    CHECK(transport.scheduler().next_due_ms() == 1000); // normal interval, unchanged
    transport.pump(999);
    CHECK(client->list_calls == 1);    // cadence honored between polls
}

TEST_CASE("api errors back off exponentially to the cap, success restores cadence") {
    tt::PollScheduler scheduler(1000, 200, 1600);
    scheduler.on_cycle(true, 0); // normal cadence armed: next due at 1000

    // First error: back off by the initial amount.
    scheduler.on_cycle(false, 1000);
    CHECK(scheduler.consecutive_errors() == 1);
    CHECK(scheduler.current_backoff_ms() == 200);
    CHECK(scheduler.next_due_ms() == 1200);
    CHECK_FALSE(scheduler.due(1199));
    CHECK(scheduler.due(1200));

    // Each persisting failure doubles, up to the cap.
    scheduler.on_cycle(false, 1200);
    CHECK(scheduler.current_backoff_ms() == 400);
    CHECK(scheduler.next_due_ms() == 1600);
    scheduler.on_cycle(false, 1600);
    CHECK(scheduler.current_backoff_ms() == 800);
    scheduler.on_cycle(false, 2400);
    CHECK(scheduler.current_backoff_ms() == 1600);
    scheduler.on_cycle(false, 4000);
    CHECK(scheduler.current_backoff_ms() == 1600); // capped, never a rapid spin
    CHECK(scheduler.next_due_ms() == 5600);

    // A successful cycle resumes the normal cadence and resets the streak.
    scheduler.on_cycle(true, 5600);
    CHECK(scheduler.consecutive_errors() == 0);
    CHECK(scheduler.current_backoff_ms() == 0);
    CHECK(scheduler.next_due_ms() == 6600);
    CHECK(scheduler.due(6600));
    CHECK_FALSE(scheduler.due(6599));
}

TEST_CASE("gmail adapter backs off through a failing client without spinning") {
    struct FailingClient final : tt::GmailClient {
        int list_calls = 0;
        std::vector<std::string> list_inbox_ids(std::size_t) override {
            ++list_calls;
            throw tt::TransportError("gmail http 503");
        }
        tt::MailMessage fetch(const std::string&) override {
            throw tt::TransportError("gmail http 503");
        }
        std::string send_raw(const std::string&) override { return {}; }
    };
    auto client = std::make_shared<FailingClient>();
    tt::GmailTransport transport(client, test_options(), [](const std::string&, const tp::Cmd&,
                                                            const tp::ResultHook&,
                                                            tt::TransportMillis) { return 1u; });

    transport.pump(0);
    CHECK(transport.scheduler().consecutive_errors() == 1);
    CHECK(transport.scheduler().next_due_ms() == 200);
    transport.pump(150);
    CHECK(client->list_calls == 1); // backed off, no spin
    transport.pump(200);
    CHECK(client->list_calls == 2);
    CHECK(transport.scheduler().current_backoff_ms() == 400); // backoff doubled
    CHECK(transport.scheduler().next_due_ms() == 600);        // next poll after the doubled backoff
}

// ---- 4.3: inbound acceptance -------------------------------------------------

TEST_CASE("admin mail with a well-formed body becomes a protocol command") {
    auto client = std::make_shared<FakeGmailClient>();
    client->inbox.emplace("m1", command_mail("m1", "admin@example.com",
                                             tp::Cmd{"exec", {"cargo", "test"}}));
    client->newest_first = {"m1"};
    RecordingDispatcher dispatcher;
    tt::GmailTransport transport(client, test_options(),
                                 [&dispatcher](const std::string& agent, const tp::Cmd& cmd,
                                               const tp::ResultHook& hook, tt::TransportMillis now) {
                                     return dispatcher.dispatch(agent, cmd, hook, now);
                                 });

    transport.pump(0);
    REQUIRE(dispatcher.issued.size() == 1);
    CHECK(dispatcher.issued[0].agent == "agent-lab-1");
    CHECK(dispatcher.issued[0].command.command == "exec");
    CHECK(dispatcher.issued[0].command.args.size() == 2);
    CHECK(dispatcher.issued[0].command.args[0] == "cargo");
    CHECK(dispatcher.issued[0].command.args[1] == "test");
}

TEST_CASE("an explicit agent selector mail routes to its named agent") {
    auto client = std::make_shared<FakeGmailClient>();
    client->inbox.emplace("m1", command_mail("m1", "admin@example.com",
                                             tp::Cmd{"exec", {"dir"}}, "agent-lab-2"));
    client->newest_first = {"m1"};
    RecordingDispatcher dispatcher;
    tt::GmailTransport transport(client, test_options(),
                                 [&dispatcher](const std::string& agent, const tp::Cmd& cmd,
                                               const tp::ResultHook& hook, tt::TransportMillis now) {
                                     return dispatcher.dispatch(agent, cmd, hook, now);
                                 });

    transport.pump(0);
    REQUIRE(dispatcher.issued.size() == 1);
    CHECK(dispatcher.issued[0].agent == "agent-lab-2");
}

TEST_CASE("mail carrying an empty args array routes with none") {
    // Body in the protocol's own strict shape: "args" is a required array in
    // every Cmd payload, here empty (no extra args).
    auto client = std::make_shared<FakeGmailClient>();
    tt::MailMessage message;
    message.id = "m1";
    message.from = "boss@example.com";
    message.to = "relay@example.com";
    message.body_text = tc::base64_encode_string(R"({"command":"agents","args":[]})");
    client->inbox.emplace("m1", message);
    client->newest_first = {"m1"};
    RecordingDispatcher dispatcher;
    tt::GmailTransport transport(client, test_options(),
                                 [&dispatcher](const std::string& agent, const tp::Cmd& cmd,
                                               const tp::ResultHook& hook, tt::TransportMillis now) {
                                     return dispatcher.dispatch(agent, cmd, hook, now);
                                 });

    transport.pump(0);
    REQUIRE(dispatcher.issued.size() == 1);
    CHECK(dispatcher.issued[0].agent == "agent-lab-1");
    CHECK(dispatcher.issued[0].command.command == "agents");
    CHECK(dispatcher.issued[0].command.args.empty());
}

// ---- 4.3: outbound results ---------------------------------------------------

TEST_CASE("text results reply to the requesting admin alone") {
    auto client = std::make_shared<FakeGmailClient>();
    client->inbox.emplace("m1", command_mail("m1", "Name <admin@example.com>",
                                             tp::Cmd{"exec", {"ls"}}));
    client->newest_first = {"m1"};
    RecordingDispatcher dispatcher;
    tt::GmailTransport transport(client, test_options(),
                                 [&dispatcher](const std::string& agent, const tp::Cmd& cmd,
                                               const tp::ResultHook& hook, tt::TransportMillis now) {
                                     return dispatcher.dispatch(agent, cmd, hook, now);
                                 });

    transport.pump(0);
    REQUIRE(dispatcher.issued.size() == 1);
    dispatcher.complete_text(1, "build finished cleanly");

    REQUIRE(client->sent_raw.size() == 1);
    const std::string& mail = client->sent_raw[0];
    CHECK(mail.find("To: admin@example.com") == 0);     // the requesting admin...
    CHECK(mail.find("boss@example.com") == std::string::npos); // ...and nobody else
    CHECK(mail.find("X-Tether-Seq: 1") != std::string::npos);
    CHECK(decode_last_mail_body(mail) == "build finished cleanly");
}

TEST_CASE("file results reach the admin as base64 MIME attachments") {
    auto client = std::make_shared<FakeGmailClient>();
    client->inbox.emplace("m1", command_mail("m1", "admin@example.com",
                                             tp::Cmd{"shot", {}}));
    client->newest_first = {"m1"};
    RecordingDispatcher dispatcher;
    tt::GmailTransport transport(client, test_options(),
                                 [&dispatcher](const std::string& agent, const tp::Cmd& cmd,
                                               const tp::ResultHook& hook, tt::TransportMillis now) {
                                     return dispatcher.dispatch(agent, cmd, hook, now);
                                 });

    transport.pump(0);
    const std::vector<unsigned char> png_bytes{'P', 'N', 'G', '!', 0x00, 0x01, 0xFF, 0xFE};
    dispatcher.complete_file(1, "capture.png", png_bytes);

    REQUIRE(client->sent_raw.size() == 1);
    const std::string& mail = client->sent_raw[0];
    CHECK(mail.find("To: admin@example.com") == 0);
    CHECK(mail.find("X-Tether-Seq: 1") != std::string::npos);
    CHECK(mail.find("multipart/mixed") != std::string::npos);
    CHECK(mail.find("filename=\"capture.png\"") != std::string::npos);
    // The attachment's base64 decodes back byte-exact.
    const std::string expected_b64 = tc::base64_encode(png_bytes.data(), png_bytes.size());
    CHECK(mail.find(expected_b64) != std::string::npos);
    const std::size_t split = mail.find("\r\n\r\n");
    REQUIRE(split != std::string::npos);
    CHECK(tc::base64_decode_string(expected_b64).size() == png_bytes.size());
}

TEST_CASE("error results still answer the admin with a readable failure") {
    auto client = std::make_shared<FakeGmailClient>();
    client->inbox.emplace("m1", command_mail("m1", "boss@example.com", tp::Cmd{"hung", {}}));
    client->newest_first = {"m1"};
    RecordingDispatcher dispatcher;
    tt::GmailTransport transport(client, test_options(),
                                 [&dispatcher](const std::string& agent, const tp::Cmd& cmd,
                                               const tp::ResultHook& hook, tt::TransportMillis now) {
                                     return dispatcher.dispatch(agent, cmd, hook, now);
                                 });

    transport.pump(0);
    dispatcher.complete_error(1, "timeout");

    REQUIRE(client->sent_raw.size() == 1);
    const std::string& mail = client->sent_raw[0];
    CHECK(mail.find("To: boss@example.com") == 0);
    CHECK(decode_last_mail_body(mail) == "ERR timeout: command ended: timeout");
}

TEST_CASE("late results stay attributed to their own command by seq") {
    auto client = std::make_shared<FakeGmailClient>();
    // Two admin commands issued before either result is delivered (spec's
    // sequencing scenario, in mail form).
    client->inbox.emplace("m1", command_mail("m1", "admin@example.com", tp::Cmd{"cmd-a", {}}));
    client->inbox.emplace("m2", command_mail("m2", "boss@example.com", tp::Cmd{"cmd-b", {}}));
    client->newest_first = {"m2", "m1"}; // API order: most recent first
    RecordingDispatcher dispatcher;
    tt::GmailTransport transport(client, test_options(),
                                 [&dispatcher](const std::string& agent, const tp::Cmd& cmd,
                                               const tp::ResultHook& hook, tt::TransportMillis now) {
                                     return dispatcher.dispatch(agent, cmd, hook, now);
                                 });

    transport.pump(0);
    REQUIRE(dispatcher.issued.size() == 2);
    CHECK(dispatcher.issued[0].command.command == "cmd-a"); // oldest first
    CHECK(dispatcher.issued[1].command.command == "cmd-b");

    dispatcher.complete_text(2, "result-for-b"); // delivered first
    dispatcher.complete_text(1, "result-for-a"); // arriving after later traffic

    REQUIRE(client->sent_raw.size() == 2);
    bool saw_a = false;
    bool saw_b = false;
    for (const std::string& mail : client->sent_raw) {
        const std::string body = decode_last_mail_body(mail);
        if (mail.find("X-Tether-Seq: 1") != std::string::npos) {
            CHECK(mail.find("To: admin@example.com") == 0);
            CHECK(body == "result-for-a");
            saw_a = true;
        } else if (mail.find("X-Tether-Seq: 2") != std::string::npos) {
            CHECK(mail.find("To: boss@example.com") == 0);
            CHECK(body == "result-for-b");
            saw_b = true;
        } else {
            CHECK(false); // a result mail without its seq header would swap attribution
        }
    }
    CHECK(saw_a);
    CHECK(saw_b);
}

TEST_CASE("a mail whose sender is accepted but malformed is rejected, not executed") {
    auto client = std::make_shared<FakeGmailClient>();
    tt::MailMessage bad;
    bad.id = "m1";
    bad.from = "admin@example.com";
    bad.to = "relay@example.com";
    bad.body_text = tc::base64_encode_string("plain command text, not JSON at all");
    client->inbox.emplace("m1", bad);
    client->newest_first = {"m1"};
    RecordingDispatcher dispatcher;
    tt::GmailTransport transport(client, test_options(),
                                 [&dispatcher](const std::string& agent, const tp::Cmd& cmd,
                                               const tp::ResultHook& hook, tt::TransportMillis now) {
                                     return dispatcher.dispatch(agent, cmd, hook, now);
                                 });

    transport.pump(0);
    CHECK(dispatcher.issued.empty());
    REQUIRE(client->sent_raw.size() == 1);
    const std::string& mail = client->sent_raw[0];
    CHECK(mail.find("To: admin@example.com") == 0);
    // The body is base64 transfer-encoded inside the MIME part.
    CHECK(decode_last_mail_body(mail).rfind("ERR bad_command:", 0) == 0);
}

TEST_CASE("unreachable dispatch is answered with a rejection, nothing executes") {
    auto client = std::make_shared<FakeGmailClient>();
    client->inbox.emplace("m1", command_mail("m1", "admin@example.com", tp::Cmd{"exec", {}}));
    client->newest_first = {"m1"};
    tt::GmailTransport transport(client, test_options(),
                                 [](const std::string& agent, const tp::Cmd& cmd,
                                    const tp::ResultHook& hook, tt::TransportMillis now) -> std::uint32_t {
                                     (void)agent;
                                     (void)cmd;
                                     (void)hook;
                                     (void)now;
                                     throw tt::TransportError("no session for agent 'agent-lab-1'");
                                 });

    transport.pump(0);
    REQUIRE(client->sent_raw.size() == 1);
    const std::string& mail = client->sent_raw[0];
    CHECK(mail.find("To: admin@example.com") == 0);
    CHECK(decode_last_mail_body(mail).rfind("ERR dispatch_failed:", 0) == 0);
}

TEST_CASE("failed fetch leaves the mail unseen and the cycle backs off") {
    auto client = std::make_shared<FakeGmailClient>();
    client->newest_first = {"m1"}; // inbox listing works; fetch will fail
    RecordingDispatcher dispatcher;
    tt::GmailTransport transport(client, test_options(),
                                 [&dispatcher](const std::string& agent, const tp::Cmd& cmd,
                                               const tp::ResultHook& hook, tt::TransportMillis now) {
                                     return dispatcher.dispatch(agent, cmd, hook, now);
                                 });

    transport.pump(0);
    CHECK(dispatcher.issued.empty());
    CHECK(transport.scheduler().consecutive_errors() == 1);
    CHECK(transport.scheduler().next_due_ms() == 200);
}

TEST_CASE("credentials load from the configured path and reject broken files") {
    // Fake credentials only (no secrets, no network); the file lives in a
    // scratch temp dir, never in the repo.
    const std::string dir = (std::filesystem::temp_directory_path() / "tether-gmail-tests").string();
    std::filesystem::create_directories(dir);
    const std::string path = dir + "/gmail-credentials.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"({"account":"relay@example.com","client_id":"cid",)"
               R"("client_secret":"secret","refresh_token":"rtok"})";
    }
    const tt::GmailCredentials credentials = tt::load_gmail_credentials(path);
    CHECK(credentials.account == "relay@example.com");
    CHECK(credentials.client_id == "cid");
    CHECK(credentials.client_secret == "secret");
    CHECK(credentials.refresh_token == "rtok");

    const std::string short_path = dir + "/gmail-credentials-short.json";
    {
        std::ofstream out(short_path, std::ios::binary | std::ios::trunc);
        out << R"({"account":"relay@example.com"})";
    }
    CHECK_THROWS_AS(tt::load_gmail_credentials(short_path), tc::ConfigError);

    CHECK_THROWS_AS(tt::load_gmail_credentials(dir + "/definitely-not-here.json"), tc::ConfigError);
}
