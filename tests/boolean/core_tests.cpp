#include "boolean/query.hpp"
#include "tokenizer/tokenizer.hpp"
#include <algorithm>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <random>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace boolean_search;
namespace fs=std::filesystem;
void require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F action,const char* reason) {
    bool rejected=false;
    try { action(); } catch (const std::exception&) { rejected=true; }
    require(rejected,reason);
}
const Normalize normalize=[](const std::string& text) {
    std::vector<std::vector<std::string>> result;
    for (const auto& t:tokenizer::tokenize(text)) {
        if (t.normalized=="игры" || t.normalized=="игрой") result.push_back({"игра"});
        else if (t.normalized=="стали") result.push_back({"сталь","стать"});
        else result.push_back({t.normalized});
    }
    return result;
};
void expect(const Index& index,const std::string& q,const std::vector<DocId>& expected) {
    const auto r=search(index,q,normalize,1000);
    require(r.total==expected.size() && r.documents==expected,"Unexpected query result");
}
struct Fixture {
    fs::path root;
    Fixture() {
        std::string name=(fs::temp_directory_path()/"boolean-tests-XXXXXX").string();
        std::vector<char> path(name.begin(),name.end()); path.push_back('\0');
        if (!mkdtemp(path.data())) throw std::runtime_error("Cannot create test directory");
        root=path.data();
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root,ec); }
};
using Set=std::set<DocId>;
struct Expression { std::string text; Set expected; };
Expression expression(std::mt19937& rng,unsigned depth,const std::vector<Set>& terms,const Set& all) {
    if (!depth || rng()%4==0) {
        auto t=rng()%terms.size(); return {"t"+std::to_string(t),terms[t]};
    }
    auto a=expression(rng,depth-1,terms,all);
    const auto operation=rng()%3;
    Set result;
    if (operation==0) {
        std::set_difference(all.begin(),all.end(),a.expected.begin(),a.expected.end(),std::inserter(result,result.end()));
        return {"NOT ("+a.text+")",result};
    }
    auto b=expression(rng,depth-1,terms,all);
    if (operation==1) std::set_intersection(a.expected.begin(),a.expected.end(),b.expected.begin(),b.expected.end(),std::inserter(result,result.end()));
    else std::set_union(a.expected.begin(),a.expected.end(),b.expected.begin(),b.expected.end(),std::inserter(result,result.end()));
    return {"("+a.text+(operation==1 ? " AND " : " OR ")+b.text+")",result};
}
}
int main() {
    try {
        Fixture f;
        {
            Builder b(f.root/"simple",4096);
            const auto a=b.add_document("{\"title\":\"Игра\"}"), c=b.add_document("{}"), d=b.add_document("{}");
            b.add_document("{}");
            for (auto term:{"игра","ps5","сталь"}) b.add(term,a);
            b.add("игра",a); b.add("игра",c); b.add("xbox",c); b.add("стать",d);
            b.add("café",d); b.add("and",d);
            b.finish({});
        }
        Index simple(f.root/"simple"); simple.verify();
        require(simple.document_count()==4 && simple.frequency("игра")==2,"Deduplication and document count");
        expect(simple,"игры",{0,1}); expect(simple,"игрой PS5",{0});
        expect(simple,"стали",{0,2}); expect(simple,"игра OR xbox AND NOT ps5",{0,1});
        expect(simple,"(игра OR xbox) NOT ps5",{1});
        expect(simple,"NOT unknown",{0,1,2,3}); expect(simple,"NOT NOT игра",{0,1});
        expect(simple,"игра AND NOT игра",{}); expect(simple,"unknown",{});
        expect(simple,"PS5(игры OR xbox)",{0}); expect(simple,"CAFE\u0301",{2}); expect(simple,"and",{2});
        require(simple.document(0).find("Игра")!=std::string::npos,"UTF8 document metadata");
        auto page=search(simple,"NOT unknown",normalize,1,2);
        require(page.total==4 && page.documents==std::vector<DocId>{2},"Pagination must not truncate total");
        require(search(simple,"игра",normalize,0).total==2,"Count-only query");
        for (const auto* q:{"","AND","OR x","NOT","()","игра AND","игра OR )","(игра","игра)","игра OR OR ps5","\"игра\"","иг*","иг?","игра & ps5","игра | ps5","!игра"})
            rejects([&] { search(simple,q,normalize); },"Invalid query accepted");
        rejects([&] { search(simple,std::string(17000,'a'),normalize); },"Long query accepted");
        rejects([&] { search(simple,std::string("\xff"),normalize); },"Invalid UTF8 query accepted");
        rejects([&] { search(simple,std::string(65,'(')+"игра"+std::string(65,')'),normalize); },"Deep query accepted");
        rejects([&] { Builder duplicate(f.root/"simple",4096); },"Existing index replaced");
        {
            Builder empty(f.root/"empty",4096); empty.finish({});
            Index index(f.root/"empty"); index.verify(); expect(index,"NOT x",{});
        }
        {
            Builder interrupted(f.root/"interrupted",4096,[] { throw std::runtime_error("Interrupted"); });
            rejects([&] { interrupted.add_document("{}"); },"Interruption ignored");
        }
        require(!fs::exists(f.root/"interrupted"),"Interrupted index published");
        {
            Builder racing(f.root/"racing",4096); racing.add_document("{}");
            fs::create_directory(f.root/"racing");
            rejects([&] { racing.finish({}); },"Concurrent destination overwritten");
        }
        std::vector<Set> term_sets(40); Set all;
        std::mt19937 rng(73019);
        std::vector<std::vector<std::string>> document_terms(200);
        for (DocId id=0;id<document_terms.size();++id) {
            all.insert(id);
            for (unsigned t=0;t<term_sets.size();++t) if (rng()%3) {
                term_sets[t].insert(id); document_terms[id].push_back("t"+std::to_string(t));
            }
        }
        const auto build=[&](const fs::path& path,std::size_t budget) {
            Builder b(path,budget);
            for (const auto& terms:document_terms) {
                const auto id=b.add_document("{}");
                for (unsigned repeat=0;repeat<3;++repeat) for (const auto& term:terms) b.add(term,id);
            }
            b.finish({}); return b.segment_count();
        };
        require(build(f.root/"many",4096)>256,"Fixture must exercise multiple merge levels");
        require(build(f.root/"one",8*1024*1024)==1,"Expected one segment");
        Index many(f.root/"many"), one(f.root/"one"); many.verify(); one.verify();
        for (unsigned t=0;t<term_sets.size();++t) {
            auto stream=many.lookup_postings("t"+std::to_string(t));
            for (const auto id:term_sets[t]) {
                require(stream && stream->value()==id && stream->title_frequency()==0 &&
                        stream->text_frequency()==3,"Multi-level merge lost term frequencies");
                stream->next();
            }
            require(stream && stream->value()==end_id,"Unexpected merged posting");
        }
        for (const auto* name:{"terms.bin","term_offsets.bin","postings.bin","documents.bin","document_offsets.bin","doc_norms.bin"})
            require(file_sha256(f.root/"many"/name)==file_sha256(f.root/"one"/name),"Merge changes index contents");
        for (unsigned i=0;i<120;++i) {
            const auto e=expression(rng,4,term_sets,all);
            expect(many,e.text,std::vector<DocId>(e.expected.begin(),e.expected.end()));
        }
        {
            std::fstream out(f.root/"simple"/"postings.bin",std::ios::in|std::ios::out|std::ios::binary);
            out.put('\xff');
        }
        rejects([&] { simple.verify(); },"Corruption not detected by checksum");
        fs::resize_file(f.root/"one"/"terms.bin",1);
        rejects([&] { Index damaged(f.root/"one"); },"Truncated index opened");
        for (const auto& entry:fs::directory_iterator(f.root))
            require(entry.path().filename().string().find(".tmp-")==std::string::npos,"Temporary files leaked");
        std::cout << "Boolean core tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
