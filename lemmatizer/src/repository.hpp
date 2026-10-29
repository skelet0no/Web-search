#pragma once
#include "options.hpp"
#include "statistics.hpp"
#include <functional>
#include <optional>
#include <bsoncxx/oid.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/database.hpp>
namespace lemmatizer {
struct Hooks {
    // Test seam: after all outputs, before checkpoint; production leaves empty.
    std::function<void()> after_output;
};
class Repository {
public:
    explicit Repository(Options options);
    ~Repository();
    void start();
    void process(Analyzer& analyzer, const std::function<bool()>& interrupted, Hooks hooks = {});
    void finish();
    void fail(const std::string& reason) noexcept;
    void check_lease();
    const Options& options() const { return options_; }
    const Statistics& statistics() const { return stats_; }
    bsoncxx::document::value metadata() const;
private:
    bsoncxx::document::value selection(bool remaining) const;
    bsoncxx::document::value owner_filter() const;
    Options options_;
    mongocxx::client client_;
    mongocxx::database db_;
    Statistics stats_;
    std::string owner_, corpus_run_, fingerprint_;
    std::optional<bsoncxx::document::value> cutoff_;
    std::optional<bsoncxx::oid> checkpoint_;
    std::int64_t expected_ = 0;
    bool owns_ = false;
    std::chrono::steady_clock::time_point next_renewal_;
};
}
