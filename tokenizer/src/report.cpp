#include "repository.hpp"
#include "tokenizer/tokenizer.hpp"
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace tokenizer {
namespace {
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::document;
void ratio(document& doc, const std::string& name, double numerator, double denominator) {
    if (denominator > 0) doc.append(kvp(name, numerator / denominator));
    else doc.append(kvp(name, bsoncxx::types::b_null{}));
}
bsoncxx::document::value metrics(const Metrics& m) {
    document doc;
    doc.append(kvp("fields", m.fields), kvp("input_utf8_bytes", m.bytes),
        kvp("tokens", m.tokens), kvp("token_codepoints", m.codepoints), kvp("core_seconds", m.seconds));
    ratio(doc, "average_token_length", m.codepoints, m.tokens);
    ratio(doc, "core_ms_per_kib", 1000 * m.seconds, m.bytes / 1024.0);
    ratio(doc, "core_kib_per_second", m.bytes / 1024.0, m.seconds);
    return doc.extract();
}
std::string divide(double n, double d) {
    if (d <= 0) return "—";
    std::ostringstream text;
    text << std::fixed << std::setprecision(6) << n / d;
    return text.str();
}
}
bsoncxx::document::value statistics_document(const RunResult& result) {
    const auto& s = result.statistics;
    document doc;
    doc.append(kvp("selected_documents", s.selected), kvp("successful_documents", s.successful),
        kvp("error_documents", s.errors), kvp("total", metrics(s.total)),
        kvp("wall_seconds", result.wall_seconds));
    ratio(doc, "wall_ms_per_kib", 1000 * result.wall_seconds, s.total.bytes / 1024.0);
    ratio(doc, "wall_kib_per_second", s.total.bytes / 1024.0, result.wall_seconds);
    return doc.extract();
}
void write_report(const Options& options, const RunResult& result) {
    auto directory = options.output / options.run_id;
    std::ofstream json(directory / "statistics.json");
    std::ofstream report(directory / "report.md");
    json.exceptions(std::ios::badbit | std::ios::failbit);
    report.exceptions(std::ios::badbit | std::ios::failbit);
    const auto stats = statistics_document(result);
    auto metadata = make_document(kvp("tokenization_run_id", options.run_id),
        kvp("corpus_run_id", options.corpus_run), kvp("rules_version", rules_version),
        kvp("icu_version", icu_version()), kvp("unicode_version", unicode_version()),
        kvp("compiler", __VERSION__), kvp("build_type", TOKENIZER_BUILD_TYPE), kvp("statistics", stats.view()));
    json << bsoncxx::to_json(metadata.view(), bsoncxx::ExtendedJsonMode::k_relaxed) << '\n';
    const auto& m = result.statistics.total;
    report << "# Токенизация корпуса\n\nЗапуск: " << options.run_id
        << ". Правила: " << rules_version << "; ICU " << icu_version() << ".\n\n"
        << "Документов: " << result.statistics.selected << "; успешно: " << result.statistics.successful
        << "; ошибок: " << result.statistics.errors << ".\n\n"
        << "| Показатель | Значение |\n| --- | --- |\n"
        << "| Токены (все вхождения) | " << m.tokens << " |\n"
        << "| Средняя длина, кодовые точки Unicode | " << divide(m.codepoints, m.tokens) << " |\n"
        << "| Вход, байты UTF-8 | " << m.bytes << " |\n"
        << "| Время ядра, с | " << m.seconds << " |\n"
        << "| Время прохода с БД, с | " << result.wall_seconds << " |\n"
        << "| Ядро, мс/КиБ | " << divide(1000 * m.seconds, m.bytes / 1024.0) << " |\n"
        << "| Проход с БД, мс/КиБ | " << divide(1000 * result.wall_seconds, m.bytes / 1024.0) << " |\n";
    json.close(); report.close();
}
}
