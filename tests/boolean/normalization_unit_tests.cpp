#include "boolean/normalization.hpp"
#include "lemmatizer/analyzer.hpp"
#include "tokenizer/tokenizer.hpp"
#include "text/unicode.hpp"
#include "../support.hpp"
#include <algorithm>
#include <iostream>

namespace {
struct Backend final : lemmatizer::Backend {
    std::vector<lemmatizer::Analysis> analyze(std::string_view word) override {
        if (word=="стали") return {{"стать",true,{}},{"сталь",true,{}}};
        if (word=="игры") return {{"игра",true,{}}};
        return {};
    }
};
}
int main() {
    using namespace test_support;
    using namespace boolean_search;
    try {
        Backend backend;
        lemmatizer::Analyzer analyzer(backend);
        const auto expanded=normalize_query("ИГРЫ стали PS5",analyzer);
        require(expanded.size()==3,"Query token expansion count");
        require(expanded[0]==std::vector<std::string>{"игра","игры"},"Original normalized form fallback");
        for (const auto* term:{"сталь","стать","стали"})
            require(std::find(expanded[1].begin(),expanded[1].end(),term)!=expanded[1].end(),"Missing ambiguity candidate");
        require(expanded[2][0]=="ps5","Unsupported token normalization");
        require(normalize_query("😀",analyzer).empty(),"Punctuation creates terms");
        Metadata metadata{{"policy_version",lemmatizer::policy_version},{"rules_version",tokenizer::rules_version},
                          {"engine_commit",lemmatizer::engine_commit},{"main_sha256",lemmatizer::main_dictionary_sha256},
                          {"affix_sha256",lemmatizer::affix_dictionary_sha256},{"icu_version",text::icu_version()},
                          {"unicode_version",text::unicode_version()}};
        require_compatible(metadata);
        for (const auto& [key,value]:metadata) {
            auto incompatible=metadata; incompatible[key]="incompatible";
            rejects<std::runtime_error>([&] { require_compatible(incompatible); },"Incompatible analyzer accepted");
            incompatible.erase(key);
            rejects<std::runtime_error>([&] { require_compatible(incompatible); },"Missing compatibility field accepted");
        }
        std::cout << "Query normalization unit tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
