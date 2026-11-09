#pragma once
#include "query.hpp"
namespace lemmatizer { class Analyzer; }
namespace boolean_search {
std::vector<std::vector<std::string>> normalize_query(const std::string& raw, lemmatizer::Analyzer& analyzer);
void require_compatible(const Metadata& metadata);
}
