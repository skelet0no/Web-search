#include "boolean/normalization.hpp"
#include "lemmatizer/analyzer.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

int main(int argc,char** argv) {
    using namespace boolean_search;
    std::filesystem::path directory;
    try {
        if (argc!=2) throw std::runtime_error("Expected dictionary path");
        std::string pattern=(std::filesystem::temp_directory_path()/"boolean-morphology-XXXXXX").string();
        std::vector<char> temporary(pattern.begin(),pattern.end()); temporary.push_back('\0');
        if (!mkdtemp(temporary.data())) throw std::runtime_error("Cannot create fixture");
        directory=temporary.data();
        {
            Builder builder(directory/"index",4096);
            auto a=builder.add_document("{}"); builder.add("игра",a); builder.add("сталь",a); builder.add("еж",a);
            auto b=builder.add_document("{}"); builder.add("стать",b); builder.add("ps5",b);
            auto c=builder.add_document("{}"); builder.add("игры",c); // Russian morphology disabled by document language.
            builder.add("абракадабрашка",c); builder.add("2026",c);
            builder.finish({});
        }
        Index index(directory/"index");
        auto backend=lemmatizer::make_xmorphy(argv[1]);
        lemmatizer::Analyzer analyzer(*backend);
        const Normalize normalize=[&](const std::string& raw) { return normalize_query(raw,analyzer); };
        const auto expect=[&](const std::string& q,const std::vector<DocId>& expected) {
            if (search(index,q,normalize).documents!=expected) throw std::runtime_error("Morphology mismatch: "+q);
        };
        expect("игры",{0,2}); expect("игрой",{0});
        expect("стали",{0,1}); expect("ёжами",{0}); expect("ежами",{0});
        expect("PS5",{1}); expect("абракадабрашка 2026",{2});
        expect("игрой OR PS5",{0,1});
        bool rejected=false;
        try { require_compatible({{"policy_version","incompatible"}}); } catch (const std::exception&) { rejected=true; }
        if (!rejected) throw std::runtime_error("Incompatible manifest accepted");
        std::filesystem::remove_all(directory);
        std::cout << "Boolean dictionary tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        if (!directory.empty()) { std::error_code ec; std::filesystem::remove_all(directory,ec); }
        std::cerr << e.what() << '\n'; return 1;
    }
}
