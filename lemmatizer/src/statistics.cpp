#include "statistics.hpp"
#include <bsoncxx/builder/basic/document.hpp>
#include <stdexcept>
namespace lemmatizer {
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
bsoncxx::document::value Statistics::bson() const {
    return make_document(kvp("records", records), kvp("errors", errors), kvp("tokens", tokens),
        kvp("changed", changed), kvp("ambiguous", ambiguous), kvp("unknown", unknown), kvp("skipped", skipped),
        kvp("input_bytes", input_bytes), kvp("output_parts", output_parts), kvp("cache_hits", cache_hits),
        kvp("input_bson_bytes", input_bson_bytes), kvp("output_bson_bytes", output_bson_bytes),
        kvp("cache_misses", cache_misses), kvp("core_seconds", core_seconds), kvp("wall_seconds", wall_seconds));
}
Statistics Statistics::load(bsoncxx::document::view v) {
    Statistics s;
#define READ_COUNT(name) s.name = v[#name].get_int64().value; if (s.name < 0) throw std::runtime_error("Invalid statistics")
    READ_COUNT(records); READ_COUNT(errors); READ_COUNT(tokens); READ_COUNT(changed);
    READ_COUNT(ambiguous); READ_COUNT(unknown); READ_COUNT(skipped); READ_COUNT(input_bytes);
    READ_COUNT(output_parts); READ_COUNT(cache_hits); READ_COUNT(cache_misses);
    READ_COUNT(input_bson_bytes); READ_COUNT(output_bson_bytes);
#undef READ_COUNT
    s.core_seconds = v["core_seconds"].get_double().value;
    s.wall_seconds = v["wall_seconds"].get_double().value;
    return s;
}
}
