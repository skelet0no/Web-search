#include "repository.hpp"
#include "lemmatizer/analysis.hpp"
#include "tokenizer/token.hpp"
#include "text/unicode.hpp"
#include <bsoncxx/array/view.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/uri.hpp>
#include <openssl/evp.h>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <vector>

namespace boolean_search {
namespace {
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using View=bsoncxx::document::view;
using Value=bsoncxx::document::value;
std::string string(View v,const char* key) {
    const auto e=v[key];
    if (!e || e.type()!=bsoncxx::type::k_string) throw std::runtime_error(std::string("Expected string: ")+key);
    return std::string(e.get_string().value);
}
std::int64_t integer(View v,const char* key) {
    const auto e=v[key];
    if (e && e.type()==bsoncxx::type::k_int32) return e.get_int32().value;
    if (e && e.type()==bsoncxx::type::k_int64) return e.get_int64().value;
    throw std::runtime_error(std::string("Expected integer: ")+key);
}
View object(View v,const char* key) {
    const auto e=v[key];
    if (!e || e.type()!=bsoncxx::type::k_document) throw std::runtime_error(std::string("Expected document: ")+key);
    return e.get_document().view();
}
bsoncxx::array::view array(View v,const char* key) {
    const auto e=v[key];
    if (!e || e.type()!=bsoncxx::type::k_array) throw std::runtime_error(std::string("Expected array: ")+key);
    return e.get_array().value;
}
std::string id_key(View v) {
    if (!v["document_id"]) throw std::runtime_error("Missing document_id");
    return bsoncxx::to_json(make_document(kvp("id",v["document_id"].get_value())),bsoncxx::ExtendedJsonMode::k_canonical);
}
std::string fingerprint(View v) {
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned length=0;
    if (EVP_Digest(v.data(),v.length(),digest,&length,EVP_sha256(),nullptr)!=1) throw std::runtime_error("SHA256 failed");
    std::ostringstream out;
    for (unsigned i=0;i<length;++i) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(digest[i]);
    return out.str();
}
Value completed(mongocxx::database db,const char* collection,const std::string& id) {
    auto value=db[collection].find_one(make_document(kvp("_id",id)));
    if (!value || string(value->view(),"state")!="completed")
        throw std::runtime_error(std::string("No completed run in ")+collection+": "+id);
    return Value(value->view());
}
void expect(View v,const char* key,const std::string& expected) {
    if (string(v,key)!=expected) throw std::runtime_error(std::string("Incompatible input: ")+key);
}
}
void build_from_mongo(const BuildOptions& o,const std::function<void()>& check) {
    const auto started=std::chrono::steady_clock::now();
    if (check) check();
    if (o.output.empty() || std::filesystem::exists(o.output)) throw std::invalid_argument("Output must be a new index directory");
    mongocxx::client client{mongocxx::uri(o.uri)};
    auto db=client[o.database];
    mongocxx::options::find latest;
    latest.sort(make_document(kvp("finished_at",-1),kvp("_id",-1)));
    auto selected=db["lemmatization_runs"].find_one(o.lemmatization_run.empty()
        ? make_document(kvp("state","completed")) : make_document(kvp("_id",o.lemmatization_run)),latest);
    if (!selected || string(selected->view(),"state")!="completed")
        throw std::runtime_error("No completed lemmatization run; completed_with_errors is not accepted");
    const auto run=selected->view();
    const auto run_id=string(run,"_id"), token_id=string(run,"tokenization_run_id"), corpus_id=string(run,"corpus_run_id");
    expect(run,"schema_version","1");
    expect(run,"policy_version",lemmatizer::policy_version);
    expect(run,"engine_commit",lemmatizer::engine_commit);
    expect(run,"main_sha256",lemmatizer::main_dictionary_sha256);
    expect(run,"affix_sha256",lemmatizer::affix_dictionary_sha256);
    expect(run,"icu_version",text::icu_version());
    expect(run,"unicode_version",text::unicode_version());
    const auto stats=object(run,"statistics");
    const auto expected_parts=integer(stats,"output_parts"), expected_inputs=integer(stats,"records"), expected_tokens=integer(stats,"tokens");
    const auto maximum=integer(run,"max_token_codepoints");
    if (integer(stats,"errors")!=0 || expected_parts<0 || expected_inputs<0 || expected_tokens<0 || maximum<1 || maximum>4096)
        throw std::runtime_error("Invalid completed run statistics or analyzer settings");
    auto token_run=completed(db,"tokenization_runs",token_id), corpus_run=completed(db,"corpus_runs",corpus_id);
    expect(token_run.view(),"corpus_run_id",corpus_id);
    expect(token_run.view(),"rules_version",tokenizer::rules_version);
    expect(token_run.view(),"icu_version",text::icu_version());
    expect(token_run.view(),"unicode_version",text::unicode_version());
    const auto input_hash=fingerprint(run), token_hash=fingerprint(token_run.view()), corpus_hash=fingerprint(corpus_run.view());
    const auto filter=make_document(kvp("lemmatization_run_id",run_id));
    if (db["lemmatized_documents"].count_documents(filter.view())!=expected_parts) throw std::runtime_error("Incomplete lemmatization output");
    // Build-time supporting index; does not alter the saved corpus or annotations.
    db["lemmatized_documents"].create_index(make_document(kvp("lemmatization_run_id",1),kvp("document_id",1),
        kvp("field",1),kvp("input_part",1),kvp("output_part",1)));
    mongocxx::options::find sorted;
    sorted.sort(make_document(kvp("document_id",1),kvp("field",1),kvp("input_part",1),kvp("output_part",1)));
    sorted.batch_size(1); // BSON records can be up to 4 MiB; keep the cursor buffer bounded.
    Builder builder(o.output,o.memory_bytes,check);
    std::string document_key, source, field;
    DocId document=0;
    std::int64_t part=-1, output_part=-1, next_position=0, records=0, inputs=0, tokens=0;
    std::optional<bsoncxx::oid> input_id;
    std::optional<Value> input;
    std::vector<std::int64_t> input_positions;
    std::size_t input_offset=0;
    const auto finish_input=[&] {
        if (input && input_offset!=input_positions.size()) throw std::runtime_error("Missing annotations for input tokens");
    };
    for (const auto row:db["lemmatized_documents"].find(filter.view(),sorted)) {
        if (check) check();
        ++records;
        expect(row,"status","ok"); expect(row,"tokenization_run_id",token_id); expect(row,"corpus_run_id",corpus_id);
        const auto key=id_key(row), row_field=string(row,"field"), row_source=string(row,"source");
        const auto row_part=integer(row,"input_part"), row_output=integer(row,"output_part");
        if ((row_field!="text" && row_field!="title") || row_part<0 || row_output<0)
            throw std::runtime_error("Invalid field or part number");
        const auto ref=row["input_record_id"];
        if (!ref || ref.type()!=bsoncxx::type::k_oid) throw std::runtime_error("Invalid input_record_id");
        const auto ref_id=ref.get_oid().value;
        const bool new_document=key!=document_key;
        const bool new_field=new_document || row_field!=field;
        const bool new_input=new_field || row_part!=part;
        if (new_input) {
            finish_input();
            if (row_output!=0 || (new_field ? row_part!=0 : row_part!=part+1))
                throw std::runtime_error("Missing or duplicate input/output parts");
            if (!new_document && new_field && row_field<=field) throw std::runtime_error("Invalid field order");
            if (new_document) {
                mongocxx::options::find projection;
                projection.projection(make_document(kvp("document_id",1),kvp("title",1),kvp("url",1),kvp("source",1)));
                auto corpus=db["corpus_documents"].find_one(make_document(kvp("run_id",corpus_id),
                    kvp("document_id",row["document_id"].get_value())),projection);
                if (!corpus) throw std::runtime_error("Missing corpus document");
                const auto cv=corpus->view();
                source=string(cv,"source");
                const auto title=cv["title"];
                if (title && title.type()!=bsoncxx::type::k_null && title.type()!=bsoncxx::type::k_string)
                    throw std::runtime_error("Invalid corpus title");
                const auto metadata=make_document(kvp("document_id",row["document_id"].get_value()),
                    kvp("url",string(cv,"url")),kvp("source",source),
                    kvp("title",title && title.type()==bsoncxx::type::k_string ? std::string(title.get_string().value) : ""));
                document=builder.add_document(bsoncxx::to_json(metadata.view(),bsoncxx::ExtendedJsonMode::k_canonical));
                document_key=key;
            }
            if (new_field) next_position=0;
            field=row_field; part=row_part; output_part=-1; input_id=ref_id;
            auto loaded=db["tokenized_documents"].find_one(make_document(kvp("_id",ref_id)));
            if (!loaded) throw std::runtime_error("Missing input tokenization record");
            input.emplace(loaded->view());
            const auto iv=input->view();
            expect(iv,"status","ok"); expect(iv,"tokenization_run_id",token_id); expect(iv,"corpus_run_id",corpus_id);
            expect(iv,"field",field); expect(iv,"source",source);
            if (id_key(iv)!=key || integer(iv,"part")!=part) throw std::runtime_error("Input tokenization links disagree");
            input_positions.clear(); input_offset=0;
            for (const auto element:array(iv,"tokens")) {
                if (element.type()!=bsoncxx::type::k_document) throw std::runtime_error("Invalid token record");
                const auto tv=element.get_document().view();
                const auto normalized=string(tv,"normalized");
                if (normalized.empty() || text::normalize(normalized)!=normalized)
                    throw std::runtime_error("Invalid normalized input token");
                input_positions.push_back(integer(tv,"position"));
            }
            ++inputs;
        } else if (!input_id || *input_id!=ref_id) throw std::runtime_error("Part refers to a different input record");
        if (row_source!=source || row_output!=output_part+1) throw std::runtime_error("Inconsistent source or output part order");
        output_part=row_output;
        std::size_t annotations=0;
        for (const auto element:array(row,"annotations")) {
            if (check) check();
            if (element.type()!=bsoncxx::type::k_document) throw std::runtime_error("Invalid annotation record");
            const auto annotation=element.get_document().view();
            const auto position=integer(annotation,"position");
            if (position!=next_position || input_offset>=input_positions.size() || input_positions[input_offset]!=position)
                throw std::runtime_error("Missing, duplicate or out-of-order annotation position");
            const auto lemma=string(annotation,"lemma");
            std::string previous;
            bool contains=false;
            std::size_t candidates=0;
            for (const auto candidate:array(annotation,"lemma_candidates")) {
                if (candidate.type()!=bsoncxx::type::k_string) throw std::runtime_error("Invalid lemma candidate");
                const std::string term(candidate.get_string().value);
                if (term.empty() || (candidates && term<=previous) || text::normalize(term)!=term)
                    throw std::runtime_error("Empty, duplicate, unsorted or unnormalized lemma candidate");
                contains=contains || term==lemma; previous=term; ++candidates;
                builder.add(term,document);
            }
            if (!candidates || !contains) throw std::runtime_error("Missing primary lemma in candidates");
            ++input_offset; ++next_position; ++annotations; ++tokens;
        }
        if (!annotations && (!input_positions.empty() || row_output!=0)) throw std::runtime_error("Unexpected empty output part");
        if (records%1000==0) std::cerr << "Indexed " << records << " annotation parts\n";
    }
    finish_input();
    if (records!=expected_parts || inputs!=expected_inputs || tokens!=expected_tokens ||
        db["lemmatized_documents"].count_documents(filter.view())!=expected_parts)
        throw std::runtime_error("Input counts changed or annotations are incomplete");
    if (fingerprint(completed(db,"lemmatization_runs",run_id).view())!=input_hash ||
        fingerprint(completed(db,"tokenization_runs",token_id).view())!=token_hash ||
        fingerprint(completed(db,"corpus_runs",corpus_id).view())!=corpus_hash)
        throw std::runtime_error("Input metadata changed during index build");
    Metadata metadata{
        {"lemmatization_run_id",run_id},{"tokenization_run_id",token_id},{"corpus_run_id",corpus_id},
        {"input_fingerprint",input_hash},{"tokenization_fingerprint",token_hash},{"corpus_fingerprint",corpus_hash},
        {"input_metadata",bsoncxx::to_json(run,bsoncxx::ExtendedJsonMode::k_canonical)},
        {"policy_version",lemmatizer::policy_version},{"engine_commit",lemmatizer::engine_commit},
        {"main_sha256",lemmatizer::main_dictionary_sha256},{"affix_sha256",lemmatizer::affix_dictionary_sha256},
        {"icu_version",text::icu_version()},{"unicode_version",text::unicode_version()},
        {"rules_version",tokenizer::rules_version},{"max_token_codepoints",std::to_string(maximum)},
        {"input_records",std::to_string(inputs)},{"output_parts",std::to_string(records)},{"tokens",std::to_string(tokens)},
        {"read_seconds",std::to_string(std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count())}
    };
    builder.finish(std::move(metadata));
}
}
