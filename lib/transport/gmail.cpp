// lib/transport/gmail.cpp - the Gmail polling adapter body (design D2, tasks
// 4.2-4.3). See gmail.hpp for the seam structure.
//
// Behavioral contracts drawn from specs/command-transport/spec.md:
// - poll at the configured interval exactly; errors back off exponentially
//   with a cap; success restores normal cadence (PollScheduler).
// - act on inbound mail only when the sender is on the configured admin list
//   AND the destination is the adapter account; everything else is ignored
//   without executing or acknowledging anything and cadence is unchanged.
// - accepted admin mail with a well-formed body becomes a protocol v2 command
//   (validated by the protocol layer's own strict payload decoding).
// - results are emailed back to the requesting admin and nobody else: text as
//   body, files as MIME attachments; every result mail carries the command's
//   seq (X-Tether-Seq) so attribution never depends on mail threading.
#include "gmail.hpp"

#include "base64.hpp"
#include "config.hpp"
#include "logging.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <utility>

namespace tether::transport {
namespace {

using common::ConfigError;
using common::Logger;
using nlohmann::json;

// Extracts an address from "Name <addr>" or a bare "addr" header value.
std::string extract_address(const std::string& header_value) {
    const std::size_t open = header_value.find('<');
    const std::size_t close = header_value.find('>');
    if (open != std::string::npos && close != std::string::npos && close > open) {
        return header_value.substr(open + 1, close - open - 1);
    }
    return header_value;
}

std::string trim_ascii(const std::string& text) {
    const char* space = " \t\r\n\f\v";
    const std::size_t first = text.find_first_not_of(space);
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(space);
    return text.substr(first, last - first + 1);
}

std::string to_lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool list_contains_address(const std::string& recipient_list, const std::string& address) {
    // Recipients arrive comma-separated; compare whole addresses
    // case-insensitively so substring coincidences never count as a match.
    std::size_t start = 0;
    while (start <= recipient_list.size()) {
        const std::size_t comma = recipient_list.find(',', start);
        const std::size_t end = comma == std::string::npos ? recipient_list.size() : comma;
        if (to_lower_ascii(trim_ascii(recipient_list.substr(start, end - start))) ==
            to_lower_ascii(address)) {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return false;
}

} // namespace

// ---- credentials -------------------------------------------------------------

GmailCredentials load_gmail_credentials(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw ConfigError(path + ": cannot open credentials file (missing or unreadable)");
    }
    json doc;
    try {
        in >> doc;
    } catch (const json::parse_error& e) {
        throw ConfigError(path + ": malformed JSON credentials (" + e.what() + ")");
    }
    if (!doc.is_object()) {
        throw ConfigError(path + ": credentials root must be a JSON object");
    }
    const auto take_required_string = [&](const char* field) {
        const auto it = doc.find(field);
        if (it == doc.end() || !it->is_string() || it->get<std::string>().empty()) {
            throw ConfigError(path + ": credentials field '" + std::string(field) +
                              "' must be a non-empty string");
        }
        return it->get<std::string>();
    };
    GmailCredentials credentials;
    credentials.account = take_required_string("account");
    credentials.client_id = take_required_string("client_id");
    credentials.client_secret = take_required_string("client_secret");
    credentials.refresh_token = take_required_string("refresh_token");
    return credentials;
}

// ---- poll scheduler (task 4.2 cadence/backoff) --------------------------------

PollScheduler::PollScheduler(std::int64_t interval_ms, std::int64_t backoff_initial_ms,
                             std::int64_t backoff_max_ms)
    : interval_(interval_ms),
      backoff_initial_(backoff_initial_ms),
      backoff_max_(backoff_max_ms) {
    if (interval_ <= 0 || backoff_initial_ <= 0 || backoff_max_ <= 0) {
        throw TransportError("gmail poll durations must be positive");
    }
    if (backoff_max_ < backoff_initial_) {
        throw TransportError("gmail backoff cap must be at least the initial backoff");
    }
}

bool PollScheduler::due(TransportMillis now) const noexcept { return now >= next_due_; }

void PollScheduler::on_cycle(bool success, TransportMillis now) {
    if (success) {
        consecutive_errors_ = 0;
        current_backoff_ = 0;
        next_due_ = now + interval_;
        return;
    }
    if (consecutive_errors_ == 0) {
        current_backoff_ = backoff_initial_;
    } else {
        const std::int64_t doubled = current_backoff_ * 2;
        current_backoff_ = doubled > backoff_max_ ? backoff_max_ : doubled;
    }
    ++consecutive_errors_;
    next_due_ = now + current_backoff_;
}

// ---- MIME rendering -----------------------------------------------------------

namespace {

// Uniqueness only matters within a mailbox poll cycle; a seq-anchored counter
// keeps boundaries deterministic enough for both tests and Gmail.
unsigned mail_boundary_counter() {
    static std::atomic<unsigned> counter{0};
    return ++counter;
}

} // namespace

std::string render_mail(const std::string& reply_to, const std::string& subject,
                        const std::string& seq_header_value, const std::string& body_text,
                        std::vector<MailAttachment> attachments) {
    std::string out;
    out += "To: " + reply_to + "\r\n";
    out += "Subject: " + subject + "\r\n";
    out += "X-Tether-Seq: " + seq_header_value + "\r\n";

    if (attachments.empty()) {
        out += "MIME-Version: 1.0\r\n";
        out += "Content-Type: text/plain; charset=\"UTF-8\"\r\n";
        out += "Content-Transfer-Encoding: base64\r\n";
        out += "\r\n";
        out += common::base64_encode_string(body_text) + "\r\n";
        return out;
    }

    const std::string boundary = "tether-mime-" + std::to_string(mail_boundary_counter());
    out += "MIME-Version: 1.0\r\n";
    out += "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n";
    out += "\r\n";
    out += "This is a MIME multipart message carrying a tether result.\r\n\r\n";
    out += "--" + boundary + "\r\n";
    out += "Content-Type: text/plain; charset=\"UTF-8\"\r\n";
    out += "Content-Transfer-Encoding: base64\r\n\r\n";
    out += common::base64_encode_string(body_text) + "\r\n";
    for (const MailAttachment& attachment : attachments) {
        out += "--" + boundary + "\r\n";
        out += "Content-Type: application/octet-stream; name=\"" + attachment.filename + "\"\r\n";
        out += "Content-Transfer-Encoding: base64\r\n";
        out += "Content-Disposition: attachment; filename=\"" + attachment.filename + "\"\r\n";
        out += "\r\n";
        if (!attachment.bytes.empty()) {
            out += common::base64_encode(attachment.bytes.data(), attachment.bytes.size()) + "\r\n";
        }
    }
    out += "--" + boundary + "--\r\n";
    return out;
}

// ---- GmailTransport ------------------------------------------------------------

GmailTransport::GmailTransport(std::shared_ptr<GmailClient> client, GmailTransportOptions options,
                               CommandDispatcher dispatch)
    : client_(std::move(client)),
      options_(std::move(options)),
      dispatch_(std::move(dispatch)),
      scheduler_(options_.poll_interval_ms, options_.backoff_initial_ms, options_.backoff_max_ms) {
    if (!client_) {
        throw TransportError("gmail transport requires a GmailClient");
    }
    if (options_.admin_list.empty()) {
        throw TransportError("gmail transport requires a non-empty admin list");
    }
    for (const std::string& admin : options_.admin_list) {
        if (admin.empty()) {
            throw TransportError("gmail admin list entries must be non-empty addresses");
        }
    }
    if (options_.adapter_account.empty() || options_.default_agent.empty()) {
        throw TransportError(
            "gmail transport requires the adapter account and the default agent path");
    }
    if (!dispatch_) {
        throw TransportError("gmail transport requires a command dispatcher");
    }
}

bool GmailTransport::accept_message(const MailMessage& message,
                                    const GmailTransportOptions& options) {
    const std::string sender = to_lower_ascii(extract_address(message.from));
    const bool sender_is_admin =
        std::any_of(options.admin_list.begin(), options.admin_list.end(),
                    [&](const std::string& admin) { return sender == to_lower_ascii(admin); });
    if (!sender_is_admin) {
        return false;
    }
    // Destination rule: the adapter account owns its inbox - the mail must
    // name exactly that address among its recipients.
    return list_contains_address(message.to, options.adapter_account);
}

std::pair<std::string, protocol::Cmd> GmailTransport::parse_command_mail(
    const MailMessage& message) {
    // Body: base64-encoded command JSON by default (MIME-safe); plain JSON is
    // tolerated for hand-pasted mail (flagged tolerant branch in the report).
    std::string body = trim_ascii(message.body_text);
    if (body.rfind('{', 0) != 0) {
        body = common::base64_decode_string(body);
    }
    json doc;
    try {
        doc = json::parse(body);
    } catch (const json::parse_error& e) {
        throw TransportError("gmail command mail body is not valid command JSON: " +
                             std::string(e.what()));
    }
    std::string agent = options_.default_agent;
    if (doc.is_object() && doc.contains("agent") && doc.at("agent").is_string()) {
        agent = doc.at("agent").get<std::string>();
        (void)doc.erase("agent");
    }
    if (agent.empty()) {
        throw TransportError("gmail command mail carries an empty agent path");
    }
    // Hand the remaining shape to the protocol layer's strict per-type
    // validation (fields, types, unknown-field rejection all live there). The
    // possibly-scrubbed document is what gets validated: an agent selector of
    // the wrong type stays in, and strict decoding rejects it.
    const std::string validated_body = doc.dump();
    const std::vector<unsigned char> payload(validated_body.begin(), validated_body.end());
    const protocol::Message decoded = protocol::decode_payload(protocol::MsgType::Cmd, payload);
    return {agent, std::get<protocol::Cmd>(decoded)};
}

void GmailTransport::send_result_mail(std::uint32_t seq, const Accumulator& results) {
    const std::string seq_value = std::to_string(seq);
    const std::string subject = "[tether seq " + seq_value + "] " + results.subject;

    std::string mime;
    if (results.file_seen) {
        // File result: MIME multipart/mixed with the base64 attachment, named
        // by the FILE_META (or a placeholder when the stream never named one).
        std::vector<MailAttachment> attachments;
        attachments.push_back(
            MailAttachment{results.meta_name.empty() ? "result.bin" : results.meta_name,
                           results.file_bytes});
        mime =
            render_mail(results.admin, subject, seq_value, "See attached result.", attachments);
    } else {
        // Text result: plain body, replying to the requesting admin alone.
        mime = render_mail(results.admin, subject, seq_value, results.text_body, {});
    }
    client_->send_raw(mime);
    Logger::instance().info("gmail result delivered", {{"seq", seq_value}, {"to", results.admin}});
}

void GmailTransport::poll_once(TransportMillis now) {
    std::vector<std::string> ids = client_->list_inbox_ids(50);

    // API order is most-recent-first; issue commands oldest-first so the mail
    // order equals the command order at the agent.
    std::reverse(ids.begin(), ids.end());

    for (const std::string& id : ids) {
        if (processed_ids_.count(id) != 0) {
            continue;
        }
        MailMessage message;
        try {
            message = client_->fetch(id);
        } catch (const std::exception& e) {
            // Unseen id stays unseen: the next (backed-off) cycle retries it.
            Logger::instance().warn("gmail fetch failed", {{"id", id}, {"error", e.what()}});
            throw; // caller schedules the backoff
        }
        processed_ids_.insert(id); // processed-or-ignored either way; never refetched

        if (!accept_message(message, options_)) {
            // Spec: foreign mail is ignored harmlessly - no execution, no
            // acknowledgement, cadence untouched.
            Logger::instance().debug("gmail mail ignored by acceptance rules",
                                     {{"id", id}, {"from", message.from}});
            continue;
        }

        std::pair<std::string, protocol::Cmd> parsed;
        try {
            parsed = parse_command_mail(message);
        } catch (const std::exception& e) {
            // An accepted sender with a malformed command gets a rejection
            // notice instead of an execution attempt.
            Logger::instance().warn("gmail command mail malformed",
                                    {{"id", id}, {"error", e.what()}});
            try {
                client_->send_raw(
                    render_mail(extract_address(message.from),
                                "[gmail command rejected] " + message.subject, "0",
                                "ERR bad_command: " + std::string(e.what()), {}));
            } catch (const std::exception& send_error) {
                Logger::instance().warn("gmail rejection notice could not be sent",
                                        {{"id", id}, {"error", send_error.what()}});
            }
            continue;
        }

        auto results = std::make_shared<Accumulator>();
        results->admin = extract_address(message.from);
        results->subject = message.subject.empty() ? "command" : message.subject;

        // The hook is owned by the dispatcher call result lifetime: late
        // deliveries (results arriving after the dispatcher returned, or after
        // later commands) still route through this mail's own accumulator, so
        // seq attribution never swaps between command mails.
        protocol::ResultHook hook = [this, results](std::uint32_t seq,
                                                    const protocol::Message& piece,
                                                    bool terminal) {
            if (std::holds_alternative<protocol::FileMeta>(piece)) {
                results->file_seen = true;
                results->meta_name = std::get<protocol::FileMeta>(piece).name;
            } else if (std::holds_alternative<protocol::FileChunk>(piece)) {
                results->file_seen = true;
                const std::vector<unsigned char>& bytes = std::get<protocol::FileChunk>(piece).bytes;
                results->file_bytes.insert(results->file_bytes.end(), bytes.begin(), bytes.end());
            } else if (std::holds_alternative<protocol::FileEnd>(piece)) {
                results->file_seen = true;
                send_result_mail(seq, *results);
            } else if (std::holds_alternative<protocol::ResultText>(piece)) {
                results->text_body = std::get<protocol::ResultText>(piece).text;
                if (terminal) {
                    send_result_mail(seq, *results);
                }
            } else if (std::holds_alternative<protocol::Err>(piece)) {
                const protocol::Err& error = std::get<protocol::Err>(piece);
                results->text_body = "ERR " + error.code + ": " + error.message;
                if (terminal) {
                    send_result_mail(seq, *results);
                }
            } else {
                Logger::instance().warn("gmail transport saw a result piece it ignores",
                                        {{"seq", std::to_string(seq)}});
            }
        };

        try {
            const std::uint32_t seq = dispatch_(parsed.first, parsed.second, hook, now);
            (void)seq; // the result mail's X-Tether-Seq comes from the hooks
        } catch (const std::exception& e) {
            // The command was never issued: reply with the failure so the
            // requesting admin learns the command was rejected (nothing ran).
            Logger::instance().warn("gmail command dispatch rejected",
                                    {{"agent", parsed.first}, {"error", e.what()}});
            try {
                client_->send_raw(render_mail(results->admin,
                                              "[gmail command rejected] " + results->subject, "0",
                                              "ERR dispatch_failed: " + std::string(e.what()), {}));
            } catch (const std::exception& send_error) {
                Logger::instance().warn("gmail rejection notice could not be sent",
                                        {{"id", id}, {"error", send_error.what()}});
            }
        }
    }
}

void GmailTransport::pump(TransportMillis now) {
    if (!scheduler_.due(now)) {
        return;
    }
    bool ok = true;
    try {
        poll_once(now);
    } catch (const std::exception& e) {
        ok = false;
        Logger::instance().warn("gmail poll failed; backing off", {{"error", e.what()}});
    }
    scheduler_.on_cycle(ok, now);
}

std::uint32_t GmailTransport::issue(const std::string&, const protocol::Cmd&,
                                    const protocol::ResultHook&, TransportMillis) {
    throw TransportError(
        "gmail transport does not issue commands programmatically; commands arrive "
        "over its inbox and are routed at poll time (D2 mail-issuance seam)");
}

} // namespace tether::transport
