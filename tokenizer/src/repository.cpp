#include "repository.hpp"
#include "tokenizer/tokenizer.hpp"
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/uri.hpp>
#include <chrono>
#include <algorithm>
#include <iostream>
#include <array>

namespace tokenizer {
namespace {
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::document;
using bsoncxx::builder::basic::array;
using Clock = std::chrono::steady_clock;
constexpr std::size_t chunk_budget = 4 * 1024 * 1024;
constexpr std::size_t batch_budget = 8 * 1024 * 1024;
std::int64_t number(std::size_t n) { return static_cast<std::int64_t>(n); }
std::string text(bsoncxx::document::view doc, const char* key, bool optional = false) {
    const auto value = doc[key];
    if ((!value || value.type() == bsoncxx::type::k_null) && optional) return "";
    if (!value || value.type() != bsoncxx::type::k_string)
        throw std::invalid_argument(std::string("Expected string field: ") + key);
    return std::string(value.get_string().value);
}
bsoncxx::types::b_date now() { return bsoncxx::types::b_date(std::chrono::system_clock::now()); }
std::vector<bsoncxx::document::value> chunks(const Options& options,
        bsoncxx::document::view original, const std::string& source, const char* field,
        const std::vector<Token>& tokens) {
    std::vector<bsoncxx::document::value> result;
    array values;
    std::size_t bytes = 0, part = 0;
    auto emit = [&] {
        result.push_back(make_document(kvp("tokenization_run_id", options.run_id),
            kvp("corpus_run_id", options.corpus_run), kvp("document_id", original["document_id"].get_value()),
            kvp("source", source), kvp("field", field), kvp("part", number(part++)),
            kvp("status", "ok"), kvp("rules_version", rules_version), kvp("tokens", values.view())));
        values.clear(); bytes = 0;
    };
    for (const auto& token : tokens) {
        auto value = make_document(kvp("position", number(token.position)),
            kvp("byte_begin", number(token.byte_begin)), kvp("byte_end", number(token.byte_end)),
            kvp("normalized", token.normalized), kvp("codepoints", number(token.codepoints)));
        if (value.view().length() > chunk_budget)
            throw std::invalid_argument("Single token exceeds 4 MiB storage budget");
        if (bytes && bytes + value.view().length() + 32 > chunk_budget) emit();
        bytes += value.view().length() + 32;
        values.append(value.view());
    }
    if (bytes || result.empty()) emit();
    return result;
}
}

Repository::Repository(const Options& options)
    : options_(options), client_(mongocxx::uri(options.uri)), db_(client_[options.database]) {}

void Repository::start() {
    if (!db_.write_concern().is_acknowledged())
        throw std::invalid_argument("Acknowledged MongoDB writes are required (w must not be 0)");
    mongocxx::options::find latest;
    latest.sort(make_document(kvp("finished_at", -1), kvp("_id", -1)));
    const auto filter = options_.corpus_run.empty()
        ? make_document(kvp("state", "completed")) : make_document(kvp("_id", options_.corpus_run));
    const auto run = db_["corpus_runs"].find_one(filter.view(), latest);
    if (!run || text(run->view(), "state") != "completed")
        throw std::invalid_argument("No completed corpus run. Run corpus extract or check --corpus-run.");
    options_.corpus_run = text(run->view(), "_id");
    if (!options_.source.empty() && !db_["corpus_documents"].find_one(
            make_document(kvp("run_id", options_.corpus_run), kvp("source", options_.source))))
        throw std::invalid_argument("Source is absent from selected corpus: " + options_.source);
    mongocxx::options::index unique;
    unique.unique(true);
    db_["tokenized_documents"].create_index(make_document(kvp("tokenization_run_id", 1),
        kvp("document_id", 1), kvp("field", 1), kvp("part", 1)), unique);
    db_["tokenization_runs"].insert_one(make_document(kvp("_id", options_.run_id),
        kvp("state", "running"), kvp("corpus_run_id", options_.corpus_run), kvp("rules_version", rules_version),
        kvp("icu_version", icu_version()), kvp("unicode_version", unicode_version()),
        kvp("started_at", now()), kvp("limit", options_.limit), kvp("source", options_.source)));
}

RunResult Repository::process(const std::function<bool()>& interrupted) {
    RunResult result;
    document query;
    query.append(kvp("run_id", options_.corpus_run), kvp("status", "ok"));
    if (!options_.source.empty()) query.append(kvp("source", options_.source));
    mongocxx::options::find find;
    find.sort(make_document(kvp("document_id", 1)));
    find.projection(make_document(kvp("document_id", 1), kvp("title", 1), kvp("text", 1),
                                 kvp("source", 1), kvp("_id", 0)));
    find.batch_size(100);
    if (options_.limit) find.limit(options_.limit);
    auto output = db_["tokenized_documents"];
    std::vector<bsoncxx::document::value> batch;
    std::size_t batch_bytes = 0;
    auto flush = [&] {
        if (interrupted()) throw std::runtime_error("Interrupted");
        if (!batch.empty()) output.insert_many(batch);
        batch.clear(); batch_bytes = 0;
    };
    auto enqueue = [&](bsoncxx::document::value value) {
        if (!batch.empty() && (batch_bytes + value.view().length() > batch_budget || batch.size() >= 100)) flush();
        batch_bytes += value.view().length();
        batch.push_back(std::move(value));
    };
    for (auto original : db_["corpus_documents"].find(query.view(), find)) {
        if (interrupted()) throw std::runtime_error("Interrupted");
        if (!original["document_id"]) throw std::runtime_error("Corpus record missing document_id");
        ++result.statistics.selected;
        std::string source = "unknown";
        std::array<std::string, 2> texts;
        std::array<std::vector<Token>, 2> tokens;
        std::array<double, 2> seconds{};
        std::vector<bsoncxx::document::value> parts;
        try {
            source = text(original, "source", true);
            if (source.empty()) source = "unknown";
            texts = {text(original, "title", true), text(original, "text")};
            for (std::size_t i = 0; i < texts.size(); ++i) {
                const auto start = Clock::now();
                tokens[i] = tokenize(texts[i]);
                seconds[i] = std::chrono::duration<double>(Clock::now() - start).count();
                auto field_parts = chunks(options_, original, source, i == 0 ? "title" : "text", tokens[i]);
                for (auto& part : field_parts) parts.push_back(std::move(part));
            }
        } catch (const std::invalid_argument& e) {
            ++result.statistics.errors;
            enqueue(make_document(kvp("tokenization_run_id", options_.run_id),
                kvp("corpus_run_id", options_.corpus_run), kvp("document_id", original["document_id"].get_value()),
                kvp("source", source), kvp("field", "document"), kvp("part", 0),
                kvp("status", "error"), kvp("error", e.what())));
            continue;
        }
        for (auto& part : parts) enqueue(std::move(part));
        ++result.statistics.successful;
        for (std::size_t i = 0; i < texts.size(); ++i)
            result.statistics.total.add(texts[i].size(), tokens[i], seconds[i]);
        if (result.statistics.selected % 1000 == 0)
            std::cerr << "Processed " << result.statistics.selected << " documents\n";
    }
    flush();
    return result;
}

void Repository::finish(const std::string& state, bsoncxx::document::view statistics, const std::string& error) {
    auto update = make_document(kvp("state", state), kvp("finished_at", now()),
        kvp("statistics", statistics), kvp("error", error));
    auto result = db_["tokenization_runs"].update_one(make_document(kvp("_id", options_.run_id), kvp("state", "running")),
        make_document(kvp("$set", update.view())));
    if (!result || result->matched_count() != 1) throw std::runtime_error("Cannot finalize tokenization run");
}
}
