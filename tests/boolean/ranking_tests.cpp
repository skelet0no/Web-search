#include "boolean/ranking.hpp"
#include "tokenizer/tokenizer.hpp"
#include "io.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <unistd.h>

namespace {
using namespace boolean_search;
namespace fs=std::filesystem;
void require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F action,const char* message) {
    bool rejected=false;
    try { action(); } catch (const std::exception&) { rejected=true; }
    require(rejected,message);
}
struct Fixture {
    fs::path root;
    Fixture() {
        std::string pattern=(fs::temp_directory_path()/"ranking-tests-XXXXXX").string();
        std::vector<char> path(pattern.begin(),pattern.end()); path.push_back('\0');
        if (!mkdtemp(path.data())) throw std::runtime_error("Cannot create fixture");
        root=path.data();
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root,ec); }
};
const Normalize normalize=[](const std::string& raw) {
    std::vector<std::vector<std::string>> groups;
    for (const auto& token:tokenizer::tokenize(raw)) groups.push_back({token.normalized});
    return groups;
};
void close(double a,double b,const char* message) {
    require(std::abs(a-b)<1e-12,message);
}
void codec() {
    for (const std::uint64_t value:std::array<std::uint64_t,10>{0,1,127,128,129,16383,16384,UINT32_MAX,
                                                            UINT64_MAX-1,UINT64_MAX}) {
        std::ostringstream out(std::ios::binary|std::ios::out);
        io::put_varint(out,value);
        const auto bytes=out.str();
        std::istringstream in(bytes,std::ios::binary|std::ios::in);
        std::uint64_t remaining=bytes.size();
        require(io::get_varint(in,remaining)==value && !remaining,"Varint round trip");
        io::eof(in);
    }
    std::ostringstream known;
    io::put_varint(known,128);
    require(known.str()==std::string("\x80\x01",2),"ULEB128 wire convention");
    for (const auto& bad:std::vector<std::string>{std::string("\x80",1), std::string("\x80\x00",2),
                                                std::string(10,'\x80'),
                                                std::string(9,'\xff')+std::string("\x02",1)}) {
        rejects([&] {
            std::istringstream in(bad);
            std::uint64_t remaining=bad.size();
            io::get_varint(in,remaining);
        },"Malformed varint accepted");
    }
    rejects([] {
        std::istringstream in(std::string("\x80\x01",2));
        std::uint64_t boundary=1;
        io::get_varint(in,boundary);
    },"Varint crossed posting boundary");
    std::ostringstream out;
    io::put_double(out,1.25);
    require(out.str()==std::string("\0\0\0\0\0\0\xf4\x3f",8),"Norm little endian representation");
    std::istringstream in(out.str()); close(io::get_double(in),1.25,"Norm round trip");
}


std::vector<ScoredDocument> oracle(const std::array<std::array<unsigned,3>,5>& counts,
                                   const std::array<unsigned,3>& query) {
    std::array<double,3> idf{}, q{};
    double q2=0;
    for (std::size_t t=0;t<3;++t) {
        unsigned df=0;
        for (const auto& row:counts) df+=row[t]>0;
        idf[t]=1+std::log(6.0/(df+1.0));
        q[t]=query[t] ? (1+std::log(query[t]))*idf[t] : 0;
        q2+=q[t]*q[t];
    }
    std::vector<ScoredDocument> result;
    for (DocId d=0;d<counts.size();++d) {
        double dot=0,norm2=0;
        for (std::size_t t=0;t<3;++t) {
            const auto w=counts[d][t] ? (1+std::log(counts[d][t]))*idf[t] : 0;
            norm2+=w*w; dot+=w*q[t];
        }
        if (dot>0) result.push_back({d,dot/std::sqrt(norm2*q2)});
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b) {
        return a.score>b.score || (a.score==b.score && a.document<b.document);
    });
    return result;
}
void assert_results(const RankedResult& actual,const std::vector<ScoredDocument>& expected) {
    require(actual.total==expected.size() && actual.documents.size()==expected.size(),"Ranked result count");
    for (std::size_t i=0;i<expected.size();++i) {
        require(actual.documents[i].document==expected[i].document,"Ranked result order");
        close(actual.documents[i].score,expected[i].score,"Cosine differs from dense oracle");
    }
}
void replace_checksum(const fs::path& directory,const std::string& name,const std::string& old_hash) {
    auto input=io::input(directory/"manifest.bin");
    std::string bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>{});
    const auto pos=bytes.find(old_hash);
    require(pos!=std::string::npos,"Missing checksum in fixture manifest");
    bytes.replace(pos,old_hash.size(),file_sha256(directory/name));
    auto output=io::output(directory/"manifest.bin");
    output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
}
}
int main() {
    try {
        codec();
        Fixture f;
        {
            Builder builder(f.root/"ranked",4096);
            for (unsigned d=0;d<5;++d) builder.add_document("{}");
            builder.add("alpha",0,1,0); builder.add("alpha",0,0,2); builder.add("beta",0);
            for (DocId d:{1u,4u}) { builder.add("alpha",d); builder.add("gamma",d); }
            builder.add("beta",2);
            builder.finish({});
        }
        Index index(f.root/"ranked"); index.verify();
        auto posting=index.lookup_postings("alpha");
        require(posting && posting->value()==0 && posting->title_frequency()==1 &&
                posting->text_frequency()==2,"Field TF aggregation");
        posting->next(); require(posting->value()==1,"Posting gap decoding");
        posting->next(); require(posting->value()==4,"Posting gap decoding");
        posting->next(); require(posting->value()==end_id,"Posting end");
        require(!index.lookup_postings("missing"),"Missing posting stream");
        require(index.document_frequency("alpha")==3,"DF must count unique documents");
        const auto norms=index.document_norms(); require(norms[3]==0,"Empty document norm");
        const Ranker ranker(index);
        const std::array<std::array<unsigned,3>,5> counts{{{3,1,0},{1,0,1},{0,1,0},{0,0,0},{1,0,1}}};
        for (const auto& q:std::vector<std::pair<std::string,std::array<unsigned,3>>>{
                 {"alpha",{1,0,0}},{"alpha beta",{1,1,0}},
                 {"alpha beta beta",{1,2,0}},{"gamma",{0,0,1}}})
            assert_results(ranker.search(q.first,normalize,100),oracle(counts,q.second));
        const auto all=ranker.search("alpha beta",normalize,100);
        const auto page=ranker.search("alpha beta",normalize,1,1);
        require(page.total==all.total && page.documents.size()==1 &&
                page.documents[0].document==all.documents[1].document,"Rank before pagination");
        require(ranker.search("alpha",normalize,0,UINT64_MAX).total==3,"Count-only search");
        require(ranker.search("alpha",normalize,1,99).documents.empty(),"Offset past total");
        require(ranker.search("missing",normalize).total==0,"Unknown query");
        const auto tied=ranker.search("gamma",normalize,1);
        require(tied.total==2 && tied.documents[0].document==1,"Tie at top-k boundary must prefer lower ID");
        assert_results(ranker.search("alpha missing",normalize,100),oracle(counts,{1,0,0}));
        const Normalize duplicates=[](const std::string&) {
            return std::vector<std::vector<std::string>>{{"alpha","alpha"},{"beta"}};
        };
        assert_results(ranker.search("alias",duplicates,100),oracle(counts,{1,1,0}));

        require(search(index,"alpha beta",normalize).documents==std::vector<DocId>{0},"Boolean mode changed");
        require(all.total==4,"Ranked mode must include partial matches");
        rejects([&] { ranker.search("",normalize); },"Empty ranked query accepted");
        rejects([&] { ranker.search(std::string(16385,'a'),normalize); },"Long ranked query accepted");
        rejects([&] { ranker.search(std::string("\xff"),normalize); },"Invalid ranked UTF8 accepted");
        rejects([&] { ranker.search("alpha",normalize,20,99999); },"Unbounded ranked window");
        rejects([&] { ranker.search("alpha",normalize,1,UINT64_MAX); },"Offset overflow");
        rejects([&] {
            const Normalize excessive=[](const std::string&) {
                return std::vector<std::vector<std::string>>(257,{"alpha"});
            };
            ranker.search("alpha",excessive);
        },"Query token limit ignored");
        rejects([&] {
            const Normalize excessive=[](const std::string&) {
                std::vector<std::string> terms;
                for (unsigned i=0;i<513;++i) terms.push_back("t"+std::to_string(i));
                return std::vector<std::vector<std::string>>{terms};
            };
            ranker.search("alpha",excessive);
        },"Query expansion limit ignored");
        rejects([&] { ranker.search("alpha",normalize,20,0,[] { throw std::runtime_error("stop"); }); },"Cancellation ignored");
        {
            Builder empty(f.root/"empty",4096); empty.finish({});
            Index e(f.root/"empty"); e.verify();
            require(Ranker(e).search("alpha",normalize).total==0,"Empty ranked index");
            {
                std::fstream manifest(f.root/"empty"/"manifest.bin",std::ios::binary|std::ios::in|std::ios::out);
                manifest.write("BOOLIDX1",8);
            }
            rejects([&] { Index legacy(f.root/"empty"); },"Legacy format silently accepted");
        }
        {
            Builder overflow(f.root/"overflow",4096); overflow.add_document("{}");
            overflow.add("x",0,UINT64_MAX,0); overflow.add("x",0,0,1);
            rejects([&] { overflow.finish({}); },"TF overflow accepted");
            require(!fs::exists(f.root/"overflow"),"Overflow index published");
        }
        {
            Builder sparse(f.root/"sparse",4096);
            for (unsigned i=0;i<200;++i) sparse.add_document("{}");
            sparse.add("gap",0); sparse.add("gap",128); sparse.add("gap",199);
            sparse.add("large-tf",199,UINT64_MAX,0);
            sparse.finish({});
            Index s(f.root/"sparse"); s.verify();
            auto stream=s.lookup_postings("gap");
            for (const auto id:{0u,128u,199u}) {
                require(stream->value()==id,"Multi-byte gap decoding"); stream->next();
            }
            require(stream->value()==end_id,"Sparse posting end");
            require(s.lookup_postings("large-tf")->title_frequency()==UINT64_MAX,"Large frequency truncated");
            require(fs::file_size(f.root/"sparse"/"postings.bin")<4*20,"Postings were not compressed");
            const auto old_postings_hash=file_sha256(f.root/"sparse"/"postings.bin");
            {
                std::fstream output(f.root/"sparse"/"postings.bin",std::ios::binary|std::ios::in|std::ios::out);
                output.put('\0');
            }
            replace_checksum(f.root/"sparse","postings.bin",old_postings_hash);
            rejects([&] { Index damaged(f.root/"sparse"); damaged.lookup("gap"); },"Zero gap accepted by reader");
            rejects([&] { Index damaged(f.root/"sparse"); damaged.verify(); },"Zero gap accepted by verifier");
        }

        const auto old_hash=file_sha256(f.root/"ranked"/"doc_norms.bin");
        {
            std::fstream output(f.root/"ranked"/"doc_norms.bin",std::ios::binary|std::ios::in|std::ios::out);
            io::put_double(output,42.0);
        }
        replace_checksum(f.root/"ranked","doc_norms.bin",old_hash);
        rejects([&] { Index damaged(f.root/"ranked"); damaged.verify(); },"Incorrect norm accepted");
        std::cout << "Compression and TF-IDF tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
