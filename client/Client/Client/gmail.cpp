#include "gmail.h"
#include "base64.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>

using json = nlohmann::json;

std::string getAccessToken() {
    std::string clientId;
    std::string clientSecret;
    std::string refreshToken;

    std::fstream idFile("client.txt", std::ios::in);
    std::getline(idFile, clientId);
    std::getline(idFile, clientSecret);
    std::getline(idFile, refreshToken);
    idFile.close();

    cpr::Response r = cpr::Post(cpr::Url{ "https://oauth2.googleapis.com/token" },
        cpr::Payload{ {"client_id", clientId},
                     {"client_secret", clientSecret},
                     {"refresh_token", refreshToken},
                     {"grant_type", "refresh_token"} });
    if (r.status_code != 200) {
        std::cerr << "Failed to get access token: " << r.status_code << std::endl;
        return "";
    }

    auto jsonData = json::parse(r.text);
    return jsonData["access_token"].get<std::string>();
}

std::string getEmail(const std::string& accessToken, const std::string& messageId) {
    cpr::Response r = cpr::Get(cpr::Url{ "https://www.googleapis.com/gmail/v1/users/me/messages/" + messageId },
        cpr::Header{ {"Authorization", "Bearer " + accessToken} });
    if (r.status_code != 200) {
        std::cerr << "Failed to get email: " << r.status_code << std::endl;
        return "";
    }
    else {
        auto jsonData = json::parse(r.text);
        std::string encodedBody = jsonData["payload"]["parts"][0]["body"]["data"].get<std::string>();
        return base64_decode(encodedBody);
    }
}

std::string getMessageId(const std::string& accessToken) {
    cpr::Response r = cpr::Get(cpr::Url{ "https://www.googleapis.com/gmail/v1/users/me/messages" },
        cpr::Header{ {"Authorization", "Bearer " + accessToken} });
    if (r.status_code != 200) {
        std::cerr << "Failed to list emails: " << r.status_code << std::endl;
        return "";
    }
    else {
        auto jsonData = json::parse(r.text);
        std::string messageId = jsonData["messages"][0]["id"].get<std::string>();
        return messageId;
    }
}

bool isAdmin(const std::string& accessToken, const std::vector<std::string>& adminMail, const std::string& messageId, std::string senderMail) {
    cpr::Response r = cpr::Get(cpr::Url{ "https://www.googleapis.com/gmail/v1/users/me/messages/" + messageId },
        cpr::Header{ {"Authorization", "Bearer " + accessToken} });

    if (r.status_code == 200) {
        auto emailData = json::parse(r.text);
        for (const auto& header : emailData["payload"]["headers"]) {
            if (header["name"] == "From") {
                for (std::string mail : adminMail) {
                    if (header["value"].get<std::string>().find(mail) != std::string::npos) {
                        senderMail = mail;
                        return true;
                    }
                }
                
            }
        }
    }

    return false;
}

void sendEmail(const std::string& accessToken, const std::string& to, const std::string& subject, const std::string& body) {
    std::string email = "From: jakeva123kl@gmail.com\r\nTo: " + to + "\r\nSubject: " + subject + "\r\n\r\n" + body;
    std::string rawMessage = "{\"raw\": \"" + base64_encode(email) + "\"}";

    cpr::Response r = cpr::Post(cpr::Url{ "https://www.googleapis.com/gmail/v1/users/me/messages/send" },
        cpr::Header{ {"Authorization", "Bearer " + accessToken},
                    {"Content-Type", "application/json"} },
        cpr::Body{ rawMessage });

    if (r.status_code != 200) {
        std::cerr << "Failed to send result to ADMIN: " << r.status_code << std::endl;
    }
    else {
        std::cout << "Result sent to ADMIN successfully" << std::endl;
    }
}

void sendGmailWithAttachment(const std::string& accessToken, const std::string& to, const std::string& subject, const std::string& body, const std::string& attachmentPath) {
    std::ifstream file(attachmentPath, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open file: " << attachmentPath << std::endl;
        return;
    }

    std::ostringstream oss;
    oss << file.rdbuf();
    std::string fileContent = oss.str();
    std::string encodedFileContent = base64_encode(fileContent);

    std::string email = "From: jakeva123kl@gmail.com\r\nTo: " + to + "\r\nSubject: " + subject + "\r\nMIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\"boundary\"\r\n\r\n";
    email += "--boundary\r\nContent-Type: text/plain; charset=\"UTF-8\"\r\n\r\n" + body + "\r\n";
    email += "--boundary\r\nContent-Type: application/octet-stream; name=\"" + attachmentPath + "\"\r\nContent-Transfer-Encoding: base64\r\nContent-Disposition: attachment; filename=\"" + attachmentPath + "\"\r\n\r\n" + encodedFileContent + "\r\n--boundary--";

    std::string rawMessage = "{\"raw\": \"" + base64_encode(email) + "\"}";

    cpr::Response r = cpr::Post(cpr::Url{ "https://www.googleapis.com/gmail/v1/users/me/messages/send" },
        cpr::Header{ {"Authorization", "Bearer " + accessToken},
                    {"Content-Type", "application/json"} },
        cpr::Body{ rawMessage });

    if (r.status_code != 200) {
        std::cerr << "Failed to send email with attachment: " << r.status_code << std::endl;
    }
    else {
        std::cout << "Email with attachment sent successfully" << std::endl;
    }
}