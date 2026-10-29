#pragma once
#include <cstdint>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
namespace lemmatizer {
struct Statistics {
    std::int64_t records = 0, errors = 0, tokens = 0, changed = 0, ambiguous = 0;
    std::int64_t unknown = 0, skipped = 0, input_bytes = 0, output_parts = 0;
    std::int64_t input_bson_bytes = 0, output_bson_bytes = 0;
    std::int64_t cache_hits = 0, cache_misses = 0;
    double core_seconds = 0, wall_seconds = 0;
    bsoncxx::document::value bson() const;
    static Statistics load(bsoncxx::document::view view);
};
}
