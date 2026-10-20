// lib/transport/gmail.hpp - the Gmail polling adapter (design D2, tasks
// 4.2-4.3): the deliberate slow-path transport kept as the coursework side.
//
// Layout (the network seam is the point of this module):
//   HttpClient            - abstract seam; the ONLY cpr consumer lives in a
//                           concrete class behind it (gmail_cpr.cpp). CI tests
//                           never touch the network: they inject fakes.
//   load_gmail_credentials- OAuth refresh-token credentials read from the
//                           configured path OUTSIDE the repo (no secrets in tree).
//   GoogleGmailClient     - OAuth token refresh + Gmail API list/fetch/send.
//   PollScheduler         - poll cadence honor + exponential error backoff
//                           (fake-clock testable, spec: bounded interval, no
//                           spinning on API errors).
//   render_mail           - MIME builders; result emails reply to the
//                           requesting admin, file results as base64 MIME
//                           attachments.
//   GmailTransport        - the ITransport implementation: inbox polls accept
//                           admin-sender mail addressed to the adapter
//                           account, translate it into a protocol command
//                           (via the shared dispatcher), and deliver results
//                           back as email.
#pragma once

#include "transport.hpp"

#include "payloads.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tether::transport {

// ---- network seam (all real I/O hides behind these) --------------------------

// Minimal HTTP operations the Gmail paths need. Every method raises
// TransportError on a non-2xx response (message carries the status), so
// callers reason in one error type.
class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual std::string post_form(const std::string& url,
                                  const std::vector<std::pair<std::string, std::string>>& form) = 0;
    virtual std::string get(const std::string& url,
                            const std::vector<std::pair<std::string, std::string>>& headers) = 0;
    virtual std::string post_json(const std::string& url,
                                  const std::vector<std::pair<std::string, std::string>>& headers,
                                  const std::string& json_body) = 0;
};

// ---- credentials (design D8: secrets never live in the repo) -----------------

// OAuth refresh-token credentials + the adapter's own mailbox identity.
// Loaded from the path named in the console config (outside the repo).
struct GmailCredentials {
    std::string account;       // the adapter account the service authenticates as
    std::string client_id;
    std::string client_secret;
    std::string refresh_token;
};

// Reads {account, client_id, client_secret, refresh_token} from `path`
// (JSON); every failure is an explicit ConfigError naming the file.
GmailCredentials load_gmail_credentials(const std::string& path);


// ---- Gmail client contract (fakes implement this in tests) -------------------

struct MailMessage {
    std::string id;
    std::string from;      // "Name <addr>" or bare addr
    std::string to;        // raw To header value
    std::string subject;
    std::string body_text; // decoded text/plain payload (base64url in the API)
};

struct MailAttachment {
    std::string filename;
    std::vector<unsigned char> bytes;
};

class GmailClient {
public:
    virtual ~GmailClient() = default;

    // Inbox message ids, "API order" = most recent first (Gmail semantics).
    // The adapter processes them oldest-first so mail issuance order is
    // preserved regardless of the polling cadence.
    virtual std::vector<std::string> list_inbox_ids(std::size_t max_results) = 0;

    virtual MailMessage fetch(const std::string& id) = 0;

    // Sends a fully rendered RFC 822 message (render_mail output); returns
    // the new message's id.
    virtual std::string send_raw(const std::string& mime) = 0;
};

// The concrete GmailClient: OAuth refresh-token flow + Gmail API v1 users.me
// endpoints. The only translation unit that touches cpr is gmail_cpr.cpp
// (HttpClient seam implementation + this client).
std::unique_ptr<GmailClient> make_google_gmail_client(std::shared_ptr<HttpClient> http,
                                                      GmailCredentials credentials);

std::unique_ptr<HttpClient> make_curl_http_client();

// ---- poll cadence (fake-clock testable; tasks 4.2 spec scenarios) ------------

// Poll an inbox every `interval_ms` exactly; after an API error, back off for
// initial, doubled-with-cap durations while errors persist; a successful cycle
// restores the normal cadence. No actionable mail is NOT an error: the
// interval marches on (spec: polling load bounded regardless of inbox state).
class PollScheduler {
public:
    PollScheduler(std::int64_t interval_ms, std::int64_t backoff_initial_ms,
                  std::int64_t backoff_max_ms);

    bool due(TransportMillis now) const noexcept;
    void on_cycle(bool success, TransportMillis now);

    TransportMillis next_due_ms() const noexcept { return next_due_; }
    std::int64_t consecutive_errors() const noexcept { return consecutive_errors_; }
    std::int64_t current_backoff_ms() const noexcept { return current_backoff_; }

private:
    std::int64_t interval_;
    std::int64_t backoff_initial_;
    std::int64_t backoff_max_;
    TransportMillis next_due_ = 0; // due immediately at t=0 (startup check)
    std::int64_t consecutive_errors_ = 0;
    std::int64_t current_backoff_ = 0;
};

// ---- MIME rendering (task 4.3 outbound half) ---------------------------------

// Renders a complete RFC 822 message. Without attachments: a single
// text/plain part (base64 transfer-encoded). With attachments:
// multipart/mixed whose file parts are application/octet-stream, base64
// body = text (Rendered content-transfer-encoded), attachments after it.
// Every result mail carries the command's seq in `X-Tether-Seq` so results
// stay attributed to their originating command mail regardless of arrival
// order, plus delivery to exactly `reply_to` (the requesting admin).
std::string render_mail(const std::string& reply_to, const std::string& subject,
                        const std::string& seq_header_value, const std::string& body_text,
                        std::vector<MailAttachment> attachments);

// ---- the adapter transport ---------------------------------------------------

struct GmailTransportOptions {
    std::int64_t poll_interval_ms = 0;   // > 0
    std::int64_t backoff_initial_ms = 0; // > 0
    std::int64_t backoff_max_ms = 0;     // >= backoff_initial_ms

    std::vector<std::string> admin_list; // sender accept rule (task 4.3)
    std::string adapter_account;         // destination accept rule (owned inbox)
    std::string default_agent;           // "the configured agent path" when a
                                         // mail omits its own agent selector
};

// Mail-issued commands are routed behind the SAME seam as TCP deliveries:
// the console's command router (typically the TcpConsoleTransport) receives
// (agent, cmd) and returns the protocol seq used to correlate results.
using CommandDispatcher =
    std::function<std::uint32_t(const std::string& agent, const protocol::Cmd& command,
                                const protocol::ResultHook& hook, TransportMillis now)>;

class GmailTransport final : public ITransport {
public:
    GmailTransport(std::shared_ptr<GmailClient> client, GmailTransportOptions options,
                   CommandDispatcher dispatch);

    // One cadence-bound inbox cycle: when the scheduler says due, list + fetch
    // + accept + (accepted and well-formed -> issue via dispatcher; results are
    // emailed back to the requesting admin). API errors back off (scheduler).
    void pump(TransportMillis now) override;

    std::uint32_t issue(const std::string& agent, const protocol::Cmd& command,
                        const protocol::ResultHook& hook, TransportMillis now) override;

    // Inspectable cadence state (tests assert exact due times / error counts).
    const PollScheduler& scheduler() const noexcept { return scheduler_; }

    // One accumulation snapshot per in-flight mail-issued command: the
    // requesting admin (reply target), the command mail's subject (threading),
    // the streamed file bytes if FILE_META/FILE_CHUNK pieces arrive, or the
    // text of a terminal RESULT_TEXT/ERR otherwise. Lives on a shared_ptr
    // owned by the per-mail result hook so late (post-dispatch) result
    // deliveries never dangle.
    struct Accumulator {
        std::string admin;
        std::string subject;
        bool file_seen = false;
        std::string meta_name;
        std::vector<unsigned char> file_bytes;
        std::string text_body;
    };

private:
    // Acceptance rule (spec): sender on the admin list AND the adapter account
    // owns the destination inbox. Anything else is a no-op + debug log.
    static bool accept_message(const MailMessage& message, const GmailTransportOptions& options);
    // base64-or-plain-JSON command body -> (agent selector, protocol Cmd);
    // uses the protocol's strict payload validation (decode_payload(Cmd)).
    std::pair<std::string, protocol::Cmd> parse_command_mail(const MailMessage& message);

    void poll_once(TransportMillis now);
    void send_result_mail(std::uint32_t seq, const Accumulator& results);

    std::shared_ptr<GmailClient> client_;
    GmailTransportOptions options_;
    CommandDispatcher dispatch_;
    PollScheduler scheduler_;
    std::set<std::string> processed_ids_;
};

} // namespace tether::transport
