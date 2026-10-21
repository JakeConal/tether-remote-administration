// Phase 4 utility: base64 codec vectors (RFC 4648) - the Gmail command-body
// decoder and MIME attachment encoder ride this codec, so it gets its own
// boundary suite.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "base64.hpp"

#include <string>
#include <vector>

namespace tc = tether::common;

namespace {
void check_roundtrip(const std::string& text, bool url_safe = false, bool pad = true) {
    const std::string encoded = tc::base64_encode_string(text, url_safe, pad);
    const std::string decoded = tc::base64_decode_string(encoded);
    CHECK(decoded == text);
    if (pad) {
        const std::string no_pad = tc::base64_encode_string(text, url_safe, false);
        CHECK(tc::base64_decode_string(no_pad) == text);
    }
}
} // namespace

TEST_CASE("RFC 4648 test vectors encode exactly") {
    // Base64 alphabet, section 10 test vectors.
    CHECK(tc::base64_encode_string("") == "");
    CHECK(tc::base64_encode_string("f") == "Zg==");
    CHECK(tc::base64_encode_string("fo") == "Zm8=");
    CHECK(tc::base64_encode_string("foo") == "Zm9v");
    CHECK(tc::base64_encode_string("foob") == "Zm9vYg==");
    CHECK(tc::base64_encode_string("fooba") == "Zm9vYmE=");
    CHECK(tc::base64_encode_string("foobar") == "Zm9vYmFy");
}

TEST_CASE("URL-safe alphabet swaps (+/ -> -_) with padding still decodable") {
    // 0xFB 0xFF = 11111011 11111111 -> groups "+/8" standard, "-_8" url-safe.
    const std::vector<unsigned char> bytes{0xFB, 0xFF};
    CHECK(tc::base64_encode(bytes.data(), bytes.size(), true) == "-_8=");
    CHECK(tc::base64_encode(bytes.data(), bytes.size(), false) == "+/8=");
    CHECK(tc::base64_decode("-_8=").at(0) == 0xFB);
    CHECK(tc::base64_decode("+/8=").at(0) == 0xFB);
}

TEST_CASE("decode accepts unpadded, padded, and whitespace-tolerant input") {
    CHECK(tc::base64_decode_string("Zg") == "f");
    CHECK(tc::base64_decode_string("Zm8") == "fo");
    CHECK(tc::base64_decode_string("Zm9vYg") == "foob");
    CHECK(tc::base64_decode_string(" Zm9v\r\nYmFy\t") == "foobar");
}

TEST_CASE("decode rejects impossible and invalid streams") {
    CHECK_THROWS_AS(tc::base64_decode("A"), tc::Base64Error);      // dangling 5 bits
    CHECK_THROWS_AS(tc::base64_decode("Z?9v"), tc::Base64Error);   // bad character
    CHECK_THROWS_AS(tc::base64_decode("---Z!"), tc::Base64Error);  // bad character
}

TEST_CASE("binary round-trips survive all lengths") {
    std::vector<unsigned char> corpus;
    for (int i = 0; i < 256; ++i) {
        corpus.push_back(static_cast<unsigned char>(i * 7 + 1));
    }
    for (std::size_t length = 0; length <= corpus.size(); ++length) {
        const std::string encoded = tc::base64_encode(corpus.data(), length);
        const std::vector<unsigned char> decoded = tc::base64_decode(encoded);
        REQUIRE(decoded.size() == length);
        for (std::size_t i = 0; i < length; ++i) {
            REQUIRE(decoded[i] == corpus[i]);
        }
    }
}

TEST_CASE("text round-trips through the string API in both alphabets") {
    // The string API is what the Gmail command-body decoder and the MIME
    // encoder call: it must survive the round trip in both alphabets, and
    // the unpadded form must decode to the same text.
    check_roundtrip("");
    check_roundtrip("a");
    check_roundtrip("ab");
    check_roundtrip("abc");
    check_roundtrip("tether console << agent >> result");
    check_roundtrip("tether console << agent >> result", /*url_safe=*/true);
    check_roundtrip("binary-ish \x01\x02\x7f payload");
}
