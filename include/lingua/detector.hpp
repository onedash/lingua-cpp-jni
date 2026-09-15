#pragma once
// Generated language IDs, names, Unicode properties, and language-specific rules.
#include "metadata.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lingua {
namespace detail {
struct Key {
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool operator==(const Key&) const = default;
    bool operator<(const Key& other) const {
        return hi != other.hi ? hi < other.hi : lo < other.lo;
    }
    void append(uint32_t cp, unsigned position) {
        if (position < 3)
            lo |= uint64_t(cp + 1) << (21 * position);
        else
            hi |= uint64_t(cp + 1) << (21 * (position - 3));
    }
    void shorten(unsigned length) {
        if (length > 3)
            hi &= ~(uint64_t(0x1fffff) << (21 * (length - 4)));
        else
            lo &= ~(uint64_t(0x1fffff) << (21 * (length - 1)));
    }
};
struct Entry {
    uint64_t key = 0;
    uint32_t offset = 0;
    uint32_t count = 0;
};
uint64_t hash(Key key);
}

// Construct once at service startup and share across all worker detectors.
class Model final {
public:
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;
    static std::shared_ptr<const Model> load(const std::string& path);
    static void compile(const std::string& raw_path, const std::string& path);
    size_t memory_bytes() const;
private:
    Model() = default;
    friend class Detector;
    std::vector<detail::Entry> entries_;
    std::vector<uint32_t> buckets_, postings_;
    std::vector<uint16_t> character_codes_;
    std::vector<double> probabilities_;
    bool encode(detail::Key key, uint64_t& packed) const;
    const detail::Entry* find(detail::Key key) const;
};

// Scale of the values compute_language_confidence_values reports. Both scales rank the
// languages identically and agree on which ones are candidates at all; they differ only in
// how the distance between them is expressed.
enum class ConfidenceScale {
    // Exponentiated and normalized, so the values are probabilities summing to 1 over the
    // candidates and the winner's value is the model's confidence in it.
    Probability,
    // Lingua's original relative scale: best_log_score / language_log_score. Log scores are
    // negative, so the winner is always exactly 1.0 and the rest fall in (0, 1]. Callers
    // whose thresholds were tuned against Lingua's own confidence values need this scale.
    // It cannot be recovered from the probability scale afterwards, because normalizing
    // discards the absolute log-score offset the ratio depends on.
    Relative,
};

class Detector final {
public:
    explicit Detector(std::shared_ptr<const Model> model);
    // One active call per Detector; separate detectors may run concurrently.
    // Input must be valid UTF-8. Invalid input throws std::invalid_argument.
    Language detect_language_of(std::string_view text);
    std::array<double, language_count> compute_language_confidence_values(
        std::string_view text, ConfidenceScale scale = ConfidenceScale::Probability);
private:
    std::shared_ptr<const Model> model_;
    std::vector<uint32_t> decoded_;
    std::vector<uint32_t> lower_;
    std::vector<uint32_t> special_;
    struct Word {
        size_t start;
        size_t end;
    };
    std::vector<Word> words_;
    std::vector<detail::Key> ngrams_;
    void split_text_into_words(std::string_view text);
};
inline const char* name(Language language) {
    const int i = static_cast<int>(language);
    return i >= 0 && i < language_count ? language_names[i] : "None";
}
} // namespace lingua
