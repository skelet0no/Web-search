#include "lemmatizer/analyzer.hpp"
#include <fstream>
#include <stdexcept>
#include <xmorphy/morph/DictMorphAnalyzer.h>
namespace lemmatizer {
namespace {
class XMorphyBackend final : public Backend {
public:
    explicit XMorphyBackend(const std::filesystem::path& directory) {
        const auto main = directory / "udmaindict.bin", affix = directory / "udaffixdict.bin";
        // Upstream's binary loader trusts its input. Validate bytes before loading.
        if (sha256_file(main) != main_dictionary_sha256 || sha256_file(affix) != affix_dictionary_sha256)
            throw std::invalid_argument("Dictionary checksum mismatch: use the pinned XMorphy dictionary");
        std::ifstream main_stream(main, std::ios::binary), affix_stream(affix, std::ios::binary);
        engine_ = std::make_unique<X::DictMorphAnalyzer>(main_stream, affix_stream);
        if (!main_stream || !affix_stream) throw std::runtime_error("Cannot load dictionary");
    }
    std::vector<Analysis> analyze(std::string_view normalized) override {
        // The supplied dictionary stores uppercase keys. Normalized input remains
        // untouched; case conversion only affects the engine's lookup key.
        X::UniString word{std::string(normalized)};
        word.toUpperCaseInPlace();
        // This dictionary folds Ё into Е. Apply the same lookup convention only
        // inside the adapter; original normalized tokens and offsets are retained.
        word = word.replace(u'Ё', u'Е');
        std::vector<Analysis> result;
        for (const auto& item : engine_->analyze(word))
            if (item->at == X::AnalyzerTag::DICT)
                result.push_back({item->normalform.getRawString(), true, std::nullopt});
        return result;
    }
private:
    std::unique_ptr<X::DictMorphAnalyzer> engine_;
};
}
std::unique_ptr<Backend> make_xmorphy(const std::filesystem::path& dictionary) {
    return std::make_unique<XMorphyBackend>(dictionary);
}
}
