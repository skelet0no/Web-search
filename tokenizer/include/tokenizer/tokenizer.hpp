#pragma once
#include "token.hpp"
#include <stdexcept>
#include <string_view>
#include <vector>

namespace tokenizer {
class InvalidUtf8 : public std::invalid_argument {
public:
    explicit InvalidUtf8(std::size_t offset);
};
std::vector<Token> tokenize(std::string_view text);
std::string unicode_version();
std::string icu_version();
}