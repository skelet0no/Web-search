#include "text/unicode.hpp"
#include <stdexcept>
#include <limits>
#include <unicode/utf8.h>
#include <unicode/locid.h>
#include <unicode/normalizer2.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/uversion.h>
#include <algorithm>

namespace text {
void validate_utf8(std::string_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
        throw std::invalid_argument("Text exceeds ICU input limit");
    int32_t offset = 0;
    while (offset < static_cast<int32_t>(value.size())) {
        const auto begin = offset;
        UChar32 cp;
        U8_NEXT(value.data(), offset, static_cast<int32_t>(value.size()), cp);
        if (cp < 0) throw std::invalid_argument("Invalid UTF-8 at byte " + std::to_string(begin));
    }
}
std::string normalize(std::string_view token, std::size_t& codepoints) {
    validate_utf8(token);
    if (std::all_of(token.begin(), token.end(), [](unsigned char c) { return c < 128; })) {
        std::string result(token);
        for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        codepoints = result.size();
        return result;
    }
    UErrorCode error = U_ZERO_ERROR;
    const auto* nfc = icu::Normalizer2::getNFCInstance(error);
    if (U_FAILURE(error)) throw std::runtime_error(u_errorName(error));
    auto value = icu::UnicodeString::fromUTF8(icu::StringPiece(token.data(), static_cast<int32_t>(token.size())));
    value.toLower(icu::Locale::getRoot());
    icu::UnicodeString result;
    nfc->normalize(value, result, error);
    if (U_FAILURE(error)) throw std::runtime_error(u_errorName(error));
    codepoints = static_cast<std::size_t>(result.countChar32());
    std::string utf8;
    result.toUTF8String(utf8);
    return utf8;
}
}

namespace text {
std::string normalize(std::string_view value) {
    std::size_t length = 0;
    return normalize(value, length);
}
std::string icu_version() { return U_ICU_VERSION; }
std::string unicode_version() {
    UVersionInfo version;
    u_getUnicodeVersion(version);
    char text[U_MAX_VERSION_STRING_LENGTH];
    u_versionToString(version, text);
    return text;
}
}
