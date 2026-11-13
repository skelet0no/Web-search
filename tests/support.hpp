#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace test_support {
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Exception=std::exception, class F>
void rejects(F action, const char* message) {
    bool caught=false;
    try { action(); } catch (const Exception&) { caught=true; }
    require(caught,message);
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        const auto pattern=(std::filesystem::temp_directory_path()/"web-search-unit-XXXXXX").string();
        std::vector<char> chars(pattern.begin(),pattern.end()); chars.push_back('\0');
        if (!mkdtemp(chars.data())) throw std::runtime_error("Cannot create test directory");
        path=chars.data();
    }
    Directory(const Directory&)=delete;
    Directory& operator=(const Directory&)=delete;
    ~Directory() { std::error_code ec; std::filesystem::remove_all(path,ec); }
};
inline std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read test artifact");
    return std::string(std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>{});
}
}
