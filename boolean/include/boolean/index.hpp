#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace boolean_search {
using DocId = std::uint32_t;
using Metadata = std::map<std::string, std::string>;
inline constexpr std::uint64_t end_id = UINT64_MAX;
struct Cursor {
    virtual ~Cursor() = default;
    virtual std::uint64_t value() const = 0;
    virtual void next() = 0;
};
using Stream = std::unique_ptr<Cursor>;
Stream intersect(Stream a, Stream b);
Stream unite(Stream a, Stream b);
Stream difference(Stream a, Stream b);
Stream universe(std::uint64_t count);
std::string json_quote(const std::string& value);
std::string file_sha256(const std::filesystem::path& path);

class Builder {
public:
    Builder(std::filesystem::path output, std::size_t memory_bytes,
            std::function<void()> check = {});
    ~Builder();
    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;
    DocId add_document(const std::string& metadata_json);
    void add(const std::string& term, DocId document);
    void finish(Metadata metadata);
    std::uint64_t segment_count() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Index {
public:
    explicit Index(std::filesystem::path directory);
    std::uint64_t document_count() const { return documents_; }
    std::uint64_t term_count() const { return terms_; }
    const Metadata& metadata() const { return metadata_; }
    Stream lookup(const std::string& term) const;
    std::uint64_t frequency(const std::string& term) const;
    std::string document(DocId id) const;
    void verify(const std::function<void()>& check = {}) const;
private:
    struct Posting { std::uint64_t offset = 0, count = 0; };
    Posting find(const std::string& term) const;
    std::filesystem::path directory_;
    std::uint64_t documents_ = 0, terms_ = 0, links_ = 0;
    Metadata metadata_, hashes_;
};
}
