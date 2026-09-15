#include "lingua/detector.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace lingua {
namespace {
using Mask = generated::Mask;
constexpr Mask all_languages{UINT64_MAX, (uint64_t(1) << (language_count - 64)) - 1};

bool any(Mask mask) {
    return mask.lo || mask.hi;
}

bool has(Mask mask, int language) {
    return language < 64 ? (mask.lo >> language) & 1
                         : (mask.hi >> (language - 64)) & 1;
}

void erase(Mask& mask, int language) {
    if (language < 64)
        mask.lo &= ~(uint64_t(1) << language);
    else
        mask.hi &= ~(uint64_t(1) << (language - 64));
}

void insert(Mask& mask, int language) {
    if (language < 64)
        mask.lo |= uint64_t(1) << language;
    else
        mask.hi |= uint64_t(1) << (language - 64);
}

int only(Mask mask) {
    if (std::popcount(mask.lo) + std::popcount(mask.hi) != 1)
        return -1;
    return mask.lo ? std::countr_zero(mask.lo) : 64 + std::countr_zero(mask.hi);
}

template<class F>
void each(Mask mask, F function) {
    while (mask.lo) {
        const int language = std::countr_zero(mask.lo);
        function(language);
        mask.lo &= mask.lo - 1;
    }
    while (mask.hi) {
        const int language = std::countr_zero(mask.hi) + 64;
        function(language);
        mask.hi &= mask.hi - 1;
    }
}

struct Property {
    uint8_t script = 255;
    uint8_t flags = 0;
};

struct Unicode {
    std::array<Property, 0x110000> properties;
    Unicode() {
        for (auto range : generated::ranges)
            for (uint32_t cp = range.start; cp <= range.end; ++cp)
                properties[cp] = {range.script, range.flags};
    }
};

const Unicode& unicode() {
    static const Unicode table;
    return table;
}

const generated::Rule* rule(uint32_t cp) {
    if (cp < 128)
        return nullptr;
    const auto end = std::end(generated::rules);
    const auto it = std::lower_bound(
        std::begin(generated::rules), end, cp,
        [](const auto& candidate, uint32_t value) { return candidate.cp < value; });
    return it != end && it->cp == cp ? it : nullptr;
}

template<size_t N>
std::pair<int, int> top_two(const std::array<size_t, N>& counts,
                            bool unknown_first = false) {
    int first = -1;
    int second = -1;
    for (int position = 0; position < int(N); ++position) {
        const int i = unknown_first ? (position + int(N) - 1) % int(N) : position;
        if (!counts[i])
            continue;
        if (first < 0 || counts[i] > counts[first]) {
            second = first;
            first = i;
        } else if (second < 0 || counts[i] > counts[second])
            second = i;
    }
    return {first, second};
}
} // namespace

Detector::Detector(std::shared_ptr<const Model> model) : model_(std::move(model)) {
    if (!model_)
        throw std::invalid_argument("Detector requires a model");
    (void)unicode();
}

void Detector::split_text_into_words(std::string_view text) {
    decoded_.clear();
    lower_.clear();
    words_.clear();
    for (size_t i = 0; i < text.size();) {
        const uint8_t first = uint8_t(text[i++]);
        uint32_t cp = first;
        unsigned rest = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            cp = first & 31;
            rest = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            cp = first & 15;
            rest = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            cp = first & 7;
            rest = 3;
        } else if (first >= 128)
            throw std::invalid_argument("Invalid UTF-8");
        const unsigned length = rest;
        for (unsigned j = 0; j < length; ++j) {
            if (i == text.size() || (uint8_t(text[i]) & 0xc0) != 0x80)
                throw std::invalid_argument("Invalid UTF-8");
            cp = (cp << 6) | (uint8_t(text[i++]) & 63);
        }
        const bool overlong = (length == 1 && cp < 128) || (length == 2 && cp < 2048) ||
                              (length == 3 && cp < 65536);
        if (overlong || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            throw std::invalid_argument("Invalid UTF-8");
        decoded_.push_back(cp);
    }

    const auto& props = unicode().properties;
    bool preceded_by_cased = false;
    for (size_t i = 0; i < decoded_.size(); ++i) {
        const uint32_t cp = decoded_[i];
        if (cp == 0x3a3) {
            // Greek sigma lowercases differently at the end of a word.
            size_t next = i + 1;
            while (next < decoded_.size() && (props[decoded_[next]].flags & 4))
                ++next;
            const bool final = preceded_by_cased &&
                               (next == decoded_.size() || !(props[decoded_[next]].flags & 2));
            lower_.push_back(final ? 0x3c2 : 0x3c3);
        } else if (cp < 128)
            lower_.push_back(cp >= 'A' && cp <= 'Z' ? cp + 32 : cp);
        else {
            const auto end = std::end(generated::lower);
            const auto it = std::lower_bound(
                std::begin(generated::lower), end, cp,
                [](const auto& candidate, uint32_t value) { return candidate.cp < value; });
            if (it == end || it->cp != cp)
                lower_.push_back(cp);
            else {
                lower_.push_back(it->a);
                if (it->b)
                    lower_.push_back(it->b);
                if (it->c)
                    lower_.push_back(it->c);
            }
        }
        if (!(props[cp].flags & 4))
            preceded_by_cased = (props[cp].flags & 2) != 0;
    }

    for (size_t i = 0; i < lower_.size();) {
        const size_t start = i;
        const auto property = props[lower_[i]];
        const int token = property.flags >> 4;
        // CJK ideographs and kana are individual tokens; alphabetic scripts form runs.
        if (token == 5 || token == 7 || token == 8)
            ++i;
        else if (token)
            while (i < lower_.size() && (props[lower_[i]].flags >> 4) == token)
                ++i;
        else if (property.flags & 1)
            while (i < lower_.size() && (props[lower_[i]].flags & 1))
                ++i;
        else {
            ++i;
            continue;
        }
        words_.push_back({start, i});
    }
}

std::array<double, language_count>
Detector::compute_language_confidence_values(std::string_view text, ConfidenceScale scale) {
    std::array<double, language_count> values{};
    split_text_into_words(text);
    if (words_.empty())
        return values;

    const auto& props = unicode().properties;
    std::array<size_t, language_count + 1> votes{};
    std::array<size_t, 18> scripts{};
    std::array<size_t, language_count> filter_counts{};
    size_t character_count = 0;
    constexpr int chinese = int(Language::Chinese);
    constexpr int japanese = int(Language::Japanese);

    // Cheap rules can decide distinctive text before the statistical model is consulted.
    for (auto word : words_) {
        std::array<size_t, language_count> counts{};
        int script = props[lower_[word.start]].script;
        special_.clear();
        for (size_t position = word.start; position < word.end; ++position) {
            const uint32_t cp = lower_[position];
            const int current_script = props[cp].script;
            if (current_script != script)
                script = 255;

            const int single = current_script < 18
                                   ? only(generated::script_languages[current_script])
                                   : -1;
            if (single >= 0)
                ++counts[single];
            else if (current_script == 9)
                ++counts[chinese];
            else if (current_script == 14 || current_script == 3 || current_script == 4)
                if (const auto character_rule = rule(cp))
                    each(character_rule->unique, [&](int language) { ++counts[language]; });
            if (cp >= 128)
                special_.push_back(cp);
        }
        character_count += word.end - word.start;
        if (script < 18)
            scripts[script] += word.end - word.start;

        // A characteristic character counts at most once per word.
        std::sort(special_.begin(), special_.end());
        special_.erase(std::unique(special_.begin(), special_.end()), special_.end());
        for (auto cp : special_)
            if (const auto character_rule = rule(cp))
                each(character_rule->filter,
                     [&](int language) { ++filter_counts[language]; });

        auto [first, second] = top_two(counts);
        if (first < 0)
            ++votes[language_count];
        else if (second < 0)
            ++votes[first];
        else if (counts[chinese] && counts[japanese])
            ++votes[japanese];
        else
            ++votes[counts[first] > counts[second] ? first : language_count];
    }

    if (votes[language_count] * 2 < words_.size())
        votes[language_count] = 0;
    // Rust's Option<Language> orders None before Some(language) on ties.
    auto [first_vote, second_vote] = top_two(votes, true);
    int detected = -1;
    if (first_vote >= 0 && second_vote < 0)
        detected = first_vote;
    else if ((first_vote == chinese && second_vote == japanese) ||
             (first_vote == japanese && second_vote == chinese))
        detected = japanese;
    else if (first_vote >= 0 && second_vote >= 0 && votes[first_vote] > votes[second_vote])
        detected = first_vote;

    if (detected >= 0 && detected < language_count) {
        values[detected] = 1;
        return values;
    }

    Mask candidates = all_languages;
    auto [dominant_script, second_script] = top_two(scripts);
    bool all_equal = true;
    if (dominant_script >= 0)
        for (auto count : scripts)
            if (count && count != scripts[dominant_script])
                all_equal = false;

    if (dominant_script >= 0 && (second_script < 0 || !all_equal)) {
        candidates = generated::script_languages[dominant_script];
        Mask subset{};
        each(candidates, [&](int language) {
            if (filter_counts[language] * 2 >= words_.size())
                insert(subset, language);
        });
        if (any(subset))
            candidates = subset;
    }

    if (const int single = only(candidates); single >= 0) {
        values[single] = 1;
        return values;
    }

    std::array<double, language_count> sums{};
    std::array<unsigned, language_count> unigrams{};
    const bool long_text = character_count >= 120;
    for (unsigned n = long_text ? 3 : 1; n <= (long_text ? 3u : 5u); ++n) {
        ngrams_.clear();
        for (auto word : words_)
            for (size_t start = word.start; start + n <= word.end; ++start) {
                detail::Key key;
                for (unsigned position = 0; position < n; ++position)
                    key.append(lower_[start + position], position);
                ngrams_.push_back(key);
            }

        // Repeated n-grams count once per order, matching Lingua's set semantics.
        std::sort(ngrams_.begin(), ngrams_.end());
        ngrams_.erase(std::unique(ngrams_.begin(), ngrams_.end()), ngrams_.end());
        std::array<double, language_count> order_sum{};
        for (auto key : ngrams_) {
            Mask remaining = candidates;
            for (unsigned length = n; length && any(remaining); --length) {
                if (const auto entry = model_->find(key)) {
                    for (uint32_t i = entry->offset; i < entry->offset + entry->count; ++i) {
                        const uint32_t posting = model_->postings_[i];
                        const int language = posting & 127;
                        if (has(remaining, language)) {
                            order_sum[language] += model_->probabilities_[posting >> 7];
                            // Once the longest available prefix matched, shorter ones must
                            // not contribute for this language.
                            erase(remaining, language);
                            if (n == 1)
                                ++unigrams[language];
                        }
                    }
                }
                key.shorten(length);
            }
        }
        each(candidates, [&](int language) {
            if (order_sum[language] < 0)
                sums[language] += order_sum[language];
        });
    }

    each(candidates, [&](int language) {
        if (unigrams[language])
            sums[language] /= unigrams[language];
    });

    if (scale == ConfidenceScale::Relative) {
        // Every scored language has a negative log score, so the best one is the greatest and
        // the ratio below lands in (0, 1] with the winner at exactly 1. A language no n-gram
        // matched keeps a sum of zero and stays out, exactly as it does on the other scale.
        int best = -1;
        each(candidates, [&](int language) {
            if (sums[language] != 0 && (best < 0 || sums[language] > sums[best]))
                best = language;
        });
        if (best >= 0)
            each(candidates, [&](int language) {
                if (sums[language] != 0)
                    values[language] = sums[best] / sums[language];
            });
        return values;
    }

    double denominator = 0;
    each(candidates, [&](int language) {
        if (sums[language] != 0) {
            values[language] = std::exp(sums[language]);
            denominator += values[language];
        }
    });
    if (denominator == 0) {
        // Preserve the original winner when every exponent underflows to zero.
        int best = -1;
        each(candidates, [&](int language) {
            if (sums[language] < 0 && (best < 0 || sums[language] > sums[best]))
                best = language;
        });
        if (best >= 0)
            values[best] = 1;
    } else
        for (double& value : values)
            value /= denominator;
    return values;
}

Language Detector::detect_language_of(std::string_view text) {
    const auto values = compute_language_confidence_values(text);
    int best = 0;
    int second = 1;
    if (values[second] > values[best])
        std::swap(best, second);
    for (int i = 2; i < language_count; ++i) {
        if (values[i] > values[best]) {
            second = best;
            best = i;
        } else if (values[i] > values[second])
            second = i;
    }
    if (std::abs(values[best] - values[second]) < std::numeric_limits<double>::epsilon())
        return Language::Unknown;
    return Language(best);
}
} // namespace lingua
