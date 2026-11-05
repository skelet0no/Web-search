#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
namespace zipf {
struct Term { std::string text; std::uint64_t count; };
struct Distribution {
    std::vector<Term> terms;
    std::uint64_t tokens = 0, singletons = 0;
    double constant = 0, log_rmse = 0, alpha = 0;
    bool has_alpha = false;
};
Distribution analyze(const std::unordered_map<std::string, std::uint64_t>& counts);
void plot(const std::filesystem::path& path, const Distribution& d);
void report(const std::filesystem::path& directory, const Distribution& d,
            const std::string& metadata_json, const std::string& run,
            const std::string& tokenization, const std::string& corpus, std::uint64_t records);
}
