#include "analysis.hpp"
#include "input.hpp"
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/options/find.hpp>
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
void check() { if (stopped) throw std::runtime_error("Interrupted"); }
std::string string(bsoncxx::document::view v, const char* key) {
    auto e = v[key];
    if (!e || e.type() != bsoncxx::type::k_string) throw std::runtime_error(std::string("Expected string: ") + key);
    return std::string(e.get_string().value);
}
}
int main(int argc, char** argv) {
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
    std::filesystem::path pending;
    bool owns_pending = false;
    try {
        std::string selected;
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "Usage: zipf [--lemmatization-run ID]\n"
                         "Latest completed lemmatization, database crawler, text fields.\n"
                         "MONGODB_URI defaults to mongodb://127.0.0.1:27017.\n"
                         "Output: docx/runs/zipf-<id>/{zipf.png,frequencies.csv,statistics.json,report.md}\n";
            std::cout << "With --lemmatization-run ID, output directory is zipf-ID; existing results are not overwritten.\n";
            return 0;
        }
        if (argc != 1) {
            if (argc != 3 || std::string(argv[1]) != "--lemmatization-run" || !*argv[2])
                throw std::invalid_argument("Usage: zipf [--lemmatization-run ID]");
            selected = argv[2];
            const auto alnum = [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            };
            if (selected.size() > 80 || !alnum(selected.front()) ||
                !std::all_of(selected.begin(), selected.end(), [&](unsigned char c) {
                    return alnum(c) || c == '.' || c == '_' || c == '-';
                }))
                throw std::invalid_argument("run ID must be 1-80 ASCII letters, digits, '.', '_' or '-', starting with a letter/digit");
        }
        const auto output = std::filesystem::path("docx/runs") /
            ("zipf-" + (selected.empty() ? bsoncxx::oid{}.to_string() : selected));
        if (std::filesystem::exists(output))
            throw std::runtime_error("Output directory already exists: " + output.string() + "; existing results are not overwritten");
        mongocxx::instance instance;
        const char* env = std::getenv("MONGODB_URI");
        std::string uri = env && *env ? env : "mongodb://127.0.0.1:27017";
        if (uri.find("serverSelectionTimeoutMS=") == std::string::npos) {
            if (uri.find('?') == std::string::npos) {
                if (uri.find('/', uri.find("://") + 3) == std::string::npos) uri += '/';
                uri += '?';
            } else uri += '&';
            uri += "serverSelectionTimeoutMS=10000";
        }
        mongocxx::client client{mongocxx::uri(uri)};
        auto db = client["crawler"];
        using bsoncxx::builder::basic::make_document;
        using bsoncxx::builder::basic::kvp;
        mongocxx::options::find latest;
        latest.sort(make_document(kvp("finished_at", -1), kvp("_id", -1)));
        auto query = selected.empty() ? make_document(kvp("state", "completed")) : make_document(kvp("_id", selected));
        auto run = db["lemmatization_runs"].find_one(query.view(), latest);
        if (!run || string(run->view(), "state") != "completed")
            throw std::runtime_error("No completed lemmatization run. Run lemmatizer first; completed_with_errors is not accepted.");
        auto metadata = run->view();
        const auto id = string(metadata, "_id");
        const auto tokenization = string(metadata, "tokenization_run_id");
        const auto corpus = string(metadata, "corpus_run_id");
        std::cout << "Analyzing lemmatization " << id << " (text fields)\n";
        auto filter = make_document(kvp("lemmatization_run_id", id), kvp("field", "text"));
        mongocxx::options::find options;
        options.batch_size(100);
        options.projection(make_document(kvp("annotations", 1), kvp("status", 1)));
        std::unordered_map<std::string, std::uint64_t> counts;
        std::uint64_t records = 0;
        for (const auto row : db["lemmatized_documents"].find(filter.view(), options)) {
            check();
            zipf::count_record(row, counts);
            if (++records % 10000 == 0) std::cout << "Processed " << records << " text parts\n";
        }
        if (records != std::uint64_t(db["lemmatized_documents"].count_documents(filter.view())))
            throw std::runtime_error("Input record count changed during analysis");
        auto d = zipf::analyze(counts);
        check();
        pending = output.string() + ".tmp";
        std::filesystem::create_directories(output.parent_path());
        if (!std::filesystem::create_directory(pending)) throw std::runtime_error("Cannot reserve output directory");
        owns_pending = true;
        zipf::plot(pending / "zipf.png", d);
        zipf::report(pending, d, bsoncxx::to_json(metadata, bsoncxx::ExtendedJsonMode::k_relaxed), id, tokenization, corpus, records);
        check();
        if (std::filesystem::exists(output))
            throw std::runtime_error("Output directory already exists: " + output.string());
        std::filesystem::rename(pending, output); owns_pending = false;
        std::cout << "Completed: " << d.tokens << " occurrences, " << d.terms.size() << " terms; " << output / "zipf.png" << '\n';
        return 0;
    } catch (const std::exception& e) {
        if (owns_pending) { std::error_code ignored; std::filesystem::remove_all(pending, ignored); }
        std::cerr << "Zipf analysis failed: " << e.what() << '\n';
        return stopped ? 130 : 2;
    }
}
