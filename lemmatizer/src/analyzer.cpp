#include "lemmatizer/analyzer.hpp"
#include "text/unicode.hpp"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <unicode/utf8.h>
namespace lemmatizer {
Analyzer::Analyzer(Backend& backend, AnalyzerOptions options) : backend_(backend), options_(options) {
    if (!options_.max_token_codepoints || options_.max_token_codepoints > 4096)
        throw std::invalid_argument("max-token-codepoints must be 1..4096");
}
Result Analyzer::analyze(std::string_view normalized, std::string_view language) {
    text::validate_utf8(normalized);
    auto unchanged = [&](const char* reason) { return Result{std::string(normalized), {std::string(normalized)}, reason}; };
    if (normalized.empty()) return unchanged("empty");
    const auto lang = text::normalize(language);
    const auto primary = lang.substr(0, lang.find_first_of("-_:"));
    if (!primary.empty() && primary != "und" && primary != "unknown" && primary != "ru")
        return unchanged("language");
    int32_t offset = 0;
    std::size_t length = 0;
    bool russian = true;
    while (offset < static_cast<int32_t>(normalized.size())) {
        UChar32 cp;
        U8_NEXT(normalized.data(), offset, static_cast<int32_t>(normalized.size()), cp);
        ++length;
        russian = russian && ((cp >= 0x430 && cp <= 0x44f) || cp == 0x451);
        if (length > options_.max_token_codepoints) return unchanged("too_long");
    }
    if (!russian) return unchanged("unsupported_token");
    const std::string key(normalized);
    if (auto found = cache_.find(key); found != cache_.end()) {
        ++hits_;
        order_.splice(order_.begin(), order_, found->second);
        return found->second->second;
    }
    ++misses_;
    // XMorphy's raw count is NOT a calibrated probability. Policy v1 uses a
    // deterministic lexicographic order and preserves every dictionary lemma.
    std::vector<std::string> candidates;
    for (const auto& analysis : backend_.analyze(normalized)) {
        if (!analysis.dictionary) continue;
        auto lemma = text::normalize(analysis.lemma);
        if (lemma.empty()) throw std::runtime_error("Dictionary returned an empty lemma");
        candidates.push_back(std::move(lemma));
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    Result result = candidates.empty() ? unchanged("unknown")
        : Result{candidates.front(), candidates, "dictionary"};
    if (options_.cache_size) {
        order_.emplace_front(key, result);
        cache_.emplace(key, order_.begin());
        if (cache_.size() > options_.cache_size) {
            cache_.erase(order_.back().first);
            order_.pop_back();
        }
    }
    return result;
}
}
