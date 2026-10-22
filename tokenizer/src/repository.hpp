#pragma once
#include "tokenizer/statistics.hpp"
#include <bsoncxx/document/value.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/database.hpp>
#include <filesystem>
#include <functional>

namespace tokenizer {
struct Options {
    std::string uri = "mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=10000";
    std::string database = "crawler", corpus_run, run_id;
    std::filesystem::path output = "docx/runs";
    std::int64_t limit = 0;
    std::string source;
};
struct RunResult {
    Statistics statistics;
    double wall_seconds = 0;
};
class Repository {
public:
    explicit Repository(const Options& options);
    void start();
    const Options& options() const { return options_; }
    RunResult process(const std::function<bool()>& interrupted);
    void finish(const std::string& state, bsoncxx::document::view statistics,
                const std::string& error = "");
private:
    Options options_;
    mongocxx::client client_;
    mongocxx::database db_;
};
bsoncxx::document::value statistics_document(const RunResult& result);
void write_report(const Options& options, const RunResult& result);
}
