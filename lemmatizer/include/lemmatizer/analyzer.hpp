#pragma once
#include "analysis.hpp"
#include <cstddef>
#include <filesystem>
#include <list>
#include <memory>
#include <string_view>
#include <unordered_map>
namespace lemmatizer {
class Backend {
public:
    virtual ~Backend() = default;
    virtual std::vector<Analysis> analyze(std::string_view normalized) = 0;
};
std::unique_ptr<Backend> make_xmorphy(const std::filesystem::path& dictionary);
std::string sha256_file(const std::filesystem::path& path);
std::string sha256(std::string_view value);
struct AnalyzerOptions {
    std::size_t cache_size = 20000;
    std::size_t max_token_codepoints = 128;
};
class Analyzer {
public:
    explicit Analyzer(Backend& backend, AnalyzerOptions options = {});
    Result analyze(std::string_view normalized, std::string_view language = {});
    std::size_t cache_entries() const { return cache_.size(); }
    std::size_t cache_hits() const { return hits_; }
    std::size_t cache_misses() const { return misses_; }
private:
    Backend& backend_;
    AnalyzerOptions options_;
    std::list<std::pair<std::string, Result>> order_;
    std::unordered_map<std::string, decltype(order_)::iterator> cache_;
    std::size_t hits_ = 0, misses_ = 0;
};
}
