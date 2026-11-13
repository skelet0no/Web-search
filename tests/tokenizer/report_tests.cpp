#include "repository.hpp"
#include "tokenizer/tokenizer.hpp"
#include "../support.hpp"
#include <bsoncxx/json.hpp>
#include <iostream>

int main() {
    using namespace test_support;
    try {
        tokenizer::RunResult empty;
        const auto zero=tokenizer::statistics_document(empty);
        const auto z=zero.view();
        require(z["wall_ms_per_kib"].type()==bsoncxx::type::k_null,"Empty wall ratio");
        const auto zm=z["total"].get_document().view();
        for (const auto* field:{"average_token_length","core_ms_per_kib","core_kib_per_second"})
            require(zm[field].type()==bsoncxx::type::k_null,"Empty metric ratio");
        tokenizer::RunResult result;
        result.statistics.selected=2; result.statistics.successful=1; result.statistics.errors=1;
        result.statistics.total.add(1024,tokenizer::tokenize("Игра PS5"),.5);
        result.statistics.total.add(0,{},0);
        result.wall_seconds=2;
        require(result.statistics.total.fields==2 && result.statistics.total.codepoints==7,"Metrics accumulation");
        const auto saved=tokenizer::statistics_document(result);
        const auto total=saved.view()["total"].get_document().view();
        require(total["average_token_length"].get_double().value==3.5,"Average token length");
        require(total["core_ms_per_kib"].get_double().value==500,"Milliseconds per KiB");
        require(saved.view()["wall_kib_per_second"].get_double().value==.5,"Wall throughput");
        Directory dir;
        tokenizer::Options options; options.output=dir.path; options.run_id="unit"; options.corpus_run="corpus";
        std::filesystem::create_directory(dir.path/"unit");
        tokenizer::write_report(options,result);
        const auto metadata=bsoncxx::from_json(read(dir.path/"unit/statistics.json"));
        require(metadata.view()["tokenization_run_id"].get_string().value=="unit","Report run ID");
        require(read(dir.path/"unit/report.md").find("unit")!=std::string::npos,"Markdown report");
        options.run_id="missing-directory";
        rejects<std::ios_base::failure>([&] { tokenizer::write_report(options,result); },"Report output failure hidden");
        std::cout << "Tokenizer report unit tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
