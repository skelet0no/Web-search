#include "boolean/normalization.hpp"
#include "app.hpp"
#include "lemmatizer/analyzer.hpp"
#include "tokenizer/tokenizer.hpp"
#include "text/unicode.hpp"

namespace boolean_search {
void require_compatible(const Metadata& m) {
    const auto expect=[&](const char* key,const std::string& value) {
        if (app::required(m,key)!=value) throw std::runtime_error(std::string("Incompatible index: ")+key);
    };
    expect("policy_version",lemmatizer::policy_version);
    expect("rules_version",tokenizer::rules_version);
    expect("engine_commit",lemmatizer::engine_commit);
    expect("main_sha256",lemmatizer::main_dictionary_sha256);
    expect("affix_sha256",lemmatizer::affix_dictionary_sha256);
    expect("icu_version",text::icu_version());
    expect("unicode_version",text::unicode_version());
}
std::vector<std::vector<std::string>> normalize_query(const std::string& raw,lemmatizer::Analyzer& analyzer) {
    std::vector<std::vector<std::string>> expanded;
    for (const auto& token:tokenizer::tokenize(raw)) {
        auto candidates=analyzer.analyze(token.normalized).candidates;
        candidates.push_back(token.normalized);
        expanded.push_back(std::move(candidates));
    }
    return expanded;
}
}
