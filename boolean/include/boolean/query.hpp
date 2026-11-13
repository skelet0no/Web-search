#pragma once
#include "index.hpp"

namespace boolean_search {

using Normalize = std::function<std::vector<std::vector<std::string>>(const std::string&)>;
struct SearchResult {
    std::uint64_t total = 0;
    std::vector<DocId> documents;
};
SearchResult search(const Index& index, const std::string& query,
                    const Normalize& normalize, std::size_t limit = 20,
                    std::uint64_t offset = 0, const std::function<void()>& check = {});
}
