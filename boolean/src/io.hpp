#pragma once
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>

namespace boolean_search::io {
inline constexpr std::uint64_t max_string = 4 * 1024 * 1024;
inline std::ifstream input(const std::filesystem::path& p) {
    std::ifstream s(p, std::ios::binary);
    if (!s) throw std::runtime_error("Cannot open " + p.string());
    s.exceptions(std::ios::badbit);
    return s;
}
inline std::ofstream output(const std::filesystem::path& p) {
    std::ofstream s;
    s.exceptions(std::ios::badbit | std::ios::failbit);
    s.open(p, std::ios::binary | std::ios::trunc);
    return s;
}
inline void put(std::ostream& s, std::uint64_t n, unsigned width = 8) {
    for (unsigned i = 0; i < width; ++i) s.put(static_cast<char>((n >> (8 * i)) & 255));
    if (!s) throw std::runtime_error("Index write failed");
}
inline std::uint64_t get(std::istream& s, unsigned width = 8) {
    std::uint64_t n = 0;
    for (unsigned i = 0; i < width; ++i) {
        const auto c = s.get();
        if (c == std::char_traits<char>::eof()) throw std::runtime_error("Truncated index file");
        n |= std::uint64_t(static_cast<unsigned char>(c)) << (8 * i);
    }
    return n;
}


inline void put_varint(std::ostream& s, std::uint64_t n) {
    do {
        const auto payload = static_cast<unsigned char>(n & 127);
        n >>= 7;
        s.put(static_cast<char>(payload | (n ? 128 : 0)));
    } while (n);
    if (!s) throw std::runtime_error("Index write failed");
}
inline std::uint64_t get_varint(std::istream& s, std::uint64_t& bytes) {
    std::uint64_t n = 0;
    for (unsigned i = 0; i < 10; ++i) {
        if (!bytes) throw std::runtime_error("Truncated posting varint");
        --bytes;
        const auto c = s.get();
        if (c == std::char_traits<char>::eof()) throw std::runtime_error("Truncated posting varint");
        const auto b = static_cast<unsigned char>(c);
        if (i == 9 && b > 1) throw std::runtime_error("Posting varint overflow");
        n |= std::uint64_t(b & 127) << (7 * i);
        if (!(b & 128)) {
            if (i && !(b & 127)) throw std::runtime_error("Noncanonical posting varint");
            return n;
        }
    }
    throw std::runtime_error("Posting varint overflow");
}
inline void put_double(std::ostream& s, double value) {
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559,
                  "Index requires IEEE 754 binary64");
    if (!std::isfinite(value) || value < 0) throw std::runtime_error("Invalid document norm");
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    put(s, bits);
}
inline double get_double(std::istream& s) {
    const auto bits = get(s);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value) || value < 0) throw std::runtime_error("Invalid document norm");
    return value;
}
inline void put_string(std::ostream& s, const std::string& v) {
    if (v.size() > max_string) throw std::runtime_error("Index string exceeds 4 MiB");
    put(s, v.size(), 4); s.write(v.data(), static_cast<std::streamsize>(v.size()));
    if (!s) throw std::runtime_error("Index write failed");
}
inline std::string get_string(std::istream& s) {
    const auto n = get(s, 4);
    if (n > max_string) throw std::runtime_error("Invalid index string length");
    std::string v(n, '\0');
    if (!s.read(v.data(), static_cast<std::streamsize>(n))) throw std::runtime_error("Truncated index string");
    return v;
}
inline void seek(std::istream& s, std::uint64_t offset) {
    s.clear(); s.seekg(static_cast<std::streamoff>(offset));
    if (!s) throw std::runtime_error("Invalid index offset");
}
inline std::uint64_t position(std::ostream& s) {
    const auto p = s.tellp();
    if (p < 0) throw std::runtime_error("Cannot obtain index offset");
    return static_cast<std::uint64_t>(p);
}
inline void eof(std::istream& s) {
    if (s.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Unexpected trailing index data");
}
}
