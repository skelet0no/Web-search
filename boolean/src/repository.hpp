#pragma once
#include "boolean/index.hpp"
#include <filesystem>

namespace boolean_search {
struct BuildOptions {
    std::string uri="mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=10000";
    std::string database="crawler", lemmatization_run;
    std::filesystem::path output;
    std::size_t memory_bytes=256*1024*1024;
};
void build_from_mongo(const BuildOptions& options, const std::function<void()>& check = {});
}
