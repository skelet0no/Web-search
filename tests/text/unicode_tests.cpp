#include "text/unicode.hpp"
#include "../support.hpp"
#include <iostream>

int main() {
    using namespace test_support;
    try {
        std::size_t count=999;
        require(text::normalize("",count).empty() && count==0,"Empty normalization");
        require(text::normalize("ABC_123",count)=="abc_123" && count==7,"ASCII fast path");
        require(text::normalize("Е\u0308Ж",count)=="ёж" && count==2,"NFC and codepoint count");
        require(text::normalize("CAFE\u0301",count)=="café" && count==4,"NFC Latin");
        require(text::normalize("ЁЖ")!="еж","Unicode normalization must not conflate е/ё");
        require(text::normalize("İ",count)=="i\u0307" && count==2,"Lowercase expansion");
        const std::string nul("A\0B",3);
        require(text::normalize(nul,count)==std::string("a\0b",3) && count==3,"Embedded NUL");
        for (const auto& valid:{"ASCII","😀","русский","中文"}) text::validate_utf8(valid);
        for (const auto& invalid:{std::string("\x80"),std::string("\xc0\xaf"),std::string("\xed\xa0\x80"),
                                 std::string("\xf4\x90\x80\x80"),std::string("\xe2\x82")}) {
            rejects<std::invalid_argument>([&] { text::validate_utf8(invalid); },"Invalid UTF8 accepted");
            rejects<std::invalid_argument>([&] { text::normalize(invalid); },"Invalid UTF8 normalized");
        }
        const auto normalized=text::normalize("ПРИВЕТ CAFE\u0301");
        require(text::normalize(normalized)==normalized,"Normalization idempotence");
        require(!text::icu_version().empty() && !text::unicode_version().empty(),"Version metadata");
        std::cout << "Unicode unit tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
