#include "tokenizer/tokenizer.hpp"
#include "unicode.hpp"
#include <limits>
#include <unicode/utf8.h>
#include <unicode/uchar.h>

namespace tokenizer {
InvalidUtf8::InvalidUtf8(std::size_t offset)
    : std::invalid_argument("Invalid UTF-8 at byte " + std::to_string(offset)) {}

std::vector<Token> tokenize(std::string_view text) {
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
        throw std::invalid_argument("Text exceeds ICU input limit");
    std::vector<Token> tokens;
    const auto size = static_cast<int32_t>(text.size());
    int32_t offset = 0, begin = -1;
    bool follows_letter = false;
    auto emit = [&](int32_t end) {
        if (begin < 0) return;
        std::size_t length = 0;
        auto normalized = unicode::normalize(text.substr(begin, end - begin), length);
        tokens.push_back({tokens.size(), static_cast<std::size_t>(begin),
                          static_cast<std::size_t>(end), std::move(normalized), length});
        begin = -1;
    };
    while (offset < size) {
        const auto current = offset;
        UChar32 cp;
        U8_NEXT(text.data(), offset, size, cp);
        if (cp < 0) throw InvalidUtf8(static_cast<std::size_t>(current));
        const auto type = u_charType(cp);
        const bool is_letter = type >= U_UPPERCASE_LETTER && type <= U_OTHER_LETTER;
        const bool is_digit = type == U_DECIMAL_DIGIT_NUMBER;
        const bool is_mark = follows_letter && type >= U_NON_SPACING_MARK && type <= U_COMBINING_SPACING_MARK;
        if (is_letter || is_digit || is_mark) {
            if (begin < 0) begin = current;
            if (!is_mark) follows_letter = is_letter;
        } else {
            emit(current);
            follows_letter = false;
        }
    }
    emit(size);
    return tokens;
}
}
