#pragma once
#include <string>
#include <string_view>

namespace tokenizer::unicode {
std::string normalize(std::string_view token, std::size_t& codepoints);
}
