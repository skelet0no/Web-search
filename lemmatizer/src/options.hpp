#pragma once
#include "lemmatizer/analyzer.hpp"
#include <cstdint>
#include <filesystem>
#include <string>
namespace lemmatizer {
struct Options {
    std::string uri = "mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=10000";
    std::string database = "crawler", tokenization_run, run_id, source;
    std::filesystem::path dictionary, output = "docx/runs";
    std::int64_t limit = 0, batch_size = 100;
    AnalyzerOptions analyzer;
    bool resume = false, help = false;
    std::int64_t lease_seconds = 60;
    std::size_t chunk_bytes = 4 * 1024 * 1024;
};
Options parse_options(int argc, char** argv);
void validate_options(const Options& options);
}
