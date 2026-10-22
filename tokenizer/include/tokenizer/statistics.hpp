#pragma once
#include "token.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace tokenizer {
struct Metrics {
    std::int64_t fields = 0, bytes = 0, tokens = 0, codepoints = 0;
    double seconds = 0;
    void add(std::size_t input_bytes, const std::vector<Token>& values, double elapsed);
};
struct Statistics {
    std::int64_t selected = 0, successful = 0, errors = 0;
    Metrics total;
};
}
