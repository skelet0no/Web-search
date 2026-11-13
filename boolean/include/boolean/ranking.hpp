#pragma once
#include "query.hpp"

namespace boolean_search {
inline constexpr std::size_t max_ranked_window = 100000;
struct ScoredDocument {
    DocId document;
    double score;
};
struct RankedResult {
    std::uint64_t total = 0;
    std::vector<ScoredDocument> documents;
};


class Ranker {
public:
    explicit Ranker(const Index& index, const std::function<void()>& check = {});
    RankedResult search(const std::string& query, const Normalize& normalize,
                        std::size_t limit = 20, std::uint64_t offset = 0,
                        const std::function<void()>& check = {}) const;
private:
    const Index& index_;
    std::vector<double> norms_;
};
}
