#include "analysis.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace zipf {
Distribution analyze(const std::unordered_map<std::string, std::uint64_t>& counts) {
    Distribution d;
    for (const auto& [text, count] : counts) {
        if (text.empty() || !count) throw std::invalid_argument("Empty term or zero frequency");
        if (count > std::numeric_limits<std::uint64_t>::max() - d.tokens)
            throw std::overflow_error("Token count overflow");
        d.tokens += count;
        d.singletons += count == 1;
        d.terms.push_back({text, count});
    }
    if (d.terms.empty()) throw std::invalid_argument("No lemmas in text fields of selected run");
    std::sort(d.terms.begin(), d.terms.end(), [](const Term& a, const Term& b) {
        return a.count != b.count ? a.count > b.count : a.text < b.text;
    });
    double harmonic = 0;
    for (std::size_t i = 1; i <= d.terms.size(); ++i) harmonic += 1.0 / i;
    d.constant = d.tokens / harmonic;
    double mx = 0, my = 0, xx = 0, xy = 0, error = 0;
    for (std::size_t i = 0; i < d.terms.size(); ++i) {
        const double x = std::log10(i + 1.0), y = std::log10(d.terms[i].count);
        const double dx = x - mx, dy = y - my;
        mx += dx / (i + 1.0); my += dy / (i + 1.0);
        xx += dx * (x - mx); xy += dx * (y - my);
        const double residual = y - (std::log10(d.constant) - x);
        error += residual * residual;
    }
    d.log_rmse = std::sqrt(error / d.terms.size());
    d.has_alpha = xx > 0;
    if (d.has_alpha) d.alpha = -xy / xx;
    return d;
}
}
