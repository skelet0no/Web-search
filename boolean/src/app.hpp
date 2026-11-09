#pragma once
#include "boolean/index.hpp"
#include <charconv>
#include <csignal>
#include <stdexcept>
#include <string>

namespace boolean_search::app {
inline volatile std::sig_atomic_t stopped=0;
inline void stop(int) { stopped=1; }
inline void check() { if (stopped) throw std::runtime_error("Interrupted"); }
inline void signals() { std::signal(SIGINT,stop); std::signal(SIGTERM,stop); }
inline std::uint64_t number(const std::string& text,bool zero=false) {
    std::uint64_t n=0;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),n);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || (!zero && !n))
        throw std::invalid_argument("Expected "+std::string(zero ? "nonnegative" : "positive")+" integer: "+text);
    return n;
}
inline const std::string& required(const Metadata& m,const std::string& key) {
    const auto it=m.find(key);
    if (it==m.end() || it->second.empty()) throw std::runtime_error("Missing index metadata: "+key);
    return it->second;
}
}
