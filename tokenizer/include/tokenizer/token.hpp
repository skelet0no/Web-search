#pragma once
#include <cstddef>
#include <string>

namespace tokenizer {
inline constexpr char rules_version[] = "unicode-alnum-v1";
struct Token {
    std::size_t position;
    std::size_t byte_begin;
    std::size_t byte_end;
    std::string normalized;
    std::size_t codepoints;
};
}
