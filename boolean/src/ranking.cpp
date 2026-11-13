#include "boolean/ranking.hpp"
#include "boolean/weighting.hpp"
#include "text/unicode.hpp"
#include <algorithm>
#include <map>
#include <queue>
#include <stdexcept>

namespace boolean_search {
namespace {
struct Better {
    bool operator()(const ScoredDocument& a, const ScoredDocument& b) const {
        return a.score>b.score || (a.score==b.score && a.document<b.document);
    }
};
struct Term {
    PostingStream stream;
    double idf, query_weight;
};
}
Ranker::Ranker(const Index& index, const std::function<void()>& check)
    : index_(index), norms_(index.document_norms(check)) {}

RankedResult Ranker::search(const std::string& query, const Normalize& normalize,
                            std::size_t limit, std::uint64_t offset,
                            const std::function<void()>& check) const {
    if (check) check();
    if (limit>max_ranked_window || (limit && offset>max_ranked_window-limit))
        throw std::invalid_argument("TF-IDF requires offset + limit <= 100000 (except count-only queries)");
    if (query.size()>16384) throw std::invalid_argument("Query exceeds 16384 bytes");
    text::validate_utf8(query);
    auto groups=normalize(query);
    if (groups.empty()) throw std::invalid_argument("No searchable query tokens");
    if (groups.size()>256) throw std::invalid_argument("Query exceeds 256 tokens");
    std::map<std::string,std::uint64_t> frequencies;
    std::size_t expansions=0;
    for (auto& group:groups) {
        if (check) check();
        std::sort(group.begin(),group.end());
        group.erase(std::unique(group.begin(),group.end()),group.end());
        if (group.empty()) throw std::invalid_argument("Empty query term expansion");
        for (const auto& term:group) {
            if (term.empty() || ++expansions>512) throw std::invalid_argument("Too many or empty query terms");
            text::validate_utf8(term);
            ++frequencies[term];
        }
    }
    std::vector<Term> terms;
    double query_squared_norm=0;
    for (const auto& [term,frequency]:frequencies) {
        if (check) check();
        const auto df=index_.document_frequency(term);
        if (!df) continue;
        const auto idf=inverse_document_frequency(index_.document_count(),df);
        const auto weight=term_weight(frequency,idf);
        query_squared_norm+=weight*weight;
        terms.push_back({index_.lookup_postings(term),idf,weight});
    }
    RankedResult result;
    if (terms.empty()) return result;
    const auto query_norm=std::sqrt(query_squared_norm);


    auto later=[&](std::size_t a,std::size_t b) {
        const auto da=terms[a].stream->value(), db=terms[b].stream->value();
        return da>db || (da==db && a>b);
    };
    std::priority_queue<std::size_t,std::vector<std::size_t>,decltype(later)> heads(later);
    for (std::size_t i=0;i<terms.size();++i) heads.push(i);
    std::priority_queue<ScoredDocument,std::vector<ScoredDocument>,Better> best;
    const auto keep=limit ? static_cast<std::size_t>(offset)+limit : 0;
    std::uint64_t visited=0;
    while (!heads.empty()) {
        if (check) check();
        const auto document=terms[heads.top()].stream->value();
        double dot=0;
        do {
            if ((visited++%4096)==0 && check) check();
            const auto term_index=heads.top(); heads.pop();
            auto& term=terms[term_index];
            const auto frequency=checked_frequency_sum(term.stream->title_frequency(),term.stream->text_frequency());
            dot+=term_weight(frequency,term.idf)*term.query_weight;
            term.stream->next();
            if (term.stream->value()!=end_id) {
                heads.push(term_index);
            }
        } while (!heads.empty() && terms[heads.top()].stream->value()==document);
        ++result.total;
        const auto norm=norms_.at(static_cast<std::size_t>(document));
        if (!norm) throw std::runtime_error("Zero norm for nonempty document; run --verify");
        const auto score=dot/(norm*query_norm);
        if (!std::isfinite(score) || score<0 || score>1.0+1e-10)
            throw std::runtime_error("Invalid TF-IDF score; run --verify");
        const ScoredDocument hit{static_cast<DocId>(document),std::min(1.0,score)};
        if (keep) {
            if (best.size()<keep) best.push(hit);
            else if (Better{}(hit,best.top())) { best.pop(); best.push(hit); }
        }
    }
    std::vector<ScoredDocument> ordered;
    ordered.reserve(best.size());
    while (!best.empty()) { ordered.push_back(best.top()); best.pop(); }
    std::sort(ordered.begin(),ordered.end(),Better{});
    if (limit && offset<ordered.size())
        result.documents.assign(ordered.begin()+static_cast<std::size_t>(offset),ordered.end());
    return result;
}
}
