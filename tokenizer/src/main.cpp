#include "repository.hpp"
#include <bsoncxx/oid.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <mongocxx/instance.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
std::int64_t positive(const std::string& value) {
    std::int64_t number = 0;
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || number < 1)
        throw std::invalid_argument("Expected positive integer: " + value);
    return number;
}
tokenizer::Options parse(int argc, char** argv) {
    tokenizer::Options options;
    if (const auto* uri = std::getenv("MONGODB_URI")) options.uri = uri;
    options.run_id = "tokens-" + bsoncxx::oid{}.to_string();
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (i + 1 >= argc) throw std::invalid_argument("Missing value for " + key);
        std::string value = argv[++i];
        if (key == "--uri") options.uri = value;
        else if (key == "--database") options.database = value;
        else if (key == "--corpus-run") options.corpus_run = value;
        else if (key == "--run-id") options.run_id = value;
        else if (key == "--output") options.output = value;
        else if (key == "--source") options.source = value;
        else if (key == "--limit") options.limit = positive(value);
        else throw std::invalid_argument("Unknown option: " + key);
    }
    if (options.database.empty()) throw std::invalid_argument("Database must not be empty");
    auto alnum = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (options.run_id.empty() || options.run_id.size() > 80 || !alnum(options.run_id.front()) ||
        !std::all_of(options.run_id.begin(), options.run_id.end(), [&](unsigned char c) { return alnum(c) || c == '.' || c == '_' || c == '-'; }))
        throw std::invalid_argument("run-id must be 1-80 ASCII letters, digits, '.', '_' or '-'");
    return options;
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "Usage: tokenizer [--corpus-run ID] [--uri URI] [--database crawler]\n"
            << "  [--run-id ID] [--source NAME] [--limit N] [--output docx/runs]\n"
            << "Default: latest completed corpus. URI: MONGODB_URI or localhost:27017.\n";
        return 0;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        auto options = parse(argc, argv);
        const auto directory = options.output / options.run_id;
        if (std::filesystem::exists(directory)) throw std::invalid_argument("Output directory already exists; use a new run-id");
        const auto start = std::chrono::steady_clock::now();
        mongocxx::instance instance;
        tokenizer::Repository repository(options);
        repository.start();.
        options = repository.options();
        bool owns_directory = false;
        try {
            std::filesystem::create_directories(options.output);
            if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Cannot reserve output directory");
            owns_directory = true;
            auto result = repository.process([] { return stopped != 0; });
            if (stopped) throw std::runtime_error("Interrupted");
            result.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            tokenizer::write_report(options, result);
            const auto stats = tokenizer::statistics_document(result);
            repository.finish("completed", stats.view());
            std::cout << "Completed " << options.run_id << ": " << result.statistics.successful
                << " documents, " << result.statistics.total.tokens << " tokens; " << directory / "report.md" << '\n';
            return 0;
        } catch (const std::exception& e) {
            std::error_code ignored;
            if (owns_directory) {
                std::filesystem::remove(directory / "report.md", ignored);
                std::filesystem::remove(directory / "statistics.json", ignored);
            }
            try {
                const auto empty = bsoncxx::builder::basic::make_document();
                repository.finish("failed", empty.view(), e.what());
            } catch (const std::exception& failure) {
                std::cerr << "Cannot record failed run: " << failure.what() << '\n';
            }
            throw;
        }
    } catch (const std::exception& e) {
        std::cerr << "Tokenization failed: " << e.what() << '\n';
        return stopped ? 130 : 2;
    }
}
