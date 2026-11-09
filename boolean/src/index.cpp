#include "boolean/index.hpp"
#include "io.hpp"
#include "text/unicode.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/fs.h>

namespace boolean_search {
namespace fs = std::filesystem;
namespace {
constexpr std::array<const char*, 5> files = {
    "terms.bin", "term_offsets.bin", "postings.bin", "documents.bin", "document_offsets.bin"};
void put_map(std::ostream& out, const Metadata& m) {
    io::put(out, m.size(), 4);
    for (const auto& [k,v] : m) { io::put_string(out,k); io::put_string(out,v); }
}
Metadata get_map(std::istream& in) {
    Metadata m;
    const auto n = io::get(in,4);
    if (n > 1024) throw std::runtime_error("Too many manifest entries");
    for (std::uint64_t i=0; i<n; ++i) {
        auto k = io::get_string(in), v = io::get_string(in);
        if (!m.emplace(k,v).second) throw std::runtime_error("Duplicate manifest key");
    }
    return m;
}
void sync_path(const fs::path& path, bool directory = false) {
    const int fd = open(path.c_str(), O_RDONLY | (directory ? O_DIRECTORY : 0));
    if (fd < 0) throw std::runtime_error("Cannot open for sync: " + path.string());
    const int result = fsync(fd);
    close(fd);
    if (result != 0) throw std::runtime_error("Cannot sync " + path.string());
}
struct Range final : Cursor {
    std::uint64_t current = 0, count;
    explicit Range(std::uint64_t n) : count(n) {}
    std::uint64_t value() const override { return current < count ? current : end_id; }
    void next() override { if (current < count) ++current; }
};
struct Binary final : Cursor {
    Stream a,b;
    char operation;
    std::uint64_t current = end_id;
    Binary(Stream x, Stream y, char op) : a(std::move(x)), b(std::move(y)), operation(op) { align(); }
    void align() {
        if (operation == '|') { current = std::min(a->value(),b->value()); return; }
        while (a->value() != end_id) {
            if (operation == '&') {
                if (b->value() == end_id) break;
                if (a->value() == b->value()) { current=a->value(); return; }
                if (a->value() < b->value()) a->next(); else b->next();
            } else {
                while (b->value() < a->value()) b->next();
                if (a->value() != b->value()) { current=a->value(); return; }
                a->next();
            }
        }
        current=end_id;
    }
    std::uint64_t value() const override { return current; }
    void next() override {
        if (current == end_id) return;
        if (a->value()==current) a->next();
        if (b->value()==current) b->next();
        align();
    }
};
struct PostingCursor final : Cursor {
    std::ifstream in;
    std::uint64_t remaining, current=end_id, previous=end_id, documents;
    PostingCursor(const fs::path& path, std::uint64_t offset, std::uint64_t n, std::uint64_t docs)
        : in(io::input(path)), remaining(n), documents(docs) { io::seek(in,offset); next(); }
    std::uint64_t value() const override { return current; }
    void next() override {
        if (!remaining) { current=end_id; return; }
        current=io::get(in,4); --remaining;
        if (current>=documents || (previous!=end_id && current<=previous))
            throw std::runtime_error("Invalid posting order or document ID");
        previous=current;
    }
};
using Pair = std::pair<std::string, DocId>;
struct Segment {
    std::ifstream in;
    Pair pair;
    bool valid=false;
    explicit Segment(const fs::path& p) : in(io::input(p)) { next(); }
    void next() {
        valid = in.peek()!=std::char_traits<char>::eof();
        if (valid) { pair.first=io::get_string(in); pair.second=static_cast<DocId>(io::get(in,4)); }
    }
};
template<class Emit> void merge(const std::vector<fs::path>& paths, Emit emit,
                               const std::function<void()>& check) {
    std::vector<Segment> streams;
    streams.reserve(paths.size());
    for (const auto& p : paths) streams.emplace_back(p);
    auto greater = [&](std::size_t a, std::size_t b) { return streams[a].pair>streams[b].pair; };
    std::priority_queue<std::size_t,std::vector<std::size_t>,decltype(greater)> q(greater);
    for (std::size_t i=0;i<streams.size();++i) if (streams[i].valid) q.push(i);
    Pair prior;
    bool have=false;
    std::uint64_t processed=0;
    while (!q.empty()) {
        if ((processed++ % 4096)==0 && check) check();
        const auto i=q.top(); q.pop();
        const auto& p=streams[i].pair;
        if (!have || p!=prior) { emit(p); prior=p; have=true; }
        streams[i].next();
        if (streams[i].valid) q.push(i);
    }
}
}
Stream universe(std::uint64_t count) { return std::make_unique<Range>(count); }
Stream intersect(Stream a, Stream b) { return std::make_unique<Binary>(std::move(a),std::move(b),'&'); }
Stream unite(Stream a, Stream b) { return std::make_unique<Binary>(std::move(a),std::move(b),'|'); }
Stream difference(Stream a, Stream b) { return std::make_unique<Binary>(std::move(a),std::move(b),'-'); }
std::string json_quote(const std::string& s) {
    std::ostringstream out; out << '"';
    for (unsigned char c:s) {
        if (c=='"' || c=='\\') out << '\\' << c;
        else if (c<32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else out << c;
    }
    out << '"'; return out.str();
}
std::string file_sha256(const fs::path& path) {
    auto in=io::input(path);
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)!=1) throw std::runtime_error("SHA256 init failed");
    std::array<char,65536> buffer{};
    while (in) {
        in.read(buffer.data(),buffer.size());
        if (EVP_DigestUpdate(ctx.get(),buffer.data(),static_cast<std::size_t>(in.gcount()))!=1)
            throw std::runtime_error("SHA256 update failed");
    }
    if (!in.eof()) throw std::runtime_error("Cannot read " + path.string());
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned n=0;
    if (EVP_DigestFinal_ex(ctx.get(),digest,&n)!=1) throw std::runtime_error("SHA256 final failed");
    std::ostringstream out;
    for (unsigned i=0;i<n;++i) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(digest[i]);
    return out.str();
}
struct Builder::Impl {
    fs::path destination, temporary;
    std::size_t budget, bytes=0;
    std::vector<Pair> buffer;
    std::vector<std::vector<fs::path>> levels;
    std::uint64_t documents=0, terms=0, links=0, segments=0, serial=0;
    std::ofstream docs, offsets;
    std::function<void()> check;
    bool published=false;
    Impl(fs::path output, std::size_t memory, std::function<void()> callback)
        : destination(fs::absolute(output)), budget(memory), check(std::move(callback)) {
        if (budget<4096) throw std::invalid_argument("Memory budget must be at least 4096 bytes");
        if (fs::exists(destination)) throw std::runtime_error("Index directory already exists");
        fs::create_directories(destination.parent_path());
        std::string pattern=destination.string()+".tmp-XXXXXX";
        std::vector<char> name(pattern.begin(),pattern.end()); name.push_back('\0');
        if (!mkdtemp(name.data())) throw std::runtime_error("Cannot reserve temporary index directory");
        temporary=name.data();
        try {
            docs=io::output(temporary/"documents.bin"); offsets=io::output(temporary/"document_offsets.bin");
        } catch (...) { fs::remove_all(temporary); throw; }
    }
    ~Impl() { if (!published) { std::error_code ec; fs::remove_all(temporary,ec); } }
    fs::path path() { return temporary/("segment-"+std::to_string(serial++)); }
    fs::path combine(const std::vector<fs::path>& paths) {
        const auto target=path(); auto out=io::output(target);
        merge(paths,[&](const Pair& p) { io::put_string(out,p.first); io::put(out,p.second,4); },check);
        out.close();
        for (const auto& p:paths) fs::remove(p);
        return target;
    }
    void carry(fs::path p, std::size_t level=0) {
        if (levels.size()<=level) levels.resize(level+1);
        levels[level].push_back(std::move(p));
        if (levels[level].size()==16) {
            const auto merged=combine(levels[level]); levels[level].clear(); carry(merged,level+1);
        }
    }
    void flush() {
        if (buffer.empty()) return;
        if (check) check();
        std::sort(buffer.begin(),buffer.end());
        auto last=std::unique(buffer.begin(),buffer.end());
        auto target=path(); auto out=io::output(target);
        for (auto it=buffer.begin();it!=last;++it) { io::put_string(out,it->first); io::put(out,it->second,4); }
        out.close();
        std::vector<Pair>().swap(buffer); bytes=0; ++segments; carry(target);
    }
};
Builder::Builder(fs::path output,std::size_t memory,std::function<void()> check)
    : impl_(std::make_unique<Impl>(std::move(output),memory,std::move(check))) {}
Builder::~Builder()=default;
std::uint64_t Builder::segment_count() const { return impl_->segments; }
DocId Builder::add_document(const std::string& json) {
    auto& b=*impl_;
    if (b.published) throw std::logic_error("Builder already finished");
    if (b.check) b.check();
    if (b.documents>=UINT32_MAX) throw std::overflow_error("Too many documents for v1 index");
    text::validate_utf8(json);
    io::put(b.offsets,io::position(b.docs)); io::put_string(b.docs,json);
    return static_cast<DocId>(b.documents++);
}
void Builder::add(const std::string& term,DocId id) {
    auto& b=*impl_;
    if (b.published || id>=b.documents) throw std::invalid_argument("Invalid document ID");
    if (term.empty() || term.size()>io::max_string) throw std::invalid_argument("Invalid term length");
    text::validate_utf8(term);
    // Include string storage, vector growth, allocator overhead and sort workspace.
    const auto cost=term.size()+2*sizeof(Pair)+64;
    if (cost>b.budget) throw std::runtime_error("Single term exceeds memory budget");
    if (b.bytes+cost>b.budget) b.flush();
    b.buffer.emplace_back(term,id); b.bytes+=cost;
}
void Builder::finish(Metadata metadata) {
    const auto started=std::chrono::steady_clock::now();
    auto& b=*impl_;
    if (b.published) throw std::logic_error("Builder already finished");
    b.flush(); b.docs.close(); b.offsets.close();
    std::vector<fs::path> paths;
    for (const auto& level:b.levels) for (const auto& p:level) paths.push_back(p);
    while (paths.size()>16) {
        std::vector<fs::path> next;
        for (std::size_t i=0;i<paths.size();i+=16)
            next.push_back(b.combine(std::vector<fs::path>(paths.begin()+i,paths.begin()+std::min(i+16,paths.size()))));
        paths=std::move(next);
    }
    auto terms=io::output(b.temporary/"terms.bin"), offsets=io::output(b.temporary/"term_offsets.bin"),
         postings=io::output(b.temporary/"postings.bin");
    std::string term;
    std::uint64_t count=0, start=0;
    auto emit=[&] {
        if (!count) return;
        io::put(offsets,io::position(terms)); io::put_string(terms,term);
        io::put(terms,start); io::put(terms,count); ++b.terms;
    };
    merge(paths,[&](const Pair& p) {
        if (p.first!=term) { emit(); term=p.first; count=0; start=io::position(postings); }
        io::put(postings,p.second,4); ++count; ++b.links;
    },b.check);
    emit(); terms.close(); offsets.close(); postings.close();
    for (const auto& p:paths) fs::remove(p);
    metadata["format"]="boolean-v1";
    metadata["state"]="completed";
    metadata["fields"]="title,text";
    metadata["lemma_policy"]="all-candidates";
    metadata["memory_budget_bytes"]=std::to_string(b.budget);
    metadata["segments"]=std::to_string(b.segments);
    Metadata hashes;
    for (const auto* name:files) {
        if (b.check) b.check();
        hashes[name]=file_sha256(b.temporary/name);
    }
    metadata["finalize_seconds_before_verification"]=std::to_string(
        std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count());
    auto manifest=io::output(b.temporary/"manifest.bin");
    manifest.write("BOOLIDX1",8);
    io::put(manifest,b.documents); io::put(manifest,b.terms); io::put(manifest,b.links);
    put_map(manifest,metadata); put_map(manifest,hashes);
    for (const auto* name:files) io::put(manifest,fs::file_size(b.temporary/name));
    manifest.close();
    auto json=io::output(b.temporary/"manifest.json");
    json << "{\n\"documents\":" << b.documents << ",\"terms\":" << b.terms << ",\"postings\":" << b.links;
    for (const auto& [k,v]:metadata) json << ",\n" << json_quote(k) << ':' << json_quote(v);
    json << ",\n\"sha256\":{"; bool first=true;
    for (const auto& [k,v]:hashes) { if (!first) json << ','; first=false; json << json_quote(k) << ':' << json_quote(v); }
    json << "}}\n"; json.close();
    auto statistics=io::output(b.temporary/"statistics.json");
    std::uint64_t data_bytes=0;
    for (const auto* name:files) data_bytes+=fs::file_size(b.temporary/name);
    statistics << "{\"documents\":" << b.documents << ",\"terms\":" << b.terms
        << ",\"postings\":" << b.links << ",\"data_bytes\":" << data_bytes
        << ",\"segments\":" << b.segments << ",\"sort_buffer_budget_bytes\":" << b.budget << "}\n";
    statistics.close();
    auto report=io::output(b.temporary/"report.md");
    report << "# Булев индекс\n\nДокументов: " << b.documents << ". Терминов: " << b.terms
           << ". Связей: " << b.links << ". Сегментов: " << b.segments << ".\n";
    report.close();
    Index(b.temporary).verify(b.check);
    for (const auto& entry:fs::directory_iterator(b.temporary)) sync_path(entry.path());
    sync_path(b.temporary,true);
    if (b.check) b.check();
    // Linux renameat2 guarantees that a concurrently created destination is never replaced.
    if (syscall(SYS_renameat2,AT_FDCWD,b.temporary.c_str(),AT_FDCWD,b.destination.c_str(),RENAME_NOREPLACE)!=0)
        throw std::runtime_error("Cannot publish index (destination must not exist): " + b.destination.string());
    b.published=true; sync_path(b.destination.parent_path(),true);
}
Index::Index(fs::path path) : directory_(std::move(path)) {
    auto in=io::input(directory_/"manifest.bin"); char magic[8];
    if (!in.read(magic,8) || std::string(magic,8)!="BOOLIDX1") throw std::runtime_error("Unsupported index format");
    documents_=io::get(in); terms_=io::get(in); links_=io::get(in);
    if (documents_>UINT32_MAX || terms_>UINT64_MAX/8 || links_>UINT64_MAX/4)
        throw std::runtime_error("Invalid manifest counts");
    metadata_=get_map(in); hashes_=get_map(in);
    if (metadata_["format"]!="boolean-v1" || metadata_["state"]!="completed" ||
        metadata_["lemma_policy"]!="all-candidates" || metadata_["fields"]!="title,text")
        throw std::runtime_error("Incompatible or incomplete index");
    for (const auto* name:files) {
        const auto size=io::get(in);
        if (fs::file_size(directory_/name)!=size || hashes_[name].size()!=64)
            throw std::runtime_error(std::string("Invalid file size or missing checksum: ")+name);
    }
    io::eof(in);
    if (fs::file_size(directory_/"term_offsets.bin")!=terms_*8 ||
        fs::file_size(directory_/"document_offsets.bin")!=documents_*8 ||
        fs::file_size(directory_/"postings.bin")!=links_*4)
        throw std::runtime_error("Index counts do not match file sizes");
}
Index::Posting Index::find(const std::string& key) const {
    auto offsets=io::input(directory_/"term_offsets.bin"), terms=io::input(directory_/"terms.bin");
    const auto size=fs::file_size(directory_/"terms.bin");
    std::uint64_t low=0, high=terms_;
    while (low<high) {
        const auto mid=low+(high-low)/2; io::seek(offsets,mid*8); const auto offset=io::get(offsets);
        if (offset>=size) throw std::runtime_error("Invalid term offset");
        io::seek(terms,offset); const auto term=io::get_string(terms);
        if (term<key) low=mid+1;
        else if (term>key) high=mid;
        else {
            Posting p{io::get(terms),io::get(terms)};
            if (p.offset%4 || p.offset/4>links_ || p.count>links_-p.offset/4 || p.count>documents_ || !p.count)
                throw std::runtime_error("Invalid posting bounds");
            return p;
        }
    }
    return {};
}
Stream Index::lookup(const std::string& term) const {
    const auto p=find(term);
    if (!p.count) return universe(0);
    return std::make_unique<PostingCursor>(directory_/"postings.bin",p.offset,p.count,documents_);
}
std::uint64_t Index::frequency(const std::string& term) const { return find(term).count; }
std::string Index::document(DocId id) const {
    if (id>=documents_) throw std::out_of_range("Invalid document ID");
    auto offsets=io::input(directory_/"document_offsets.bin"), docs=io::input(directory_/"documents.bin");
    io::seek(offsets,std::uint64_t(id)*8); const auto offset=io::get(offsets);
    if (offset>=fs::file_size(directory_/"documents.bin")) throw std::runtime_error("Invalid document offset");
    io::seek(docs,offset); return io::get_string(docs);
}
void Index::verify(const std::function<void()>& check) const {
    for (const auto* name:files) {
        if (check) check();
        if (file_sha256(directory_/name)!=hashes_.at(name)) throw std::runtime_error(std::string("Checksum mismatch: ")+name);
    }
    auto terms=io::input(directory_/"terms.bin"), offsets=io::input(directory_/"term_offsets.bin"),
         postings=io::input(directory_/"postings.bin");
    std::string prior;
    std::uint64_t expected=0;
    for (std::uint64_t i=0;i<terms_;++i) {
        if (check) check();
        if (io::get(offsets)!=static_cast<std::uint64_t>(terms.tellg())) throw std::runtime_error("Noncontiguous term offset");
        auto term=io::get_string(terms); text::validate_utf8(term);
        if (term.empty() || (i && term<=prior)) throw std::runtime_error("Invalid term order");
        prior=std::move(term);
        const auto start=io::get(terms), count=io::get(terms);
        if (start!=expected*4 || !count || count>documents_ || count>links_-expected)
            throw std::runtime_error("Invalid posting bounds");
        std::uint64_t previous=end_id;
        for (std::uint64_t j=0;j<count;++j) {
            if ((j%4096)==0 && check) check();
            const auto id=io::get(postings,4);
            if (id>=documents_ || (j && id<=previous)) throw std::runtime_error("Invalid posting order");
            previous=id;
        }
        expected+=count;
    }
    if (expected!=links_) throw std::runtime_error("Posting count mismatch");
    io::eof(terms); io::eof(offsets); io::eof(postings);
    auto docs=io::input(directory_/"documents.bin"), doc_offsets=io::input(directory_/"document_offsets.bin");
    for (std::uint64_t i=0;i<documents_;++i) {
        if (check) check();
        if (io::get(doc_offsets)!=static_cast<std::uint64_t>(docs.tellg())) throw std::runtime_error("Noncontiguous document offset");
        text::validate_utf8(io::get_string(docs));
    }
    io::eof(docs); io::eof(doc_offsets);
}
}
