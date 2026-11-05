#pragma once
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types.hpp>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
namespace zipf {
inline void count_record(bsoncxx::document::view row,
                         std::unordered_map<std::string, std::uint64_t>& counts) {
    const auto status = row["status"];
    if (!status || status.type() != bsoncxx::type::k_string || status.get_string().value != "ok")
        throw std::runtime_error("Unexpected error record in completed run");
    const auto annotations = row["annotations"];
    if (!annotations || annotations.type() != bsoncxx::type::k_array)
        throw std::runtime_error("Missing annotations array");
    for (const auto element : annotations.get_array().value) {
        if (element.type() != bsoncxx::type::k_document) throw std::runtime_error("Invalid annotation");
        const auto lemma = element.get_document().view()["lemma"];
        if (!lemma || lemma.type() != bsoncxx::type::k_string || lemma.get_string().value.empty())
            throw std::runtime_error("Missing or empty lemma");
        auto& count = counts[std::string(lemma.get_string().value)];
        if (count == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Frequency overflow");
        ++count;
    }
}
}
