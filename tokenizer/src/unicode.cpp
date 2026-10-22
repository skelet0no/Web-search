#include "unicode.hpp"
#include "tokenizer/tokenizer.hpp"
#include <unicode/locid.h>
#include <unicode/normalizer2.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/uversion.h>
#include <algorithm>

namespace tokenizer::unicode {
std::string normalize(std::string_view token, std::size_t& codepoints) {
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

namespace tokenizer {
std::string icu_version() { return U_ICU_VERSION; }
std::string unicode_version() {
    UVersionInfo version;
    u_getUnicodeVersion(version);
    char text[U_MAX_VERSION_STRING_LENGTH];
    u_versionToString(version, text);
    return text;
}
}
