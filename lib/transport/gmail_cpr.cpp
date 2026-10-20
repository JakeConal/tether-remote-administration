// lib/transport/gmail_cpr.cpp - the concrete GmailClient over the Gmail REST
// API, plus the concrete cpr-backed HttpClient behind the seam. THIS file is
// the only place in the tree where cpr is used (task 4.2's seam rule).
//
// OAuth refresh-token flow (design: credentials live at the config-named path
// outside the repo):
//   POST https://oauth2.googleapis.com/token
//     grant_type=refresh_token, client_id, client_secret, refresh_token
//   -> {access_token, expires_in}  (cached until shortly before expiry)
//
// Inbox flows ride Gmail API v1 /users/me endpoints (list, fetch, send) with
// the refreshed bearer token. Every non-2xx becomes a TransportError carrying
// the status, so callers reason in one error taxonomy.
//
// Tests never instantiate any of this (fakes subclass the seams), so CI runs
// with zero network I/O by construction.
#include "gmail.hpp"

#include "base64.hpp"

#include <cpr/cpr.h>

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <utility>

namespace tether::transport {
namespace {

using nlohmann::json;

std::string require_2xx(long status, const std::string& body, const std::string& what) {
    if (status < 200 || status > 299) {
        throw TransportError(what + ": http status " + std::to_string(status) +
                             (body.empty() ? "" : (": " + body.substr(0, 512))));
    }
    return body;
}

// ---------------------------------------------------------------------------
// The curl/cpr-backed HttpClient seam implementation.
// ---------------------------------------------------------------------------
class CprHttpClient final : public HttpClient {
public:
    std::string post_form(const std::string& url,
                          const std::vector<std::pair<std::string, std::string>>& form) override {
        // cpr's Payload carries url-encoded key/value pairs; the iterator ctor
        // avoids a default construction the installed cpr version does not have.
        std::vector<cpr::Pair> pairs;
        pairs.reserve(form.size());
        for (const auto& [key, value] : form) {
            pairs.emplace_back(key, value);
        }
        const cpr::Payload payload(pairs.begin(), pairs.end());
        const cpr::Response response = cpr::Post(cpr::Url{url}, payload);
        return require_2xx(response.status_code, response.text, "form POST " + url);
    }

    std::string get(const std::string& url,
                    const std::vector<std::pair<std::string, std::string>>& headers) override {
        cpr::Header cpr_headers;
        for (const auto& [key, value] : headers) {
            cpr_headers[key] = value;
        }
        const cpr::Response response = cpr::Get(cpr::Url{url}, cpr_headers);
        return require_2xx(response.status_code, response.text, "GET " + url);
    }

    std::string post_json(const std::string& url,
                          const std::vector<std::pair<std::string, std::string>>& headers,
                          const std::string& json_body) override {
        cpr::Header cpr_headers;
        for (const auto& [key, value] : headers) {
            cpr_headers[key] = value;
        }
        cpr_headers["Content-Type"] = "application/json";
        const cpr::Response response =
            cpr::Post(cpr::Url{url}, cpr_headers, cpr::Body{json_body});
        return require_2xx(response.status_code, response.text, "POST " + url);
    }
};

// ---------------------------------------------------------------------------
// The real Gmail client over the REST API.
// ---------------------------------------------------------------------------
class GoogleGmailClient final : public GmailClient {
public:
    GoogleGmailClient(std::shared_ptr<HttpClient> http, GmailCredentials credentials)
        : http_(std::move(http)), credentials_(std::move(credentials)) {}

    std::vector<std::string> list_inbox_ids(std::size_t max_results) override {
        const std::string body = http_->get(
            std::string(kApiBase) +
                "/messages?labelIds=INBOX&maxResults=" + std::to_string(max_results),
            bearer_header());
        const json doc = json::parse(require_2xx(200, body, "gmail list"));
        std::vector<std::string> ids;
        const auto messages = doc.find("messages");
        if (messages != doc.end() && messages->is_array()) {
            for (const auto& entry : *messages) {
                if (entry.is_object() && entry.contains("id") && entry.at("id").is_string()) {
                    ids.push_back(entry.at("id").get<std::string>());
                }
            }
        }
        return ids;
    }

    MailMessage fetch(const std::string& id) override {
        const std::string body = http_->get(
            std::string(kApiBase) + "/messages/" + id + "?format=full", bearer_header());
        const json doc = json::parse(require_2xx(200, body, "gmail fetch " + id));

        MailMessage message;
        message.id = id;
        const auto payload = doc.find("payload");
        if (payload == doc.end() || !payload->is_object()) {
            throw TransportError("gmail fetch: message carries no payload (" + id + ")");
        }
        const auto headers = payload->find("headers");
        if (headers != payload->end() && headers->is_array()) {
            for (const auto& header : *headers) {
                if (!header.is_object()) {
                    continue;
                }
                const auto name = header.find("name");
                const auto value = header.find("value");
                if (name == header.end() || !name->is_string() || value == header.end() ||
                    !value->is_string()) {
                    continue;
                }
                std::string lower = name->get<std::string>();
                for (char& c : lower) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (lower == "from") {
                    message.from = value->get<std::string>();
                } else if (lower == "to") {
                    message.to = value->get<std::string>();
                } else if (lower == "subject") {
                    message.subject = value->get<std::string>();
                }
            }
        }
        message.body_text = extract_text_body(*payload);
        return message;
    }

    std::string send_raw(const std::string& mime) override {
        // Gmail consumes the whole RFC 822 message as a base64url `raw` body.
        json request;
        request["raw"] = common::base64_encode_string(mime, /*url_safe=*/true, /*pad=*/false);
        const std::string body = http_->post_json(std::string(kApiBase) + "/messages/send",
                                                  bearer_header(), request.dump());
        const json sent = json::parse(require_2xx(200, body, "gmail send"));
        if (sent.is_object() && sent.contains("id") && sent.at("id").is_string()) {
            return sent.at("id").get<std::string>();
        }
        return {};
    }

private:
    static constexpr const char* kTokenUrl = "https://oauth2.googleapis.com/token";
    static constexpr const char* kApiBase = "https://gmail.googleapis.com/gmail/v1/users/me";

    // Refreshes the OAuth access token when required; returns the Bearer header.
    std::vector<std::pair<std::string, std::string>> bearer_header() {
        const auto now = std::chrono::steady_clock::now();
        if (access_token_.empty() || now >= token_expiry_) {
            token_refresh();
        }
        return {{"Authorization", "Bearer " + access_token_}};
    }

    void token_refresh() {
        const std::string body = http_->post_form(
            kTokenUrl,
            {{"grant_type", "refresh_token"},
             {"client_id", credentials_.client_id},
             {"client_secret", credentials_.client_secret},
             {"refresh_token", credentials_.refresh_token}});
        const json doc = json::parse(require_2xx(200, body, "oauth refresh"));
        const auto token = doc.find("access_token");
        const auto expires = doc.find("expires_in");
        if (token == doc.end() || !token->is_string() || token->get<std::string>().empty()) {
            throw TransportError("oauth refresh-token response carried no access_token");
        }
        access_token_ = token->get<std::string>();
        const int expires_in =
            expires != doc.end() && expires->is_number_integer() ? expires->get<int>() : 3600;
        token_expiry_ = std::chrono::steady_clock::now() +
                        std::chrono::seconds(expires_in > 120 ? expires_in - 60 : expires_in);
    }

    static std::string extract_text_body(const json& payload) {
        // Full-format payloads nest the text/plain body either in this part's
        // own `body.data` or inside recursive `parts` entries.
        const auto mime = payload.find("mimeType");
        if (mime != payload.end() && mime->is_string() &&
            mime->get<std::string>().rfind("text/plain", 0) == 0) {
            const auto body = payload.find("body");
            if (body != payload.end() && body->is_object()) {
                const auto data = body->find("data");
                if (data != body->end() && data->is_string()) {
                    return common::base64_decode_string(data->get<std::string>());
                }
            }
        }
        const auto parts = payload.find("parts");
        if (parts != payload.end() && parts->is_array()) {
            for (const auto& part : *parts) {
                if (part.is_object()) {
                    std::string text = extract_text_body(part);
                    if (!text.empty()) {
                        return text;
                    }
                }
            }
        }
        return {};
    }

    std::shared_ptr<HttpClient> http_;
    GmailCredentials credentials_;
    std::string access_token_;
    std::chrono::steady_clock::time_point token_expiry_{};
};

} // namespace

std::unique_ptr<HttpClient> make_curl_http_client() { return std::make_unique<CprHttpClient>(); }

std::unique_ptr<GmailClient> make_google_gmail_client(std::shared_ptr<HttpClient> http,
                                                      GmailCredentials credentials) {
    return std::make_unique<GoogleGmailClient>(std::move(http), std::move(credentials));
}

} // namespace tether::transport
