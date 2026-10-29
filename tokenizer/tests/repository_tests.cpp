#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/uri.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdlib>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
namespace fs = std::filesystem;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::int64_t number(bsoncxx::document::element e) {
    return e.type() == bsoncxx::type::k_int64 ? e.get_int64().value : e.get_int32().value;
}
std::string string(bsoncxx::document::element e) { return std::string(e.get_string().value); }
struct Fixture {
    std::string uri, binary, database;
    mongocxx::client client;
    mongocxx::database db;
    fs::path output;
    Fixture(const std::string& address, const std::string& executable)
        : uri(address), binary(executable), database("tokenizer_test_" + bsoncxx::oid{}.to_string()),
          client(mongocxx::uri(uri)), db(client[database]), output(fs::temp_directory_path() / database) {
        db["corpus_runs"].insert_one(make_document(kvp("_id", "corpus"), kvp("state", "completed")));
    }
    ~Fixture() {
        try { db.drop(); } catch (...) {}
        std::error_code ignored;
        fs::remove_all(output, ignored);
    }
    bsoncxx::oid add(const std::string& source = "stopgame", const std::string& text = "PS5 C++",
                    const std::string& status = "ok", const std::string& run = "corpus") {
        bsoncxx::oid id;
        db["corpus_documents"].insert_one(make_document(kvp("run_id", run), kvp("document_id", id),
            kvp("status", status), kvp("source", source), kvp("title", "ЁЖ"), kvp("text", text)));
        return id;
    }
    void run(const std::string& id = "tokens", std::vector<std::string> extra = {},
             bool success = true, bool explicit_corpus = true) {
        std::vector<std::string> args{binary, "--uri", uri, "--database", database,
            "--run-id", id, "--output", output.string()};
        if (explicit_corpus) args.insert(args.end(), {"--corpus-run", "corpus"});
        args.insert(args.end(), extra.begin(), extra.end());
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        const auto pid = fork();
        if (pid < 0) throw std::runtime_error("fork failed");
        if (pid == 0) { execv(argv[0], argv.data()); _exit(127); }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) throw std::runtime_error("waitpid failed");
        }
        require(WIFEXITED(status), "Tokenizer terminated by signal");
        require(WEXITSTATUS(status) != 127, "Cannot execute tokenizer binary");
        require((WEXITSTATUS(status) == 0) == success, "Unexpected tokenizer exit code");
    }
    bsoncxx::document::value metadata() {
        auto result = db["tokenization_runs"].find_one({});
        require(bool(result), "Missing run metadata");
        return std::move(*result);
    }
};
int main(int argc, char** argv) {
    const char* uri = std::getenv("TOKENIZER_TEST_URI");
    if (!uri) { std::cout << "Set TOKENIZER_TEST_URI to run MongoDB integration tests\n"; return 77; }
    if (argc != 2) { std::cerr << "Expected tokenizer binary path\n"; return 1; }
    try {
        mongocxx::instance instance;
        const auto binary = fs::absolute(argv[1]).string();
        {
            Fixture f(uri, binary); auto id = f.add();
            auto before = f.db["corpus_documents"].find_one(make_document(kvp("document_id", id)));
            f.add("stopgame", "ignored", "empty"); f.run();
            auto meta = f.metadata(); auto stats = meta.view()["statistics"].get_document().view();
            auto total = stats["total"].get_document().view();
            require(string(meta.view()["state"]) == "completed", "Completed run");
            require(number(stats["selected_documents"]) == 1, "Only ok input selected");
            require(number(total["tokens"]) == 3 && number(total["token_codepoints"]) == 6, "Token statistics");
            require(total["average_token_length"].get_double().value == 2, "Average length");
            require(number(total["input_utf8_bytes"]) == static_cast<std::int64_t>(std::string("ЁЖPS5 C++").size()), "Input bytes");
            auto body = f.db["tokenized_documents"].find_one(make_document(kvp("field", "text")));
            require(bool(body), "Body persisted");
            auto tokens = body->view()["tokens"].get_array().value;
            require(body->view()["document_id"].get_oid().value == id, "Original document link");
            require(std::distance(tokens.begin(), tokens.end()) == 2, "Body token count");
            require(string(tokens[0].get_document().view()["normalized"]) == "ps5" &&
                string(tokens[1].get_document().view()["normalized"]) == "c", "Normalized tokens");
            require(number(tokens[1].get_document().view()["byte_begin"]) == 4, "Byte offset");
            auto after = f.db["corpus_documents"].find_one(make_document(kvp("document_id", id)));
            require(bsoncxx::to_json(before->view()) == bsoncxx::to_json(after->view()), "Input immutable");
            require(fs::exists(f.output / "tokens/report.md"), "Markdown report exists");
            std::ifstream report(f.output / "tokens/statistics.json");
            auto json = bsoncxx::from_json(std::string(std::istreambuf_iterator<char>(report), {}));
            require(bsoncxx::to_json(json.view()["statistics"].get_document().view()) == bsoncxx::to_json(stats), "JSON statistics match database");
        }
        {
            Fixture f(uri, binary); f.add();
            f.db["corpus_documents"].update_one({}, make_document(kvp("$set", make_document(kvp("text", 123)))));
            f.run(); auto meta = f.metadata(); auto stats = meta.view()["statistics"].get_document().view();
            require(number(stats["error_documents"]) == 1 && number(stats["total"].get_document().view()["tokens"]) == 0, "Bad document statistics");
            auto record = f.db["tokenized_documents"].find_one({});
            require(f.db["tokenized_documents"].count_documents({}) == 1 && string(record->view()["status"]) == "error", "No partial fields");
        }
        {
            Fixture f(uri, binary); f.run(); auto meta = f.metadata();
            auto total = meta.view()["statistics"].get_document().view()["total"].get_document().view();
            require(total["average_token_length"].type() == bsoncxx::type::k_null && total["core_ms_per_kib"].type() == bsoncxx::type::k_null, "Empty corpus ratios");
        }
        {
            Fixture f(uri, binary); f.add();
            f.run("unknown", {"--source", "missing"}, false);
            f.run("bad-limit", {"--limit", "-1"}, false);
            f.run("../unsafe", {}, false);
            require(f.db["tokenization_runs"].count_documents({}) == 0, "Invalid CLI creates no runs");
        }
        {
            Fixture f(uri, binary); f.add();
            auto address = f.uri + (f.uri.find('?') == std::string::npos ? "?" : "&") + "w=0";
            f.run("unacknowledged", {"--uri", address}, false);
            require(f.db["tokenization_runs"].count_documents({}) == 0, "Unacknowledged writes rejected");
        }
        {
            Fixture f(uri, binary); f.add("stopgame", std::string(4 * 1024 * 1024, 'a')); f.run();
            auto record = f.db["tokenized_documents"].find_one({});
            require(f.db["tokenized_documents"].count_documents({}) == 1 && string(record->view()["status"]) == "error", "Oversized token rejects document");
            require(string(record->view()["error"]).find("4 MiB") != std::string::npos, "Storage budget error");
        }
        {
            Fixture f(uri, binary); f.add("vgtimes"); f.add(); f.add();
            f.run("one", {"--source", "stopgame", "--limit", "1"}); auto meta = f.metadata();
            require(number(meta.view()["statistics"].get_document().view()["selected_documents"]) == 1, "Document limit");
            for (auto record : f.db["tokenized_documents"].find({})) require(string(record["source"]) == "stopgame", "Source filter");
            f.run("one", {}, false);
            f.run("one", {"--output", (f.output / "other").string()}, false);
            meta = f.metadata(); require(string(meta.view()["state"]) == "completed", "Duplicate leaves original run intact");
            f.db["corpus_runs"].update_one({}, make_document(kvp("$set", make_document(kvp("state", "running")))));
            f.run("unfinished", {}, false);
            require(f.db["tokenization_runs"].count_documents({}) == 1, "Unfinished corpus creates no run");
        }
        {
            Fixture f(uri, binary); std::string text;
            for (int i = 0; i < 80000; ++i) text += "слово ";
            auto id = f.add("stopgame", text);
            f.db["corpus_documents"].update_one({}, make_document(kvp("$set", make_document(kvp("title", bsoncxx::types::b_null{})))));
            f.run(); mongocxx::options::find sorted; sorted.sort(make_document(kvp("part", 1)));
            auto filter = make_document(kvp("field", "text"));
            require(f.db["tokenized_documents"].count_documents(filter.view()) > 1, "Large field chunked");
            std::int64_t position = 0;
            for (auto part : f.db["tokenized_documents"].find(filter.view(), sorted)) {
                require(part["document_id"].get_oid().value == id, "Chunk document link");
                for (auto token : part["tokens"].get_array().value)
                    require(number(token.get_document().view()["position"]) == position++, "Continuous positions");
            }
            require(position == 80000, "All tokens retained");
        }
        {
            Fixture f(uri, binary); f.add();
            // A regular file cannot be used as the output directory, even as root.
            fs::create_directories(f.output); auto blocked = f.output / "blocked";
            { std::ofstream file(blocked); file << "blocked"; }
            f.run("failure", {"--output", blocked.string()}, false);
            auto meta = f.metadata(); require(string(meta.view()["state"]) == "failed", "Output failure marks run failed");
        }
        {
            Fixture f(uri, binary); f.add();
            f.db["corpus_runs"].insert_one(make_document(kvp("_id", "newer"), kvp("state", "completed"), kvp("finished_at", "2026-10-23")));
            f.db["corpus_runs"].insert_one(make_document(kvp("_id", "incomplete"), kvp("state", "running"), kvp("finished_at", "2026-10-24")));
            auto id = f.add("stopgame", "Новый текст", "ok", "newer"); f.run("auto", {}, true, false);
            auto meta = f.metadata(); require(string(meta.view()["corpus_run_id"]) == "newer", "Latest completed corpus selected");
            for (auto part : f.db["tokenized_documents"].find({})) require(part["document_id"].get_oid().value == id, "Only selected corpus processed");
        }
        std::cout << "All 10 MongoDB tokenizer scenarios passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
