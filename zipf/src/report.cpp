#include "analysis.hpp"
#include <cairo.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
namespace zipf {
namespace {
std::ofstream output(const std::filesystem::path& path) {
    std::ofstream stream;
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream.open(path);
    stream << std::setprecision(12);
    return stream;
}
std::string csv(std::string text) {
    std::string result = "\"";
    for (char c : text) { if (c == '"') result += '"'; result += c; }
    return result + '"';
}
std::string markdown(const std::string& text) {
    std::string result;
    for (char c : text) {
        if (c == '|' || c == '\\' || c == '`' || c == '*' || c == '_') result += '\\';
        result += (c == '\n' || c == '\r') ? ' ' : c;
    }
    return result;
}
}
void plot(const std::filesystem::path& path, const Distribution& d) {
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(
        cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1600, 1000), cairo_surface_destroy);
    std::unique_ptr<cairo_t, decltype(&cairo_destroy)> context(cairo_create(surface.get()), cairo_destroy);
    auto* cr = context.get();
    cairo_set_source_rgb(cr, 1, 1, 1); cairo_paint(cr);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    auto label = [&](double x, double y, const std::string& text, double size = 20) {
        cairo_set_source_rgb(cr, .12, .15, .20); cairo_set_font_size(cr, size);
        cairo_move_to(cr, x, y); cairo_show_text(cr, text.c_str());
    };
    const double left = 140, right = 1520, top = 160, bottom = 870;
    const double xmax = std::max(1.0, std::ceil(std::log10(d.terms.size())));
    const double ymin = std::floor(std::log10(std::min(1.0, d.constant / d.terms.size())));
    const double ymax = std::max(ymin + 1, std::ceil(std::log10(std::max(double(d.terms.front().count), d.constant))));
    auto x = [&](double rank) { return left + std::log10(rank) / xmax * (right - left); };
    auto y = [&](double count) { return bottom - (std::log10(count) - ymin) / (ymax - ymin) * (bottom - top); };
    cairo_set_line_width(cr, 1);
    for (int i = 0; i <= int(xmax); ++i) {
        double px = x(std::pow(10., i));
        cairo_set_source_rgb(cr, .87, .89, .92); cairo_move_to(cr, px, top); cairo_line_to(cr, px, bottom); cairo_stroke(cr);
        label(px - 15, bottom + 32, "10^" + std::to_string(i), 17);
    }
    for (int i = int(ymin); i <= int(ymax); ++i) {
        double py = y(std::pow(10., i));
        cairo_set_source_rgb(cr, .87, .89, .92); cairo_move_to(cr, left, py); cairo_line_to(cr, right, py); cairo_stroke(cr);
        label(65, py + 6, "10^" + std::to_string(i), 17);
    }
    label(140, 55, "Распределение лемм и закон Ципфа", 32);
    label(140, 98, "Основной текст; стоп-слова сохранены; обе оси логарифмические", 21);
    label(640, 960, "Ранг термина r (log10)", 23);
    cairo_save(cr); cairo_translate(cr, 30, 650); cairo_rotate(cr, -1.5707963267948966);
    label(0, 0, "Число вхождений f (log10)", 23); cairo_restore(cr);
    cairo_set_source_rgb(cr, .12, .40, .78); cairo_set_line_width(cr, 2);
    for (std::size_t i = 0; i < d.terms.size(); ++i) {
        if (!i) cairo_move_to(cr, x(i + 1), y(d.terms[i].count));
        else cairo_line_to(cr, x(i + 1), y(d.terms[i].count));
    }
    cairo_stroke(cr);
    for (std::size_t i = 0; i < d.terms.size(); ++i) {
        cairo_arc(cr, x(i + 1), y(d.terms[i].count), 1.8, 0, 6.283185307179586); cairo_fill(cr);
    }
    cairo_set_source_rgb(cr, .85, .25, .16); cairo_set_line_width(cr, 2.5);
    const double dash[] = {10, 6}; cairo_set_dash(cr, dash, 2, 0);
    cairo_move_to(cr, x(1), y(d.constant));
    cairo_line_to(cr, x(d.terms.size()), y(d.constant / d.terms.size())); cairo_stroke(cr);
    label(900, 120, "Синий: корпус; красный: N / (H_V × r)", 19);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) throw std::runtime_error(cairo_status_to_string(cairo_status(cr)));
    auto status = cairo_surface_write_to_png(surface.get(), path.string().c_str());
    if (status != CAIRO_STATUS_SUCCESS) throw std::runtime_error(cairo_status_to_string(status));
}
void report(const std::filesystem::path& dir, const Distribution& d, const std::string& metadata,
            const std::string& run, const std::string& tokens, const std::string& corpus, std::uint64_t records) {
    auto frequencies = output(dir / "frequencies.csv");
    frequencies << "rank,term,count,relative_frequency,zipf_count,ratio_to_zipf\n";
    for (std::size_t i = 0; i < d.terms.size(); ++i) {
        const auto& t = d.terms[i]; const double expected = d.constant / (i + 1);
        frequencies << i + 1 << ',' << csv(t.text) << ',' << t.count << ',' << double(t.count) / d.tokens
                    << ',' << expected << ',' << t.count / expected << '\n';
    }
    frequencies.close();
    auto json = output(dir / "statistics.json");
    json << "{\"input\":" << metadata << ",\"field\":\"text\",\"stop_words_removed\":false,\"records\":" << records
         << ",\"tokens\":" << d.tokens << ",\"vocabulary\":" << d.terms.size() << ",\"singletons\":" << d.singletons
         << ",\"zipf_constant\":" << d.constant << ",\"log10_rmse\":" << d.log_rmse << ",\"alpha\":";
    if (d.has_alpha) json << d.alpha; else json << "null";
    json << ",\"alpha_rank_range\":[1," << d.terms.size() << "]}\n"; json.close();
    auto md = output(dir / "report.md");
    md << "# Закон Ципфа\n\nЛемматизация: " << markdown(run) << ". Токенизация: " << markdown(tokens)
       << ". Корпус: " << markdown(corpus) << ".\n\n"
       << "Анализируется набор документов выбранного запуска, включая его ограничения по источнику и объёму; "
          "исходные метаданные сохранены в statistics.json. Читаются все части поля text, только status=ok. "
          "Каждое вхождение даёт одну основную лемму; альтернативы разбора не считаются. "
          "Заголовки исключены, стоп-слова, числа и иностранные слова сохранены.\n\n"
       << "![Распределение частот](zipf.png)\n\n"
       << "| Показатель | Значение |\n| --- | --- |\n"
       << "| Записи (части полей, не документы) | " << records << " |\n"
       << "| Вхождения N | " << d.tokens << " |\n| Словарь V | " << d.terms.size()
       << " |\n| Термины с одним вхождением | " << d.singletons << " (" << 100.0 * d.singletons / d.terms.size()
       << "%) |\n| C = N / H_V | " << d.constant << " |\n| RMSE в log10 | " << d.log_rmse << " |\n";
    if (d.has_alpha) md << "| Оценка α по всем рангам | " << d.alpha << " |\n";
    md << "\n## Методика\n\nРанги начинаются с 1, частоты отсортированы по убыванию, равные — по термину. "
          "Закон Ципфа: f(r)=C/r, H_V=Σ(1/r), C=N/H_V. Сумма теоретических частот равна N. "
          "На обеих осях log10, без сглаживания и отсечения хвоста. "
          "RMSE — корень среднего квадрата разности log10 фактической и теоретической частот. "
          "Оценка α получена обычной линейной регрессией log10(f)=a−α·log10(r) по всем рангам 1…V; "
          "это описательная оценка, чувствительная к хвосту, а не доказательство степенного закона. "
          "При V=1 оценка α не определена.\n\n## Наблюдения и причины расхождений\n\n"
       << "Частота первого термина / прогноз Ципфа: " << d.terms.front().count / d.constant
       << ". Доля единичных терминов: " << 100.0 * d.singletons / d.terms.size() << "%. "
          "В хвосте эмпирические частоты целочисленные, тогда как C/r меняется непрерывно и может быть меньше 1. "
          "Поэтому конечный корпус образует ступени и не обязан точно совпадать с кривой.\n\n"
          "Возможные дополнительные причины: игровая тематика и повторяемость названий, смешение языков, "
          "неравные доли источников, повторяющиеся тексты и остатки HTML-шаблонов. "
          "Эти причины являются гипотезами: частотный график сам по себе их не подтверждает. "
          "Лемматизация объединяет словоформы; текущий анализатор не снимает омонимию по контексту "
          "и выбирает лексикографически первую словарную лемму. Неизвестные слова и неподдерживаемые токены сохраняются. "
          "Это также может менять форму распределения.\n\n## Наиболее частые термины\n\n"
          "| Ранг | Термин | Частота | Ципф |\n| --- | --- | --- | --- |\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(20, d.terms.size()); ++i)
        md << "| " << i + 1 << " | " << markdown(d.terms[i].text) << " | " << d.terms[i].count
           << " | " << d.constant / (i + 1) << " |\n";
    md << "\nПолное распределение: [frequencies.csv](frequencies.csv).\n"; md.close();
}
}
