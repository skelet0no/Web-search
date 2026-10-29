#pragma once
#include <optional>
#include <string>
#include <vector>
namespace lemmatizer {
inline constexpr char policy_version[] = "ru-dictionary-v1";
inline constexpr char engine_commit[] = "88eb5b84e28f0e00672f07e4fe92e88262242163";
inline constexpr char main_dictionary_sha256[] = "0d92b222f96abab667449a816f3e45d9c28a30f47a0100c1a8e3682d0b6444ff";
inline constexpr char affix_dictionary_sha256[] = "779a1988e7af29e389179681a59f10e169760f5e5cbeb79580e7219bedbd3a60";
struct Analysis {
    std::string lemma;
    bool dictionary = true;
    std::optional<double> score;
};
struct Result {
    std::string lemma;
    std::vector<std::string> candidates;
    std::string method;
    bool ambiguous() const { return candidates.size() > 1; }
};
}
