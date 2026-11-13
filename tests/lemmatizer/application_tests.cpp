#include "options.hpp"
#include "report.hpp"
#include "statistics.hpp"
#include "../support.hpp"
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>
#include <iostream>

namespace {
lemmatizer::Options parse(std::vector<std::string> args) {
    args.insert(args.begin(),"lemmatizer");
    std::vector<char*> pointers;
    for (auto& arg:args) pointers.push_back(arg.data());
    return lemmatizer::parse_options(static_cast<int>(pointers.size()),pointers.data());
}
}
int main() {
    using namespace test_support;
    using namespace lemmatizer;
    try {
        const auto options=parse({"--run-id","valid-1","--dictionary","/unit/dict","--uri","mongodb://unit",
                                  "--source","stopgame","--limit","3","--cache-size","0","--batch-size","2"});
        require(options.run_id=="valid-1" && options.limit==3 && options.analyzer.cache_size==0 && options.batch_size==2,"Option parsing");
        require(parse({"--help"}).help,"Help does not validate dictionary");
        const auto resume=parse({"--resume","saved","--uri","mongodb://unit","--dictionary","/unit/dict"});
        require(resume.resume && resume.run_id=="saved","Resume permitted overrides");
        for (const auto& args:std::vector<std::vector<std::string>>{
                 {"--limit","0"},{"--cache-size","-1"},{"--batch-size","1x"},{"--unknown","x"},
                 {"--limit"},{"--run-id","../escape"},{"--resume","saved","--limit","1"},
                 {"--resume","saved","--run-id","new"}})
            rejects<std::invalid_argument>([&] { parse(args); },"Invalid options accepted");
        for (unsigned boundary=0;boundary<8;++boundary) {
            auto invalid=options;
            switch (boundary) {
                case 0: invalid.batch_size=1001; break;
                case 1: invalid.lease_seconds=2; break;
                case 2: invalid.lease_seconds=3601; break;
                case 3: invalid.chunk_bytes=1023; break;
                case 4: invalid.chunk_bytes=4*1024*1024+1; break;
                case 5: invalid.analyzer.cache_size=1000001; break;
                case 6: invalid.database.clear(); break;
                case 7: invalid.dictionary.clear(); break;
            }
            rejects<std::invalid_argument>([&] { validate_options(invalid); },"Option boundary accepted");
        }
        Statistics stats;
        stats.records=2; stats.tokens=7; stats.changed=3; stats.cache_hits=4;
        stats.input_bytes=32; stats.output_parts=5; stats.core_seconds=.25; stats.wall_seconds=.5;
        const auto saved=stats.bson();
        const auto restored=Statistics::load(saved.view());
        require(bsoncxx::to_json(restored.bson().view())==bsoncxx::to_json(saved.view()),"Statistics round trip");
        auto bad=stats; bad.tokens=-1;
        const auto negative=bad.bson();
        rejects<std::runtime_error>([&] { Statistics::load(negative.view()); },"Negative statistics accepted");
        Directory dir;
        auto report_options=options; report_options.output=dir.path;
        auto metadata=bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("state","running"));
        write_report(report_options,stats,metadata.view());
        const auto path=dir.path/options.run_id;
        require(bsoncxx::from_json(read(path/"statistics.json")).view()["state"].get_string().value=="running","Report metadata");
        require(read(path/"report.md").find("valid-1")!=std::string::npos,"Report run ID");
        require(!std::filesystem::exists(path/"report.md.tmp") && !std::filesystem::exists(path/"statistics.json.tmp"),"Temporary report files left behind");
        stats.wall_seconds=0;
        write_report(report_options,stats,metadata.view());
        require(read(path/"report.md").find("inf")==std::string::npos,"Zero-time report division");
        std::cout << "Lemmatizer application unit tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
