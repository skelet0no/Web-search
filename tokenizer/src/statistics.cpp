#include "tokenizer/statistics.hpp"

namespace tokenizer {
void Metrics::add(std::size_t input_bytes, const std::vector<Token>& values, double elapsed) {
    ++fields;
    bytes += static_cast<std::int64_t>(input_bytes);
    tokens += static_cast<std::int64_t>(values.size());
    for (const auto& token : values) codepoints += static_cast<std::int64_t>(token.codepoints);
    seconds += elapsed;
}
}