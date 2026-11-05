#include "analysis.hpp"
#include <cairo.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int main() {
    const auto dir = std::filesystem::temp_directory_path() / ("zipf-test-" + std::to_string(getpid()));
    try {
        auto d = zipf::analyze({{"игра", 6}, {"a", 3}, {"b", 2}});
        require(d.tokens == 11 && d.terms.size() == 3, "Totals");
        require(std::abs(d.constant - 6) < 1e-10, "Harmonic normalization");
        require(d.log_rmse < 1e-10 && std::abs(d.alpha - 1) < 1e-10, "Exact Zipf distribution");
        double sum = 0;
        for (std::size_t i = 0; i < d.terms.size(); ++i) sum += d.constant / (i + 1);
        require(std::abs(sum - d.tokens) < 1e-10, "Theoretical mass");
        auto ties = zipf::analyze({{"b", 1}, {"a", 1}});
        require(ties.terms[0].text == "a" && ties.singletons == 2, "Ties and singletons");
        auto single = zipf::analyze({{"один", 1}});
        require(!single.has_alpha && single.log_rmse == 0, "One term");
        bool rejected = false;
        try { zipf::analyze({}); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Empty corpus accepted");
        rejected = false;
        try { zipf::analyze({{"a", 0}}); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Zero frequency accepted");
        std::filesystem::create_directory(dir);
        zipf::plot(dir / "zipf.png", d);
        auto* png = cairo_image_surface_create_from_png((dir / "zipf.png").c_str());
        const bool valid = cairo_surface_status(png) == CAIRO_STATUS_SUCCESS &&
            cairo_image_surface_get_width(png) == 1600 && cairo_image_surface_get_height(png) == 1000;
        cairo_surface_destroy(png);
        require(valid, "PNG decoding or dimensions");
        zipf::plot(dir / "single.png", single);
        zipf::report(dir, d, "{}", "run", "tokens", "corpus", 3);
        require(std::filesystem::file_size(dir / "report.md") > 100, "Report missing");
        std::ifstream csv(dir / "frequencies.csv"); std::string line;
        std::getline(csv, line); std::getline(csv, line);
        require(line.find("1,\"игра\",6,") == 0, "CSV ordering/encoding");
        std::filesystem::remove_all(dir);
        std::cout << "Zipf tests passed\n";
    } catch (const std::exception& e) {
        std::error_code ignored; std::filesystem::remove_all(dir, ignored);
        std::cerr << e.what() << '\n'; return 1;
    }
}
