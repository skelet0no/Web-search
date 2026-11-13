#include "lemmatizer/analyzer.hpp"
#include "../support.hpp"
#include <iostream>
#include <map>

namespace {
struct Backend final : lemmatizer::Backend {
    std::map<std::string,unsigned> calls;
    std::vector<lemmatizer::Analysis> analyze(std::string_view word) override {
        ++calls[std::string(word)];
        if (word=="пустой") return {{"",true,{}}};
        if (word=="прогноз") return {{"гипотеза",false,{}}};
        return {{std::string(word),true,{}}};
    }
};
}
int main() {
    using namespace test_support;
    try {
        Backend backend;
        rejects<std::invalid_argument>([&] { lemmatizer::Analyzer a(backend,{0,0}); },"Zero max length");
        rejects<std::invalid_argument>([&] { lemmatizer::Analyzer a(backend,{0,4097}); },"Oversize max length");
        lemmatizer::Analyzer analyzer(backend,{2,128});
        analyzer.analyze("один"); analyzer.analyze("два"); analyzer.analyze("один");
        analyzer.analyze("три"); analyzer.analyze("два");
        require(backend.calls["один"]==1 && backend.calls["два"]==2,"LRU must evict least recently used");
        require(analyzer.cache_hits()==1 && analyzer.cache_misses()==4 && analyzer.cache_entries()==2,"LRU statistics");
        lemmatizer::Analyzer uncached(backend,{0,128});
        uncached.analyze("четыре"); uncached.analyze("четыре");
        require(backend.calls["четыре"]==2 && uncached.cache_entries()==0,"Disabled cache");
        require(uncached.analyze("прогноз").method=="unknown","Predicted analyses must not enter dictionary candidates");
        rejects<std::runtime_error>([&] { uncached.analyze("пустой"); },"Empty dictionary lemma");
        for (const auto* language:{"en","en-US","uk","de_DE"})
            require(uncached.analyze("игры",language).method=="language","Foreign language policy");
        for (const auto* language:{"ru","RU-ru","ru_RU","ru:RU","und","unknown",""})
            require(uncached.analyze("игры",language).method=="dictionary","Russian/unknown language policy");
        require(!backend.calls.count("123"),"Unexpected backend call before unsupported token");
        require(uncached.analyze("123").method=="unsupported_token" && !backend.calls.count("123"),"Unsupported token reaches backend");
        Directory directory;
        const auto path=directory.path/"bytes.bin";
        const std::string bytes("a\0b",3);
        { std::ofstream out(path,std::ios::binary); out.write(bytes.data(),bytes.size()); }
        require(lemmatizer::sha256_file(path)==lemmatizer::sha256(bytes),"File hashing includes NUL bytes");
        require(lemmatizer::sha256("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","Empty SHA256");
        rejects<std::runtime_error>([&] { lemmatizer::sha256_file(directory.path/"missing"); },"Missing hash input");
        rejects<std::runtime_error>([&] { lemmatizer::make_xmorphy(directory.path/"missing"); },"Missing dictionary accepted");
        { std::ofstream out(directory.path/"udmaindict.bin"); out << "incorrect dictionary"; }
        rejects<std::invalid_argument>([&] { lemmatizer::make_xmorphy(directory.path); },"Unpinned dictionary accepted");
        std::cout << "Lemmatizer policy unit tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
