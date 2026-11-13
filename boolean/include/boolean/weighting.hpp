#pragma once
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace boolean_search {
inline constexpr char weighting_policy[] = "log-tf-smooth-idf-cosine-v1";
inline std::uint64_t checked_frequency_sum(std::uint64_t a, std::uint64_t b) {
    if (b > UINT64_MAX - a) throw std::overflow_error("Term frequency overflow");
    return a + b;
}
inline double inverse_document_frequency(std::uint64_t documents, std::uint64_t df) {
    if (!df || df > documents) throw std::runtime_error("Invalid document frequency");
    return 1.0 + std::log((static_cast<double>(documents) + 1.0) /
                          (static_cast<double>(df) + 1.0));
}
inline double term_weight(std::uint64_t frequency, double idf) {
    return frequency ? (1.0 + std::log(static_cast<double>(frequency))) * idf : 0.0;
}
}
