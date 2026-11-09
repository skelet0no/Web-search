#include "boolean/query.hpp"
#include "boolean/normalization.hpp"
#include "app.hpp"
#include "lemmatizer/analyzer.hpp"
#include "tokenizer/tokenizer.hpp"
#include "text/unicode.hpp"
#include <iostream>
#include <optional>

int main(int argc,char** argv) {
    using namespace boolean_search;
    app::signals();
    try {
        std::filesystem::path path,dictionary=BOOLEAN_DEFAULT_DICTIONARY;
        std::optional<std::string> query;
        bool interactive=false, verify=false, json=false;
        std::uint64_t limit=20, offset=0;
        for (int i=1;i<argc;++i) {
            const std::string key=argv[i];
            if (key=="--help") {
                std::cout << "Usage: boolean-search --index DIR --query 'QUERY' [--limit 20] [--offset 0] [--json]\n"
                    "       boolean-search --index DIR --interactive [--json]\n"
                    "       boolean-search --index DIR --verify\n"
                    "  [--dictionary DIR]\n"
                    "Operators: uppercase AND OR NOT; priority NOT > AND > OR. Parentheses supported.\n"
                    "Spaces imply AND. All dictionary lemma candidates are matched.\n"
                    "title and text form one field. No ranking, phrases, wildcards or fuzzy search.\n"
                    "Counter-Strike -> counter AND strike; C++ -> c; S.T.A.L.K.E.R. -> s AND t AND a AND l AND k AND e AND r.\n"
                    "Interactive: one query per line, empty lines ignored, EOF exits. MongoDB is not needed.\n";
                return 0;
            }
            if (key=="--interactive") { interactive=true; continue; }
            if (key=="--verify") { verify=true; continue; }
            if (key=="--json") { json=true; continue; }
            if (i+1==argc) throw std::invalid_argument("Missing value for "+key);
            const std::string value=argv[++i];
            if (key=="--index") path=value;
            else if (key=="--dictionary") dictionary=value;
            else if (key=="--query") {
                if (query) throw std::invalid_argument("Repeated --query");
                query=value;
            } else if (key=="--limit") limit=app::number(value,true);
            else if (key=="--offset") offset=app::number(value,true);
            else throw std::invalid_argument("Unknown option: "+key);
        }
        if (path.empty() || unsigned(query.has_value())+unsigned(interactive)+unsigned(verify)!=1)
            throw std::invalid_argument("Specify --index and exactly one of --query, --interactive, --verify");
        if (limit>100000) throw std::invalid_argument("--limit must be at most 100000");
        Index index(path);
        if (verify) { index.verify(app::check); std::cout << "Index verified\n"; return 0; }
        require_compatible(index.metadata());
        auto backend=lemmatizer::make_xmorphy(dictionary);
        const auto maximum=app::number(app::required(index.metadata(),"max_token_codepoints"));
        if (maximum>4096) throw std::runtime_error("Invalid max_token_codepoints in index");
        lemmatizer::Analyzer analyzer(*backend,{20000,static_cast<std::size_t>(maximum)});
        const Normalize normalize=[&](const std::string& raw) {
            return normalize_query(raw,analyzer);
        };
        const auto execute=[&](const std::string& text) {
            app::check();
            const auto result=search(index,text,normalize,static_cast<std::size_t>(limit),offset,app::check);
            if (json) {
                std::cout << "{\"query\":" << json_quote(text) << ",\"total\":" << result.total << ",\"offset\":" << offset << ",\"documents\":[";
                bool first=true;
                for (auto id:result.documents) {
                    if (!first) std::cout << ',';
                    first=false;
                    std::cout << "{\"id\":" << id << ",\"document\":" << index.document(id) << '}';
                }
                std::cout << "]}\n";
            } else {
                std::cout << "Matches: " << result.total << "; offset: " << offset << '\n';
                for (auto id:result.documents) std::cout << id << '\t' << index.document(id) << '\n';
            }
        };
        if (query) execute(*query);
        else {
            std::string line;
            while (!app::stopped && std::getline(std::cin,line)) {
                if (line.find_first_not_of(" \t\r\n")==std::string::npos) continue;
                try { execute(line); }
                catch (const std::invalid_argument& e) {
                    if (json) std::cout << "{\"error\":" << json_quote(e.what()) << "}\n";
                    else std::cerr << "Invalid query: " << e.what() << '\n';
                }
            }
            app::check();
            if (std::cin.bad()) throw std::runtime_error("Cannot read queries");
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Search failed: " << e.what() << '\n';
        return app::stopped ? 130 : 2;
    }
}
