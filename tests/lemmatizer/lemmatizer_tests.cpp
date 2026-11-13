#include "lemmatizer/analyzer.hpp"
#include "tokenizer/tokenizer.hpp"
#include "text/unicode.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace lemmatizer;
void check(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
struct Fake : Backend {
    std::size_t calls = 0;
    std::vector<Analysis> analyze(std::string_view word) override {
        ++calls;
        if (word == "игры") return {{"ИГРА", true, {}}, {"игра", true, {}}, {"ошибка", false, {}}};
        if (word == "стали") return {{"стать", true, {}}, {"сталь", true, {}}};
        return {};
    }
};
int main(int argc, char** argv) {
    try {
        Fake fake;
        Analyzer analyzer(fake, {2, 128});
        auto r = analyzer.analyze("игры");
        check(r.lemma == "игра" && r.candidates.size() == 1 && r.method == "dictionary", "Deduplication/normalization");
        check(analyzer.analyze("стали").candidates == std::vector<std::string>({"сталь", "стать"}), "Ambiguity order");
        analyzer.analyze("игры");
        check(analyzer.cache_hits() == 1, "Cache hit");
        check(analyzer.analyze("абракадабрашка").method == "unknown", "Unknown fallback");
        check(analyzer.cache_entries() == 2, "Bounded cache");
        for (auto word : {"ps5", "rtx4090", "123", "café", "ігри", "а\u0301"})
            check(analyzer.analyze(word).lemma == word, "Unsupported token preserved");
        check(analyzer.analyze("игры", "en-US").method == "language", "Language boundary before cache");
        check(analyzer.analyze("игры", "ru_RU").lemma == "игра", "Russian locale");
        check(analyzer.analyze("").method == "empty", "Empty token");
        Analyzer short_words(fake, {0, 2});
        check(short_words.analyze("игры").method == "too_long", "Length limit");
        bool invalid = false;
        try { analyzer.analyze(std::string("\xff")); } catch (const std::invalid_argument&) { invalid = true; }
        check(invalid, "Reject malformed UTF-8");
        const auto tokens = tokenizer::tokenize("😀 ИГРЫ, PS5");
        check(tokens.size() == 2 && analyzer.analyze(tokens[0].normalized).lemma == "игра"
            && tokens[0].position == 0 && tokens[0].byte_begin == 5 && tokens[0].byte_end == 13, "Token positions preserved");
        check(text::normalize("Е\u0308Ж") == "ёж", "Shared NFC");
        check(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256");
        if (argc == 2) {
            auto engine = make_xmorphy(argv[1]);
            Analyzer real(*engine);
            for (auto word : {"игры", "игрой", "игру"})
                check(real.analyze(word).lemma == "игра", "Real dictionary forms");
            const auto ambiguous = real.analyze("стали");
            check(std::find(ambiguous.candidates.begin(), ambiguous.candidates.end(), "стать") != ambiguous.candidates.end(), "Verb candidate");
            check(std::find(ambiguous.candidates.begin(), ambiguous.candidates.end(), "сталь") != ambiguous.candidates.end(), "Noun candidate");
            check(real.analyze("абракадабрашка").method == "unknown", "No suffix prediction");
            check(real.analyze("ёжами").lemma == "еж" && real.analyze("ежами").lemma == "еж", "Dictionary yo convention");
            for (auto word : {"ёж", "еж", "ёжами", "ежами"}) {
                auto item = real.analyze(word);
                std::cout << word << ": " << item.lemma << " (" << item.method << ")\n";
            }
        }
        std::cout << "All lemmatizer tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
