#include "report.hpp"
#include <bsoncxx/json.hpp>
#include <fstream>
#include <stdexcept>
namespace lemmatizer {
namespace {
template<class Writer> void write_atomic(const std::filesystem::path& path, Writer writer) {
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream stream;
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream.open(temporary);
    writer(stream);
    stream.close();
    std::filesystem::rename(temporary, path);
}
}
void write_report(const Options& options, const Statistics& s, bsoncxx::document::view metadata) {
    const auto directory = options.output / options.run_id;
    std::filesystem::create_directories(directory);
    write_atomic(directory / "statistics.json", [&](std::ostream& out) {
        out << bsoncxx::to_json(metadata, bsoncxx::ExtendedJsonMode::k_relaxed) << '\n';
    });
    write_atomic(directory / "report.md", [&](std::ostream& out) {
        out << "# Лемматизация корпуса\n\nЗапуск: " << options.run_id << ".\n\n"
            << "Политика: " << policy_version << "; C++17 / XMorphy " << engine_commit << ".\n\n"
            << "Русский словарный анализ. Другие языки, числа и неизвестные слова сохраняются. "
            << "Первая лемма выбирается лексикографически; омонимия по контексту не снимается.\n\n"
            << "| Показатель | Значение |\n| --- | --- |\n"
            << "| Входные записи (части полей, не документы) | " << s.records << " |\n"
            << "| Ошибки записей | " << s.errors << " |\n"
            << "| Вхождения токенов | " << s.tokens << " |\n"
            << "| Изменённая основная форма | " << s.changed << " |\n"
            << "| Несколько лемм | " << s.ambiguous << " |\n"
            << "| Неизвестные русские слова | " << s.unknown << " |\n"
            << "| Пропущено по политике | " << s.skipped << " |\n"
            << "| Входной BSON, байт | " << s.input_bson_bytes << " |\n"
            << "| Выходной BSON без автоматически добавленного _id, байт | " << s.output_bson_bytes << " |\n"
            << "| Попадания / промахи кеша | " << s.cache_hits << " / " << s.cache_misses << " |\n"
            << "| Время анализа, с | " << s.core_seconds << " |\n"
            << "| Время обработки с БД, с | " << s.wall_seconds << " |\n";
        if (s.wall_seconds > 0) out << "| Токенов/с с БД | " << s.tokens / s.wall_seconds << " |\n";
        out << "\nСостояние публикации определяется коллекцией lemmatization_runs. "
            << "Отчёт создаётся перед финализацией; потребители принимают только completed.\n";
    });
}
}
