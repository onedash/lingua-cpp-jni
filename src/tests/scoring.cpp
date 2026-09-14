#include <lingua/detector.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

// Tiny independent model: verify zero-valued hits, prefix backoff, distinct
// n-grams and unigram normalization without depending on real-language labels.
int main() {
    namespace fs = std::filesystem;
    using namespace lingua;
    const auto dir = fs::temp_directory_path() /
                     ("lingua-test-" + std::to_string(
                                           std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count()));
    if (!fs::create_directory(dir))
        throw std::runtime_error("Cannot create test directory");
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            std::error_code error;
            fs::remove(dir / "raw", error);
            fs::remove(dir / "model", error);
            fs::remove(dir, error);
        }
    } cleanup{dir};
    std::ofstream raw(dir / "raw", std::ios::binary);
    auto add = [&](std::string_view text, Language language, double value) {
        struct Record {
            detail::Key key;
            double value;
            uint64_t language;
        } record{{}, value, uint64_t(language)};
        static_assert(sizeof(Record) == 32);
        for (unsigned i = 0; i < text.size(); ++i)
            record.key.append(uint8_t(text[i]), i);
        raw.write(reinterpret_cast<const char*>(&record), sizeof(record));
    };
    add("a", Language::English, -1);
    add("b", Language::English, -2);
    add("c", Language::English, -3);
    add("ab", Language::English, 0);
    add("abc", Language::English, 0);
    add("a", Language::German, -2);
    add("b", Language::German, -2);
    add("c", Language::German, -2);
    add("ab", Language::German, -0.2);
    add("bc", Language::German, -0.3);
    raw.close();
    Model::compile((dir / "raw").string(), (dir / "model").string());
    Detector detector(Model::load((dir / "model").string()));
    const auto result = detector.compute_language_confidence_values("abc");
    const double english = std::exp(-8.0 / 3);
    const double german = std::exp(-6.7 / 3);
    if (std::abs(result[int(Language::English)] - english / (english + german)) > 1e-14)
        throw std::runtime_error("Wrong scoring semantics");
    if (result != detector.compute_language_confidence_values("abc abc"))
        throw std::runtime_error("N-grams must be distinct");
    if (detector.detect_language_of("qzx") != Language::Unknown)
        throw std::runtime_error("Missing n-grams must stay unknown");
    // Corrupt lengths must be rejected before allocation or posting access.
    std::ofstream truncated(dir / "model", std::ios::binary | std::ios::trunc);
    truncated << "bad";
    truncated.close();
    bool rejected = false;
    try {
        Model::load((dir / "model").string());
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("Truncated model accepted");
}
