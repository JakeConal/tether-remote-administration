// lib/common/base64.hpp - RFC 4648 base64/base64url codec (Phase 4 utility).
//
// Shared, hand-rolled, unit-tested codec so transport code never reaches for
// OS-specific helpers. Gmail flows ping this twice: command mail bodies
// (standard base64 inside JSON/MIME) and the Gmail API `raw` upload
// (base64url without padding).
//
// Encoding: `url_safe` selects RFC 4648 section 5 ('-'/'_') over the standard
// alphabet; `pad` toggles '=' padding. Decoding is whitespace-tolerant and
// accepts both alphabets plus optional padding regardless of how the text was
// produced - everything else is a Base64Error.
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace tether::common {

// Thrown for every decode problem (invalid character, malformed padding,
// impossible length). Encode never throws.
class Base64Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string base64_encode(const unsigned char* bytes, std::size_t count, bool url_safe = false,
                          bool pad = true);
std::string base64_encode_string(const std::string& text, bool url_safe = false, bool pad = true);

std::vector<unsigned char> base64_decode(const std::string& text);
std::string base64_decode_string(const std::string& text);

} // namespace tether::common
