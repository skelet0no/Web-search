#pragma once
#include <cstddef>
#include <string>
#include <string_view>
namespace text {
void validate_utf8(std::string_view value);
std::string normalize(std::string_view value, std::size_t& codepoints);
std::string normalize(std::string_view value);
std::string icu_version();
std::string unicode_version();
}
