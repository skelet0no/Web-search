#include "input.hpp"
#include <bsoncxx/json.hpp>
#include <iostream>
int main() {
    try {
        std::unordered_map<std::string, std::uint64_t> counts;
        auto first = bsoncxx::from_json(R"({"status":"ok","output_part":0,"annotations":[{"lemma":"игра","lemma_candidates":["игра","играть"]},{"lemma":"игра"},{"lemma":"2026"}]})");
        auto second = bsoncxx::from_json(R"({"status":"ok","output_part":1,"annotations":[{"lemma":"игра"},{"lemma":"game"}]})");
        zipf::count_record(first.view(), counts);
        zipf::count_record(second.view(), counts);
        if (counts.size() != 3 || counts.at("игра") != 3 || counts.at("2026") != 1 || counts.at("game") != 1)
            throw std::runtime_error("Parts, repeated lemmas or candidate policy");
        for (const auto* json : {R"({"status":"error"})", R"({"status":"ok"})",
             R"({"annotations":[]})", R"({"status":42,"annotations":[]})",
             R"({"status":"ok","annotations":42})", R"({"status":"ok","annotations":[{"lemma":7}]})",
             R"({"status":"ok","annotations":[{"lemma":""}]})",
             R"({"status":"ok","annotations":[42]})"}) {
            bool rejected = false;
            auto row = bsoncxx::from_json(json);
            try { zipf::count_record(row.view(), counts); } catch (const std::runtime_error&) { rejected = true; }
            if (!rejected) throw std::runtime_error("Malformed input accepted");
        }
        counts["игра"]=UINT64_MAX;
        bool overflow=false;
        try { zipf::count_record(first.view(),counts); } catch (const std::overflow_error&) { overflow=true; }
        if (!overflow) throw std::runtime_error("Term frequency overflow accepted");
        std::cout << "Zipf input tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
