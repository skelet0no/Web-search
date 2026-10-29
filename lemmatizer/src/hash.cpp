#include "lemmatizer/analyzer.hpp"
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <openssl/evp.h>
namespace lemmatizer {
namespace {
struct Digest {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    Digest() {
        if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("Cannot initialize SHA-256");
    }
    void add(const char* data, std::size_t size) {
        if (EVP_DigestUpdate(context.get(), data, size) != 1) throw std::runtime_error("Cannot hash input");
    }
    std::string finish() {
        unsigned char bytes[EVP_MAX_MD_SIZE];
        unsigned int length;
        if (EVP_DigestFinal_ex(context.get(), bytes, &length) != 1) throw std::runtime_error("Cannot finish hash");
        std::ostringstream out;
        for (unsigned int i = 0; i < length; ++i) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
        return out.str();
    }
};
}
std::string sha256(std::string_view value) {
    Digest digest;
    digest.add(value.data(), value.size());
    return digest.finish();
}
std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open dictionary: " + path.string());
    Digest digest;
    std::array<char, 65536> buffer;
    while (input) {
        input.read(buffer.data(), buffer.size());
        digest.add(buffer.data(), static_cast<std::size_t>(input.gcount()));
    }
    if (!input.eof()) throw std::runtime_error("Cannot read dictionary: " + path.string());
    return digest.finish();
}
}
