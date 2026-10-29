#include "repository.hpp"
#include "logging.hpp"
#include "text/unicode.hpp"
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/update.hpp>
#include <mongocxx/options/replace.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/uri.hpp>
#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>

namespace lemmatizer {
namespace {
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::document;
using bsoncxx::builder::basic::array;
using View = bsoncxx::document::view;
using Value = bsoncxx::document::value;
using Clock = std::chrono::steady_clock;
bsoncxx::types::b_date now() { return bsoncxx::types::b_date(std::chrono::system_clock::now()); }
bsoncxx::types::b_date until(std::int64_t seconds) {
    return bsoncxx::types::b_date(std::chrono::system_clock::now() + std::chrono::seconds(seconds));
}
std::string string(View v, const char* key) {
    auto item = v[key];
    if (!item || item.type() != bsoncxx::type::k_string) throw std::invalid_argument(std::string("Expected string: ") + key);
    return std::string(item.get_string().value);
}
std::int64_t integer(View v, const char* key) {
    auto item = v[key];
    if (item && item.type() == bsoncxx::type::k_int32) return item.get_int32().value;
    if (item && item.type() == bsoncxx::type::k_int64) return item.get_int64().value;
    throw std::invalid_argument(std::string("Expected integer: ") + key);
}
std::string key_of(bsoncxx::document::element element) {
    return bsoncxx::to_json(make_document(kvp("id", element.get_value())));
}
std::string fingerprint(View v) {
    return sha256(std::string_view(reinterpret_cast<const char*>(v.data()), v.length()));
}
Value annotation(std::int64_t position, const Result& r) {
    array candidates;
    for (const auto& candidate : r.candidates) candidates.append(candidate);
    return make_document(kvp("position", position), kvp("lemma", r.lemma),
        kvp("lemma_candidates", candidates.view()), kvp("ambiguous", r.ambiguous()), kvp("method", r.method));
}
}

Repository::Repository(Options options) : options_(std::move(options)), client_(mongocxx::uri(options_.uri)),
    db_(client_[options_.database]), owner_(bsoncxx::oid{}.to_string()) {}
Repository::~Repository() { if (owns_) fail("Process exited before finalization"); }
Value Repository::owner_filter() const {
    return make_document(kvp("_id", options_.run_id), kvp("owner", owner_), kvp("state", "running"),
        kvp("lease_until", make_document(kvp("$gt", now()))));
}
void Repository::check_lease() {
    if (!owns_) throw std::runtime_error("Run lease not held");
    if (Clock::now() < next_renewal_) return;
    auto r = db_["lemmatization_runs"].update_one(owner_filter(),
        make_document(kvp("$set", make_document(kvp("lease_until", until(options_.lease_seconds))))));
    if (!r || r->matched_count() != 1) { owns_ = false; throw std::runtime_error("Run lease lost"); }
    next_renewal_ = Clock::now() + std::chrono::seconds(options_.lease_seconds / 3);
}
Value Repository::selection(bool remaining) const {
    document q;
    q.append(kvp("tokenization_run_id", options_.tokenization_run));
    if (!options_.source.empty()) q.append(kvp("source", options_.source));
    if (cutoff_) q.append(kvp("document_id", make_document(kvp("$lte", cutoff_->view()["id"].get_value()))));
    if (remaining && checkpoint_) q.append(kvp("_id", make_document(kvp("$gt", *checkpoint_))));
    return q.extract();
}
void Repository::start() {
    validate_options(options_);
    if (!db_.write_concern().is_acknowledged()) throw std::invalid_argument("Acknowledged writes required (w must not be 0)");
    auto runs = db_["lemmatization_runs"];
    std::optional<Value> previous;
    if (options_.resume) {
        auto saved = runs.find_one(make_document(kvp("_id", options_.run_id)));
        if (!saved) throw std::invalid_argument("Unknown lemmatization run");
        previous.emplace(saved->view());
        const auto v = previous->view();
        const auto state = string(v, "state");
        if (state == "completed" || state == "completed_with_errors") throw std::invalid_argument("Run already finalized");
        if (string(v, "schema_version") != "1" || string(v, "policy_version") != policy_version || string(v, "engine_commit") != engine_commit ||
            string(v, "main_sha256") != main_dictionary_sha256 || string(v, "affix_sha256") != affix_dictionary_sha256 ||
            string(v, "icu_version") != text::icu_version() || string(v, "unicode_version") != text::unicode_version())
            throw std::invalid_argument("Incompatible engine, dictionary, policy or Unicode version");
        options_.tokenization_run = string(v, "tokenization_run_id");
        options_.source = string(v, "source");
        options_.output = string(v, "output");
        options_.limit = integer(v, "limit");
        options_.batch_size = integer(v, "batch_size");
        options_.analyzer.cache_size = integer(v, "cache_size");
        options_.analyzer.max_token_codepoints = integer(v, "max_token_codepoints");
        options_.chunk_bytes = integer(v, "chunk_bytes");
        options_.lease_seconds = integer(v, "lease_seconds");
        stats_ = Statistics::load(v["statistics"].get_document().view());
        expected_ = integer(v, "expected_records");
        if (v["cutoff"]) cutoff_.emplace(v["cutoff"].get_document().view());
        if (v["checkpoint"]) checkpoint_ = v["checkpoint"].get_oid().value;
        validate_options(options_);
    } else if (runs.find_one(make_document(kvp("_id", options_.run_id)))) {
        throw std::invalid_argument("Run ID already exists; use --resume or a new ID");
    }
    mongocxx::options::find latest;
    latest.sort(make_document(kvp("finished_at", -1), kvp("_id", -1)));
    auto input = db_["tokenization_runs"].find_one(options_.tokenization_run.empty()
        ? make_document(kvp("state", "completed")) : make_document(kvp("_id", options_.tokenization_run)), latest);
    if (!input || string(input->view(), "state") != "completed") throw std::invalid_argument("No completed tokenization run");
    options_.tokenization_run = string(input->view(), "_id");
    corpus_run_ = string(input->view(), "corpus_run_id");
    if (string(input->view(), "rules_version") != "unicode-alnum-v1") throw std::invalid_argument("Unsupported tokenizer rules");
    fingerprint_ = fingerprint(input->view());
    if (previous && (string(previous->view(), "input_fingerprint") != fingerprint_ ||
        db_["tokenized_documents"].count_documents(selection(false)) != expected_))
        throw std::invalid_argument("Input run changed since checkpoint");
    if (previous && checkpoint_) {
        auto record = db_["tokenized_documents"].find_one(make_document(kvp("_id", *checkpoint_)));
        if (!record || fingerprint(record->view()) != string(previous->view(), "checkpoint_fingerprint"))
            throw std::invalid_argument("Checkpoint input changed");
    }
    if (!options_.source.empty() && !db_["tokenized_documents"].find_one(selection(false)))
        throw std::invalid_argument("Source absent from input run");
    if (!previous && options_.limit) {
        // Stream the existing (run, document, field, part) index instead of
        // grouping a million document IDs into an aggregation's memory.
        mongocxx::options::find documents;
        documents.sort(make_document(kvp("document_id", 1)));
        documents.projection(make_document(kvp("document_id", 1)));
        std::string prior;
        std::int64_t count = 0;
        for (auto row : db_["tokenized_documents"].find(selection(false), documents)) {
            if (!row["document_id"]) throw std::invalid_argument("Missing document_id");
            const auto key = key_of(row["document_id"]);
            if (count && key == prior) continue;
            cutoff_ = make_document(kvp("id", row["document_id"].get_value()));
            prior = key;
            if (++count == options_.limit) break;
        }
    }
    if (!previous) expected_ = db_["tokenized_documents"].count_documents(selection(false));
    mongocxx::options::index unique;
    unique.unique(true);
    db_["lemmatized_documents"].create_index(make_document(kvp("lemmatization_run_id", 1),
        kvp("input_record_id", 1), kvp("output_part", 1)), unique);
    db_["tokenized_documents"].create_index(make_document(kvp("tokenization_run_id", 1), kvp("_id", 1)));
    if (previous) {
        auto r = runs.update_one(make_document(kvp("_id", options_.run_id),
            kvp("statistics.records", stats_.records),
            kvp("state", make_document(kvp("$in", bsoncxx::builder::basic::make_array("running", "failed")))),
            kvp("lease_until", make_document(kvp("$lte", now())))),
            make_document(kvp("$set", make_document(kvp("state", "running"), kvp("owner", owner_),
                kvp("lease_until", until(options_.lease_seconds)), kvp("error", "")))));
        if (!r || r->matched_count() != 1) throw std::runtime_error("Another process owns this run");
    } else {
        document meta;
        meta.append(kvp("_id", options_.run_id), kvp("schema_version", "1"), kvp("state", "running"),
            kvp("owner", owner_), kvp("lease_until", until(options_.lease_seconds)), kvp("started_at", now()),
            kvp("tokenization_run_id", options_.tokenization_run), kvp("corpus_run_id", corpus_run_),
            kvp("input_fingerprint", fingerprint_), kvp("input_metadata", input->view()),
            kvp("policy_version", policy_version), kvp("engine", "XMorphy/DictMorphAnalyzer"), kvp("engine_commit", engine_commit),
            kvp("main_sha256", main_dictionary_sha256), kvp("affix_sha256", affix_dictionary_sha256),
            kvp("icu_version", text::icu_version()), kvp("unicode_version", text::unicode_version()), kvp("compiler", __VERSION__),
            kvp("build_type", LEMMATIZER_BUILD_TYPE), kvp("source", options_.source), kvp("limit", options_.limit),
            kvp("batch_size", options_.batch_size), kvp("cache_size", static_cast<std::int64_t>(options_.analyzer.cache_size)),
            kvp("max_token_codepoints", static_cast<std::int64_t>(options_.analyzer.max_token_codepoints)),
            kvp("chunk_bytes", static_cast<std::int64_t>(options_.chunk_bytes)), kvp("lease_seconds", options_.lease_seconds),
            kvp("output", std::filesystem::absolute(options_.output).string()),
            kvp("expected_records", expected_), kvp("statistics", stats_.bson()));
        if (cutoff_) meta.append(kvp("cutoff", cutoff_->view()));
        runs.insert_one(meta.view());
    }
    owns_ = true;
    next_renewal_ = Clock::now();
    check_lease();
}

void Repository::process(Analyzer& analyzer, const std::function<bool()>& interrupted, Hooks hooks) {
    const auto started = Clock::now();
    const auto prior_wall = stats_.wall_seconds;
    auto check = [&] { if (interrupted()) throw std::runtime_error("Interrupted"); check_lease(); };
    mongocxx::options::find find;
    find.sort(make_document(kvp("_id", 1)));
    find.batch_size(static_cast<std::int32_t>(options_.batch_size));
    auto cursor = db_["tokenized_documents"].find(selection(true), find);
    auto it = cursor.begin();
    while (it != cursor.end()) {
        check();
        // Bound both count and bytes; MongoDB's own batches do not bound this buffer.
        std::vector<Value> batch;
        std::size_t bytes = 0;
        while (it != cursor.end() && batch.size() < static_cast<std::size_t>(options_.batch_size)) {
            if (!batch.empty() && bytes + (*it).length() > 8 * 1024 * 1024) break;
            bytes += (*it).length();
            batch.emplace_back(*it);
            ++it;
        }
        array ids;
        for (const auto& row : batch) if (row.view()["document_id"]) ids.append(row.view()["document_id"].get_value());
        std::map<std::string, std::string> languages;
        mongocxx::options::find projection;
        projection.projection(make_document(kvp("document_id", 1), kvp("language", 1)));
        for (auto doc : db_["corpus_documents"].find(make_document(kvp("run_id", corpus_run_),
                 kvp("document_id", make_document(kvp("$in", ids.view())))), projection)) {
            std::string lang;
            if (doc["language"] && doc["language"].type() == bsoncxx::type::k_string) lang = string(doc, "language");
            languages[key_of(doc["document_id"])] = std::move(lang);
        }
        for (const auto& row : batch) {
            check();
            const auto v = row.view();
            if (!v["_id"] || v["_id"].type() != bsoncxx::type::k_oid || !v["document_id"])
                throw std::runtime_error("Input must have ObjectId _id and document_id");
            const auto id = v["_id"].get_oid().value;
            auto updated = stats_;
            ++updated.records;
            std::vector<Value> outputs;
            array values;
            std::size_t payload = 0;
            std::size_t element_index = 0;
            std::int64_t part = 0;
            auto base = [&](const char* status) {
                return make_document(kvp("lemmatization_run_id", options_.run_id), kvp("tokenization_run_id", options_.tokenization_run),
                    kvp("corpus_run_id", corpus_run_), kvp("input_record_id", id), kvp("document_id", v["document_id"].get_value()),
                    kvp("source", string(v, "source")), kvp("field", string(v, "field")), kvp("input_part", integer(v, "part")),
                    kvp("output_part", part), kvp("status", status));
            };
            auto emit = [&] {
                document out;
                auto metadata = base("ok");
                out.append(bsoncxx::builder::basic::concatenate(metadata.view()), kvp("annotations", values.view()));
                auto ready = out.extract();
                if (ready.view().length() > options_.chunk_bytes) throw std::invalid_argument("Output exceeds BSON budget");
                outputs.push_back(std::move(ready));
                ++part; values.clear(); payload = 0; element_index = 0;
            };
            double core_seconds = 0;
            const auto hits = analyzer.cache_hits(), misses = analyzer.cache_misses();
            try {
                if (string(v, "status") != "ok") throw std::invalid_argument("Input tokenization record has error status");
                if (!v["tokens"] || v["tokens"].type() != bsoncxx::type::k_array) throw std::invalid_argument("Expected tokens array");
                const auto lang = languages.find(key_of(v["document_id"]));
                if (lang == languages.end()) throw std::invalid_argument("Missing corpus document for language metadata");
                std::int64_t previous_position = -1;
                for (auto element : v["tokens"].get_array().value) {
                    check();
                    if (element.type() != bsoncxx::type::k_document) throw std::invalid_argument("Expected token document");
                    const auto token = element.get_document().view();
                    const auto position = integer(token, "position");
                    if (position < 0 || (previous_position >= 0 && position != previous_position + 1))
                        throw std::invalid_argument("Invalid token positions");
                    previous_position = position;
                    const auto normalized = string(token, "normalized");
                    const auto analysis_begin = Clock::now();
                    const auto result = analyzer.analyze(normalized, lang->second);
                    core_seconds += std::chrono::duration<double>(Clock::now() - analysis_begin).count();
                    auto value = annotation(position, result);
                    // Exact BSON-array element overhead: type + decimal index + NUL.
                    auto size = value.view().length() + 2 + std::to_string(element_index).size();
                    if (payload && payload + size + base("ok").view().length() + 32 > options_.chunk_bytes) {
                        emit();
                        size = value.view().length() + 3;
                    }
                    values.append(value.view()); payload += size; ++element_index;
                    ++updated.tokens; updated.input_bytes += normalized.size();
                    updated.changed += result.lemma != normalized;
                    updated.ambiguous += result.ambiguous(); updated.unknown += result.method == "unknown";
                    updated.skipped += result.method != "dictionary" && result.method != "unknown";
                }
                emit();
            } catch (const std::invalid_argument& error) {
                updated = stats_; ++updated.records; ++updated.errors;
                outputs.clear(); part = 0;
                document failure;
                auto metadata = base("error");
                failure.append(bsoncxx::builder::basic::concatenate(metadata.view()), kvp("error", error.what()));
                outputs.push_back(failure.extract());
            }
            updated.core_seconds += core_seconds;
            updated.cache_hits += analyzer.cache_hits() - hits;
            updated.cache_misses += analyzer.cache_misses() - misses;
            updated.output_parts += outputs.size();
            updated.input_bson_bytes += v.length();
            for (const auto& output : outputs) updated.output_bson_bytes += output.view().length();
            mongocxx::options::replace upsert;
            upsert.upsert(true);
            for (const auto& output : outputs) {
                check();
                db_["lemmatized_documents"].replace_one(make_document(kvp("lemmatization_run_id", options_.run_id),
                    kvp("input_record_id", id), kvp("output_part", integer(output.view(), "output_part"))), output.view(), upsert);
            }
            if (hooks.after_output) hooks.after_output();
            check();
            updated.wall_seconds = prior_wall + std::chrono::duration<double>(Clock::now() - started).count();
            auto ack = db_["lemmatization_runs"].update_one(owner_filter(), make_document(kvp("$set", make_document(
                kvp("checkpoint", id), kvp("checkpoint_fingerprint", fingerprint(v)), kvp("statistics", updated.bson())))));
            if (!ack || ack->matched_count() != 1) { owns_ = false; throw std::runtime_error("Checkpoint lease lost"); }
            checkpoint_ = id; stats_ = updated;
            if (stats_.records % 1000 == 0) log_event("progress", options_.run_id, "", stats_.records);
        }
    }
    check();
    if (stats_.records != expected_ || db_["tokenized_documents"].count_documents(selection(false)) != expected_)
        throw std::runtime_error("Input count changed or checkpoint incomplete");
    auto first = make_document(kvp("lemmatization_run_id", options_.run_id), kvp("output_part", 0));
    if (db_["lemmatized_documents"].count_documents(first.view()) != expected_ ||
        db_["lemmatized_documents"].count_documents(make_document(kvp("lemmatization_run_id", options_.run_id))) != stats_.output_parts)
        throw std::runtime_error("Output count does not match checkpoint");
}
Value Repository::metadata() const {
    auto value = db_["lemmatization_runs"].find_one(make_document(kvp("_id", options_.run_id)));
    if (!value) throw std::runtime_error("Run metadata missing");
    return Value(value->view());
}
void Repository::finish() {
    check_lease();
    auto result = db_["lemmatization_runs"].update_one(owner_filter(), make_document(kvp("$set", make_document(
        kvp("state", stats_.errors ? "completed_with_errors" : "completed"), kvp("finished_at", now()),
        kvp("statistics", stats_.bson()), kvp("lease_until", now()), kvp("error", "")))));
    if (!result || result->matched_count() != 1) throw std::runtime_error("Finalization lease lost");
    owns_ = false;
}
void Repository::fail(const std::string& reason) noexcept {
    if (!owns_) return;
    try {
        db_["lemmatization_runs"].update_one(owner_filter(), make_document(kvp("$set", make_document(
            kvp("state", "failed"), kvp("error", reason), kvp("lease_until", now())))));
    } catch (...) {}
    owns_ = false;
}
}
