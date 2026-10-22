#include "tokenizer/tokenizer.hpp"
#include "tokenizer/statistics.hpp"
#include <iostream>
#include <cstdlib>

void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
void expect(const std::string& text, const std::vector<std::string>& values) {
    const auto tokens = tokenizer::tokenize(text);
    check(tokens.size() == values.size(), "Unexpected token count");
    for (std::size_t i = 0; i < values.size(); ++i) {
        check(tokens[i].normalized == values[i], "Unexpected normalization");
        check(tokens[i].position == i, "Unexpected position");
        check(tokens[i].byte_begin < tokens[i].byte_end && tokens[i].byte_end <= text.size(), "Invalid span");
        const auto original = text.substr(tokens[i].byte_begin, tokens[i].byte_end - tokens[i].byte_begin);
        check(tokenizer::tokenize(original).front().normalized == values[i], "Span does not restore source");
    }
}
int main() {
    expect("Привет, МИР! PS5 RTX4090 Ёж ель", {"привет", "мир", "ps5", "rtx4090", "ёж", "ель"});
    expect("S.T.A.L.K.E.R. Counter-Strike C++ 3.14 don't", {"s","t","a","l","k","e","r","counter","strike","c","3","14","don","t"});
    expect("", {});
    expect(" \t\n😀—_+", {});
    expect("И\u0306 Е\u0308 CAFÉ CAFE\u0301 İ", {"й", "ё", "café", "café", "i\u0307"});
    expect("ΟΣ Σ ² Ⅳ", {"ος", "σ"});
    expect("\u0301а\u0301 12\u0301б 中日 ١٢ 𝔄", {"а\u0301", "12", "б", "中日", "١٢", "𝔄"});
    expect(std::string("a\0b", 3), {"a", "b"});
    const auto spans = tokenizer::tokenize("😀ЁЖ, PS5");
    check(spans[0].byte_begin == 4 && spans[0].byte_end == 8 && spans[0].codepoints == 2, "Cyrillic UTF-8 span");
    check(spans[1].byte_begin == 10 && spans[1].byte_end == 13, "ASCII UTF-8 span");
    for (const auto& invalid : {std::string("a\xff"), std::string("\xc0\xaf"), std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"), std::string("\xe2\x82")}) {
        bool threw = false;
        try { tokenizer::tokenize(invalid); } catch (const tokenizer::InvalidUtf8&) { threw = true; }
        check(threw, "Malformed UTF-8 must reject entire field");
    }
    tokenizer::Statistics stats;
    stats.total.add(13, spans, .01);
    check(stats.total.tokens == 2 && stats.total.codepoints == 5 && stats.total.bytes == 13, "Statistics mismatch");
    expect(std::string(100000, 'A'), {std::string(100000, 'a')});
    std::cout << "All tokenizer tests passed (ICU " << tokenizer::icu_version() << ")\n";
}
