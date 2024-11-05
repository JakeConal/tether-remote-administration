#include <iostream>
#include <string>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

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

void sendEmail(const std::string& accessToken) {
    std::string email = "From: jakeva123kl@gmail.com\r\nTo: hieunguyen.jc@gmail.com\r\nSubject: Test Email\r\n\r\nThis is a test email.";
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

void listEmails(const std::string& accessToken) {
    cpr::Response r = cpr::Get(cpr::Url{ "https://www.googleapis.com/gmail/v1/users/me/messages" },
        cpr::Header{ {"Authorization", "Bearer " + accessToken} });
    if (r.status_code != 200) {
        std::cerr << "Failed to list emails: " << r.status_code << std::endl;
    }
    else {
        std::cout << "Emails: " << r.text << std::endl;
    }
}

void getEmail(const std::string& accessToken, const std::string& messageId) {
    cpr::Response r = cpr::Get(cpr::Url{ "https://www.googleapis.com/gmail/v1/users/me/messages/" + messageId },
        cpr::Header{ {"Authorization", "Bearer " + accessToken} });
    if (r.status_code != 200) {
        std::cerr << "Failed to get email: " << r.status_code << std::endl;
    }
    else {
        std::cout << "Email: " << r.text << std::endl;
    }
}

int main() {
    std::string clientId = "";
    std::string clientSecret = "";
    std::string refreshToken = "";
    std::string accessToken = getAccessToken(refreshToken, clientId, clientSecret);
    if (!accessToken.empty()) {
        sendEmail(accessToken);
        listEmails(accessToken);
        // Replace "messageId" with the actual message ID you want to read
        //getEmail(accessToken, "messageId"); 
    }
    else {
        std::cerr << "Failed to get access token" << std::endl;
    }
    return 0;
}