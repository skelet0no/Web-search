#include "repository.hpp"
#include "report.hpp"
#include "tokenizer/tokenizer.hpp"
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/uri.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace lemmatizer;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::array;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class F> void rejects(F action, const char* reason) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    require(failed, reason);
}
struct Fixture {
    mongocxx::client client;
    mongocxx::database db;
    Options options;
    Fixture(const std::string& uri, const std::string& dictionary) : client(mongocxx::uri(uri)),
        db(client["lemmatizer_test_" + bsoncxx::oid{}.to_string()]) {
        options.uri = uri; options.dictionary = dictionary; options.database = std::string(db.name());
        options.tokenization_run = "input"; options.run_id = "result";
        options.output = std::filesystem::temp_directory_path() / options.database;
        db["tokenization_runs"].insert_one(make_document(kvp("_id", "input"), kvp("state", "completed"),
            kvp("corpus_run_id", "corpus"), kvp("rules_version", "unicode-alnum-v1")));
    }
    ~Fixture() {
        try { db.drop(); } catch (...) {}
        std::error_code error;
        std::filesystem::remove_all(options.output, error);
    }
    bsoncxx::oid add(std::string text = "игры стали ps5", std::string source = "stopgame", std::string language = "ru") {
        bsoncxx::oid id;
        db["corpus_documents"].insert_one(make_document(kvp("run_id", "corpus"), kvp("document_id", id), kvp("language", language)));
        add_part(id, text, source, "text", 0);
        return id;
    }
    void add_part(bsoncxx::oid id, const std::string& text, const std::string& source,
                  const std::string& field, std::int64_t part, std::int64_t offset = 0) {
        array tokens;
        for (const auto& t : tokenizer::tokenize(text)) tokens.append(make_document(
            kvp("position", static_cast<std::int64_t>(t.position) + offset), kvp("normalized", t.normalized),
            kvp("byte_begin", static_cast<std::int64_t>(t.byte_begin)), kvp("byte_end", static_cast<std::int64_t>(t.byte_end))));
        db["tokenized_documents"].insert_one(make_document(kvp("tokenization_run_id", "input"), kvp("document_id", id),
            kvp("source", source), kvp("field", field), kvp("part", part), kvp("status", "ok"), kvp("tokens", tokens.view())));
    }
    void run(Backend& engine, Options o) {
        Repository repository(o); repository.start();
        Analyzer analyzer(engine, repository.options().analyzer);
        repository.process(analyzer, [] { return false; });
        auto meta = repository.metadata();
        write_report(repository.options(), repository.statistics(), meta.view());
        repository.finish();
    }
    std::string state() {
        auto r = db["lemmatization_runs"].find_one(make_document(kvp("_id", options.run_id)));
        return std::string(r->view()["state"].get_string().value);
    }
};
int main(int argc, char** argv) {
    const char* uri = argc > 2 ? argv[2] : std::getenv("LEMMATIZER_TEST_URI");
    if (!uri || argc < 2) { std::cout << "Set LEMMATIZER_TEST_URI to run MongoDB integration tests\n"; return 77; }
    try {
        mongocxx::instance instance;
        auto engine = make_xmorphy(argv[1]);
        {
            Fixture f(uri, argv[1]);
            auto id = f.add();
            const auto before = f.db["tokenized_documents"].find_one({});
            f.run(*engine, f.options);
            require(f.state() == "completed", "Successful finalization");
            auto out = f.db["lemmatized_documents"].find_one({});
            auto annotations = out->view()["annotations"].get_array().value;
            require(std::string(annotations[0].get_document().view()["lemma"].get_string().value) == "игра", "Real lemma persisted");
            require(annotations[1].get_document().view()["ambiguous"].get_bool().value, "Ambiguity persisted");
            auto after = f.db["tokenized_documents"].find_one({});
            require(bsoncxx::to_json(before->view()) == bsoncxx::to_json(after->view()), "Input immutable");
            require(out->view()["document_id"].get_oid().value == id, "Original document link");
            require(std::filesystem::exists(f.options.output / "result" / "statistics.json"), "Report produced");
            rejects([&] { f.run(*engine, f.options); }, "Duplicate run rejected");
            auto resumed = f.options; resumed.resume = true;
            rejects([&] { f.run(*engine, resumed); }, "Completed resume rejected");
        }
        {
            Fixture f(uri, argv[1]);
            f.add(); f.add("игрой", "vgtimes");
            auto o = f.options; o.source = "vgtimes"; o.limit = 1;
            f.run(*engine, o);
            require(f.db["lemmatized_documents"].count_documents({}) == 1, "Source filter and document limit");
        }
        {
            Fixture f(uri, argv[1]);
            auto id = f.add("игры");
            f.add_part(id, "игрой", "stopgame", "title", 0);
            f.add_part(id, "игру", "stopgame", "text", 1, 1);
            f.add("другой");
            auto o = f.options; o.limit = 1;
            f.run(*engine, o);
            require(f.db["lemmatized_documents"].count_documents({}) == 3, "Limit includes all fields and parts");
        }
        {
            Fixture f(uri, argv[1]);
            f.add("игры", "stopgame", "en-US");
            f.run(*engine, f.options);
            auto out = f.db["lemmatized_documents"].find_one({});
            auto a = out->view()["annotations"].get_array().value[0].get_document().view();
            require(std::string(a["lemma"].get_string().value) == "игры", "Document language preserved");
        }
        {
            Fixture f(uri, argv[1]);
            f.run(*engine, f.options);
            require(f.state() == "completed" && f.db["lemmatized_documents"].count_documents({}) == 0, "Empty run");
        }
        {
            Fixture f(uri, argv[1]);
            f.add("");
            f.run(*engine, f.options);
            auto out = f.db["lemmatized_documents"].find_one({});
            require(out->view()["annotations"].get_array().value.empty(), "Empty field retained");
        }
        {
            Fixture f(uri, argv[1]);
            std::string text;
            for (int i = 0; i < 1000; ++i) text += "стали ";
            f.add(text);
            auto o = f.options; o.chunk_bytes = 1024;
            f.run(*engine, o);
            require(f.db["lemmatized_documents"].count_documents({}) > 1, "Expanded output chunked");
            mongocxx::options::find sorted; sorted.sort(make_document(kvp("output_part", 1)));
            std::int64_t position = 0;
            for (auto out : f.db["lemmatized_documents"].find({}, sorted)) {
                require(out.length() <= 1024, "Actual BSON bound");
                for (auto a : out["annotations"].get_array().value)
                    require(a.get_document().view()["position"].get_int64().value == position++, "Continuous positions");
            }
            require(position == 1000, "Every occurrence retained");
        }
        {
            Fixture f(uri, argv[1]);
            f.add(); f.add("игрой");
            {
                Repository repo(f.options); repo.start(); Analyzer a(*engine);
                rejects([&] { repo.process(a, [] { return false; }, {[] { throw std::runtime_error("Simulated crash before checkpoint"); }}); }, "Crash injected");
                repo.fail("Simulated interruption");
            }
            require(f.db["lemmatized_documents"].count_documents({}) == 1, "Partial output exists");
            auto o = f.options; o.resume = true;
            f.run(*engine, o);
            require(f.state() == "completed" && f.db["lemmatized_documents"].count_documents({}) == 2, "Resume without duplicate outputs");
            auto run = f.db["lemmatization_runs"].find_one({});
            require(run->view()["statistics"].get_document().view()["tokens"].get_int64().value == 4, "Resume without double count");
        }
        {
            Fixture f(uri, argv[1]); f.add(); f.add("игрой");
            {
                Repository r(f.options); r.start(); Analyzer a(*engine);
                int count = 0;
                rejects([&] { r.process(a, [] { return false; }, {[&] {
                    if (++count == 2) throw std::runtime_error("Crash after first checkpoint");
                }}); }, "Second record crash injected");
                r.fail("interrupted");
            }
            auto saved = f.db["lemmatization_runs"].find_one({});
            require(saved->view()["statistics"].get_document().view()["records"].get_int64().value == 1, "First checkpoint persisted");
            auto o = f.options; o.resume = true;
            f.run(*engine, o);
            require(f.db["lemmatized_documents"].count_documents({}) == 2, "Resume from nonempty checkpoint");
        }
        {
            Fixture f(uri, argv[1]); f.add();
            {
                Repository r(f.options); r.start(); Analyzer a(*engine);
                r.process(a, [] { return false; }); r.fail("Before finalization");
            }
            f.db["tokenized_documents"].update_one({}, make_document(kvp("$set", make_document(kvp("source", "changed")))));
            auto o = f.options; o.resume = true;
            rejects([&] { f.run(*engine, o); }, "Modified checkpoint input rejected");
        }
        {
            Fixture f(uri, argv[1]);
            f.add();
            Repository first(f.options); first.start();
            auto o = f.options; o.resume = true;
            rejects([&] { Repository second(o); second.start(); }, "Concurrent resume blocked");
            Analyzer a(*engine);
            rejects([&] { first.process(a, [] { return true; }); }, "Signal interruption");
            first.fail("Interrupted");
            f.db["lemmatization_runs"].update_one({}, make_document(kvp("$set", make_document(kvp("policy_version", "changed")))));
            rejects([&] { f.run(*engine, o); }, "Policy mismatch rejected");
        }
        {
            Fixture f(uri, argv[1]);
            f.add();
            Repository r(f.options); r.start(); Analyzer a(*engine);
            rejects([&] { r.process(a, [] { return false; }, {[&] {
                f.db["lemmatization_runs"].update_one({}, make_document(kvp("$set", make_document(kvp("owner", "another")))));
            }}); }, "Lease loss prevents checkpoint");
            auto run = f.db["lemmatization_runs"].find_one({});
            require(!run->view()["checkpoint"], "Stolen lease never checkpointed");
        }
        {
            Fixture f(uri, argv[1]);
            f.add();
            f.db["tokenized_documents"].update_one({}, make_document(kvp("$set", make_document(kvp("tokens", 123)))));
            f.run(*engine, f.options);
            require(f.state() == "completed_with_errors", "Bad record counted explicitly");
            auto out = f.db["lemmatized_documents"].find_one({});
            require(std::string(out->view()["status"].get_string().value) == "error", "No partial annotations");
        }
        {
            Fixture f(uri, argv[1]); f.add();
            f.db["tokenization_runs"].update_one({}, make_document(kvp("$set", make_document(kvp("state", "running")))));
            rejects([&] { f.run(*engine, f.options); }, "Unfinished input rejected");
            require(f.db["lemmatization_runs"].count_documents({}) == 0, "No run after invalid input");
        }
        {
            Fixture f(uri, argv[1]); f.add();
            auto o = f.options; o.source = "missing";
            rejects([&] { f.run(*engine, o); }, "Unknown source rejected");
            o = f.options; o.uri += (o.uri.find('?') == std::string::npos ? "?w=0" : "&w=0");
            rejects([&] { f.run(*engine, o); }, "w=0 rejected");
        }
        {
            Fixture f(uri, argv[1]); f.add();
            auto o = f.options; o.output = "/proc/lemmatizer-test-output";
            rejects([&] { f.run(*engine, o); }, "Report error propagated");
            require(f.state() == "failed", "Report error leaves resumable failed run");
        }
        {
            // Expired running lease simulates a killed process, unlike fail().
            Fixture f(uri, argv[1]); f.add();
            Repository abandoned(f.options); abandoned.start();
            f.db["lemmatization_runs"].update_one({}, make_document(kvp("$set", make_document(
                kvp("owner", "dead-process"), kvp("lease_until", bsoncxx::types::b_date(std::chrono::milliseconds(0)))))));
            auto o = f.options; o.resume = true;
            f.run(*engine, o);
            require(f.state() == "completed", "Expired crash lease recovered");
        }
        {
            Options o; o.dictionary = argv[1]; o.run_id = "../bad";
            rejects([&] { validate_options(o); }, "Unsafe output ID");
            const char* args[] = {"lemmatizer", "--resume", "saved", "--limit", "1"};
            rejects([&] { parse_options(5, const_cast<char**>(args)); }, "Resume selection override rejected");
            const char* bad[] = {"lemmatizer", "--cache-size", "-1"};
            rejects([&] { parse_options(3, const_cast<char**>(bad)); }, "Negative cache rejected");
        }
        if (argc > 3 && std::string(argv[3]) == "--acceptance") {
            Fixture f(uri, argv[1]);
            auto started = std::chrono::steady_clock::now();
            for (int i = 0; i < 1000; ++i) f.add("Обзоры новых игр игры игрой стали PS5 RTX4090 ёжами", i % 3 == 0 ? "igromania" : i % 3 == 1 ? "stopgame" : "vgtimes");
            f.run(*engine, f.options);
            require(f.db["lemmatized_documents"].count_documents({}) == 1000, "1000 saved synthetic documents");
            auto run = f.db["lemmatization_runs"].find_one({});
            std::cout << "Synthetic acceptance (not corpus quality): " << bsoncxx::to_json(run->view()["statistics"].get_document().view())
                << "; including fixture setup seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << '\n';
            std::ifstream status("/proc/self/status");
            std::string line;
            while (std::getline(status, line)) if (line.rfind("VmHWM:", 0) == 0) std::cout << "Test-process peak RSS: " << line << '\n';
        }
        std::cout << "All MongoDB lemmatizer tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
