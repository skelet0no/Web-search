#include "repository.hpp"
#include "boolean/query.hpp"
#include "boolean/ranking.hpp"
#include "lemmatizer/analysis.hpp"
#include "tokenizer/tokenizer.hpp"
#include "text/unicode.hpp"
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>
#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace boolean_search;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::array;
void require(bool ok,const char* reason) { if (!ok) throw std::runtime_error(reason); }
template<class F> void rejects(F action,const char* reason) {
    bool failed=false;
    try { action(); } catch (const std::exception&) { failed=true; }
    require(failed,reason);
}
struct Fixture {
    mongocxx::client client;
    mongocxx::database db;
    BuildOptions options;
    std::int64_t parts=0, inputs=0, tokens=0;
    explicit Fixture(const std::string& uri)
        : client(mongocxx::uri(uri)),db(client["boolean_test_"+bsoncxx::oid{}.to_string()]) {
        options.uri=uri; options.database=std::string(db.name()); options.lemmatization_run="lemmas";
        options.output=std::filesystem::temp_directory_path()/options.database;
        options.memory_bytes=4096;
        db["corpus_runs"].insert_one(make_document(kvp("_id","corpus"),kvp("state","completed")));
        db["tokenization_runs"].insert_one(make_document(kvp("_id","tokens"),kvp("state","completed"),
            kvp("corpus_run_id","corpus"),kvp("rules_version",tokenizer::rules_version),
            kvp("icu_version",text::icu_version()),kvp("unicode_version",text::unicode_version())));
        db["lemmatization_runs"].insert_one(make_document(kvp("_id","lemmas"),kvp("state","completed"),
            kvp("schema_version","1"),kvp("tokenization_run_id","tokens"),kvp("corpus_run_id","corpus"),
            kvp("policy_version",lemmatizer::policy_version),kvp("engine_commit",lemmatizer::engine_commit),
            kvp("main_sha256",lemmatizer::main_dictionary_sha256),kvp("affix_sha256",lemmatizer::affix_dictionary_sha256),
            kvp("icu_version",text::icu_version()),kvp("unicode_version",text::unicode_version()),
            kvp("max_token_codepoints",128)));
        update();
    }
    ~Fixture() {
        try { db.drop(); } catch (...) {}
        std::error_code ec; std::filesystem::remove_all(options.output,ec);
    }
    void update() {
        db["lemmatization_runs"].update_one({},make_document(kvp("$set",make_document(kvp("statistics",
            make_document(kvp("errors",std::int64_t(0)),kvp("records",inputs),kvp("tokens",tokens),kvp("output_parts",parts)))))));
    }
    void field(bsoncxx::oid id,const std::string& name,const std::string& raw,std::int64_t part=0,std::int64_t position=0) {
        const bsoncxx::oid ref;
        array values;
        const auto ts=tokenizer::tokenize(raw);
        for (const auto& t:ts) values.append(make_document(kvp("position",position+static_cast<std::int64_t>(t.position)),kvp("normalized",t.normalized)));
        db["tokenized_documents"].insert_one(make_document(kvp("_id",ref),kvp("tokenization_run_id","tokens"),kvp("corpus_run_id","corpus"),
            kvp("document_id",id),kvp("source","stopgame"),kvp("field",name),kvp("part",part),kvp("status","ok"),kvp("tokens",values.view())));

        for (std::size_t i=0;i<std::max<std::size_t>(1,ts.size());++i) {
            array annotations;
            if (i<ts.size()) {
                const auto& normalized=ts[i].normalized;
                const std::string lemma=normalized=="игры" ? "игра" : normalized=="стали" ? "сталь" : normalized;
                array candidates; candidates.append(lemma);
                if (normalized=="стали") candidates.append("стать");
                annotations.append(make_document(kvp("position",position+static_cast<std::int64_t>(i)),
                    kvp("lemma",lemma),kvp("lemma_candidates",candidates.view())));
            }
            db["lemmatized_documents"].insert_one(make_document(kvp("lemmatization_run_id","lemmas"),kvp("tokenization_run_id","tokens"),
                kvp("corpus_run_id","corpus"),kvp("input_record_id",ref),kvp("document_id",id),kvp("source","stopgame"),
                kvp("field",name),kvp("input_part",part),kvp("output_part",static_cast<std::int64_t>(i)),
                kvp("status","ok"),kvp("annotations",annotations.view())));
            ++parts;
        }
        ++inputs; tokens+=static_cast<std::int64_t>(ts.size()); update();
    }
    bsoncxx::oid add(const std::string& raw="игры стали ps5") {
        bsoncxx::oid id;
        db["corpus_documents"].insert_one(make_document(kvp("run_id","corpus"),kvp("document_id",id),
            kvp("source","stopgame"),kvp("url","https://stopgame.ru/"+id.to_string()),kvp("title","Игры")));
        field(id,"text",raw);
        field(id,"title","игры");
        return id;
    }
    void build() { build_from_mongo(options); }
};
}
int main() {
    const auto* uri=std::getenv("BOOLEAN_TEST_URI");
    if (!uri) { std::cout << "Set BOOLEAN_TEST_URI to run isolated MongoDB integration tests\n"; return 77; }
    try {
        mongocxx::instance instance;
        {
            Fixture f(uri); const auto id=f.add(); f.field(id,"text","игры",1,3); f.add("xbox");
            const auto before=f.db["lemmatized_documents"].find_one({});
            f.build(); Index index(f.options.output); index.verify();
            require(index.document_count()==2 && index.frequency("игра")==2,"Fields and documents deduplicated");
            require(index.frequency("сталь")==1 && index.frequency("стать")==1,"All lemma candidates indexed");
            auto games=index.lookup_postings("игра");
            require(games && games->title_frequency()==1 && games->text_frequency()==2,
                    "TF must sum across input/output parts and preserve fields");
            games->next();
            require(games->title_frequency()==1 && games->text_frequency()==0,"Title-only TF");
            for (const auto* candidate:{"сталь","стать"}) {
                auto stream=index.lookup_postings(candidate);
                require(stream && stream->title_frequency()==0 && stream->text_frequency()==1,
                        "Each lemma candidate must get one occurrence per token");
            }
            require(index.metadata().at("lemmatization_run_id")=="lemmas","Input run metadata saved");
            const auto after=f.db["lemmatized_documents"].find_one(make_document(kvp("_id",before->view()["_id"].get_value())));
            require(bsoncxx::to_json(before->view())==bsoncxx::to_json(after->view()),"Source annotations changed");
            rejects([&] { f.build(); },"Existing output overwritten");
            f.db["lemmatized_documents"].drop();
            require(index.frequency("игра")==2,"Search still depends on MongoDB data");
            const Normalize normalized=[](const std::string&) {
                return std::vector<std::vector<std::string>>{{"игра"},{"ps5"}};
            };
            require(Ranker(index).search("игра ps5",normalized).total==2,"Ranking still depends on MongoDB");
        }
        { Fixture f(uri); f.build(); Index index(f.options.output); index.verify(); require(index.document_count()==0,"Empty input"); }
        {
            Fixture f(uri); f.add("");
            f.build(); Index index(f.options.output); require(index.document_count()==1,"Empty text lost");
        }
        for (const auto* state:{"running","failed","completed_with_errors"}) {
            Fixture f(uri); f.add();
            f.db["lemmatization_runs"].update_one({},make_document(kvp("$set",make_document(kvp("state",state)))));
            rejects([&] { f.build(); },"Invalid run state accepted"); require(!std::filesystem::exists(f.options.output),"Invalid input published");
        }
        {
            Fixture f(uri); f.add();
            f.db["lemmatized_documents"].delete_one(make_document(kvp("field","text"),kvp("output_part",1)));
            rejects([&] { f.build(); },"Missing output part accepted");
        }
        {
            Fixture f(uri); f.add();
            f.db["tokenized_documents"].delete_one(make_document(kvp("field","text")));
            rejects([&] { f.build(); },"Broken input link accepted");
        }
        {
            Fixture f(uri); f.add();
            f.db["lemmatized_documents"].update_one(make_document(kvp("field","text"),kvp("output_part",0)),
                make_document(kvp("$set",make_document(kvp("annotations",array{}.extract())))));
            rejects([&] { f.build(); },"Missing token annotations accepted");
        }
        {
            Fixture f(uri); f.add();
            f.db["lemmatization_runs"].update_one({},make_document(kvp("$set",make_document(kvp("main_sha256","wrong")))));
            rejects([&] { f.build(); },"Incompatible dictionary accepted");
        }
        {
            Fixture f(uri); f.add();
            rejects([&] { build_from_mongo(f.options,[] { throw std::runtime_error("Interrupted"); }); },"Interruption ignored");
            require(!std::filesystem::exists(f.options.output),"Interrupted build published");
        }
        std::cout << "Boolean MongoDB integration tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
