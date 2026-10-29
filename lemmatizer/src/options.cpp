#include "options.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <bsoncxx/oid.hpp>
namespace lemmatizer {
namespace {
std::int64_t number(const std::string& value, bool zero = false) {
    std::int64_t result;
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result < (zero ? 0 : 1))
        throw std::invalid_argument("Invalid integer: " + value);
    return result;
}
}
void validate_options(const Options& o) {
    auto alnum = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (o.run_id.empty() || o.run_id.size() > 80 || !alnum(o.run_id.front()) ||
        !std::all_of(o.run_id.begin(), o.run_id.end(), [&](unsigned char c) { return alnum(c) || c == '.' || c == '_' || c == '-'; }))
        throw std::invalid_argument("Invalid run-id");
    if (o.database.empty() || o.dictionary.empty() || o.limit < 0 || o.batch_size < 1 || o.batch_size > 1000 ||
        o.lease_seconds < 3 || o.lease_seconds > 3600 || o.chunk_bytes < 1024 || o.chunk_bytes > 4 * 1024 * 1024 ||
        o.analyzer.max_token_codepoints < 1 || o.analyzer.max_token_codepoints > 4096 || o.analyzer.cache_size > 1000000)
        throw std::invalid_argument("Invalid configuration or missing dictionary");
}
Options parse_options(int argc, char** argv) {
    Options o;
    o.run_id = "lemmas-" + bsoncxx::oid{}.to_string();
    o.dictionary = LEMMATIZER_DEFAULT_DICTIONARY;
    if (auto value = std::getenv("MONGODB_URI")) o.uri = value;
    bool explicit_selection = false, explicit_id = false;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--help") { o.help = true; return o; }
        if (i + 1 == argc) throw std::invalid_argument("Missing value for " + key);
        std::string value = argv[++i];
        if (key == "--uri") o.uri = value;
        else if (key == "--database") o.database = value;
        else if (key == "--dictionary") o.dictionary = value;
        else if (key == "--output") { o.output = value; explicit_selection = true; }
        else if (key == "--tokenization-run") { o.tokenization_run = value; explicit_selection = true; }
        else if (key == "--source") { o.source = value; explicit_selection = true; }
        else if (key == "--limit") { o.limit = number(value); explicit_selection = true; }
        else if (key == "--batch-size") { o.batch_size = number(value); explicit_selection = true; }
        else if (key == "--cache-size") { o.analyzer.cache_size = number(value, true); explicit_selection = true; }
        else if (key == "--max-token-codepoints") { o.analyzer.max_token_codepoints = number(value); explicit_selection = true; }
        else if (key == "--run-id") { o.run_id = value; explicit_id = true; }
        else if (key == "--resume") { o.run_id = value; o.resume = true; }
        else throw std::invalid_argument("Unknown option: " + key);
    }
    if (o.resume && (explicit_selection || explicit_id))
        throw std::invalid_argument("--resume restores saved options; only uri/database/dictionary may be overridden");
    validate_options(o);
    return o;
}
}
