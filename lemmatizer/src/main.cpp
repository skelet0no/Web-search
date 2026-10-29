#include "options.hpp"
#include "repository.hpp"
#include "report.hpp"
#include "logging.hpp"
#include <mongocxx/instance.hpp>
#include <csignal>
#include <iostream>
namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
}
int main(int argc, char** argv) {
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        auto options = lemmatizer::parse_options(argc, argv);
        if (options.help) {
            std::cout << "Usage: lemmatizer [--tokenization-run ID] [--run-id ID] [--dictionary DIR]\n"
                << "  [--uri URI] [--database crawler] [--source NAME] [--limit DOCUMENTS]\n"
                << "  [--batch-size 100] [--cache-size 20000] [--max-token-codepoints 128]\n"
                << "  [--output docx/runs]\n"
                << "Resume: lemmatizer --resume ID [--uri URI] [--database DB] [--dictionary DIR]\n";
            return 0;
        }
        if (!options.resume && std::filesystem::exists(options.output / options.run_id))
            throw std::invalid_argument("Output directory already exists; use another run-id");
        auto backend = lemmatizer::make_xmorphy(options.dictionary);
        mongocxx::instance instance;
        lemmatizer::Repository repository(options);
        repository.start();
        try {
            options = repository.options();
            lemmatizer::Analyzer analyzer(*backend, options.analyzer);
            repository.process(analyzer, [] { return stopped != 0; });
            if (stopped) throw std::runtime_error("Interrupted");
            repository.check_lease();
            auto metadata = repository.metadata();
            lemmatizer::write_report(options, repository.statistics(), metadata.view());
            repository.finish();
            std::cout << "Completed " << options.run_id << ": " << repository.statistics().tokens
                << " tokens, " << repository.statistics().errors << " errors; "
                << options.output / options.run_id / "report.md" << '\n';
            return repository.statistics().errors ? 2 : 0;
        } catch (const std::exception& e) {
            repository.fail(e.what());
            throw;
        }
    } catch (const std::exception& e) {
        lemmatizer::log_event("failed", "", e.what());
        return stopped ? 130 : 2;
    }
}
