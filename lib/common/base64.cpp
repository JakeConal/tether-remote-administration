// lib/common/base64.cpp - see base64.hpp for the contract.
#include "base64.hpp"

#include <array>

namespace tether::common {
namespace {

constexpr char kStandardAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kUrlSafeAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// '-'/'+', '_'/'/' both map to the same two 6-bit values, so one lookup table
// serves both alphabets; anything else (before '=' padding handling) is an
// error.
int six_bits(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';        // 0..25
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;   // 26..51
    if (c >= '0' && c <= '9') return c - '0' + 52;   // 52..61
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1; // '=' handled by the caller; whitespace skipped there
}

bool is_skippable(char c) {
    return c == '\r' || c == '\n' || c == ' ' || c == '\t' || c == '\f' || c == '\v';
}

} // namespace

std::string base64_encode(const unsigned char* bytes, std::size_t count, bool url_safe, bool pad) {
    const char* alphabet = url_safe ? kUrlSafeAlphabet : kStandardAlphabet;
    std::string out;
    out.reserve((count + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= count; i += 3) {
        const unsigned n = (static_cast<unsigned>(bytes[i]) << 16) |
                           (static_cast<unsigned>(bytes[i + 1]) << 8) |
                           static_cast<unsigned>(bytes[i + 2]);
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(alphabet[(n >> 6) & 63]);
        out.push_back(alphabet[n & 63]);
    }
    const std::size_t rest = count - i;
    if (rest == 1) {
        const unsigned n = static_cast<unsigned>(bytes[i]) << 16;
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        if (pad) out += "==";
    } else if (rest == 2) {
        const unsigned n = (static_cast<unsigned>(bytes[i]) << 16) |
                           (static_cast<unsigned>(bytes[i + 1]) << 8);
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(alphabet[(n >> 6) & 63]);
        if (pad) out.push_back('=');
    }
    return out;
}

std::string base64_encode_string(const std::string& text, bool url_safe, bool pad) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    return base64_encode(bytes, text.size(), url_safe, pad);
}

std::vector<unsigned char> base64_decode(const std::string& text) {
    std::vector<unsigned char> out;
    out.reserve(text.size() / 4 * 3);

    unsigned buffer = 0; // packed 6-bit groups, LSB-aligned to the next slot
    int groups = 0;      // how many 6-bit groups sit in `buffer`
    std::size_t index = 0;
    while (index < text.size()) {
        const char c = text[index++];
        if (is_skippable(c)) {
            continue; // MIME bodies carry CRLF + tabs around the payload
        }
        if (c == '=') {
            // Padding may only terminate the stream (possibly with a second
            // '='; both are just stream end from here).
            break;
        }
        const int bits = six_bits(c);
        if (bits < 0) {
            throw Base64Error("base64: invalid character in input");
        }
        buffer = (buffer << 6) | static_cast<unsigned>(bits);
        ++groups;
        if (groups == 4) {
            out.push_back(static_cast<unsigned char>((buffer >> 16) & 0xFF));
            out.push_back(static_cast<unsigned char>((buffer >> 8) & 0xFF));
            out.push_back(static_cast<unsigned char>(buffer & 0xFF));
            buffer = 0;
            groups = 0;
        }
    }
    // Tail: 1, 2, or 3 leftover groups encode 1 or 2 bytes. Anything else is a
    // broken stream.
    if (groups == 1) {
        throw Base64Error("base64: dangling character after padding-less group");
    }
    if (groups == 2) {
        out.push_back(static_cast<unsigned char>((buffer >> 4) & 0xFF));
    } else if (groups == 3) {
        out.push_back(static_cast<unsigned char>((buffer >> 10) & 0xFF));
        out.push_back(static_cast<unsigned char>((buffer >> 2) & 0xFF));
    }
    return out;
}

std::string base64_decode_string(const std::string& text) {
    const std::vector<unsigned char> bytes = base64_decode(text);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

} // namespace tether::common
