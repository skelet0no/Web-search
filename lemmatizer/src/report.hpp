#pragma once
#include "options.hpp"
#include "statistics.hpp"
#include <bsoncxx/document/view.hpp>
namespace lemmatizer {
void write_report(const Options& options, const Statistics& statistics, bsoncxx::document::view metadata);
}
