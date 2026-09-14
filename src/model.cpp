#include "lingua/detector.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace lingua {
uint64_t detail::hash(Key k) {
    uint64_t x = k.lo ^ std::rotl(k.hi * 0x9e3779b97f4a7c15ULL, 27);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

namespace {
template <class T> void read(std::istream &in, T *data, size_t count) {
    if (!in.read(reinterpret_cast<char *>(data), sizeof(T) * count))
        throw std::runtime_error("Truncated model");
}

template <class T> void write(std::ostream &out, const T *data, size_t count) {
    if (!out.write(reinterpret_cast<const char *>(data), sizeof(T) * count))
        throw std::runtime_error("Cannot write model");
}

constexpr uint64_t magic = 0x324c444f4d474e4cULL;
constexpr size_t character_count = 0x110001; // Includes codepoint+1 encoding.
constexpr unsigned code_bits = 12;
constexpr unsigned common_count = (1 << code_bits) - 1;

uint32_t bucket_for(uint64_t key, size_t count) {
    return uint32_t(detail::hash({key, 0}) & (count - 1));
}

std::filesystem::path utf8_path(const std::string &value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
} // namespace

// Common characters take 12 bits; zero escapes to an exact 21-bit scalar.
// Oversized queries cannot be stored, but their prefixes are still searched.
bool Model::encode(detail::Key key, uint64_t &packed) const {
    unsigned shift = 0;
    packed = 0;
    for (unsigned i = 0; i < 5; ++i) {
        const uint32_t cp =
            uint32_t((i < 3 ? key.lo >> (21 * i) : key.hi >> (21 * (i - 3))) & 0x1fffff);
        if (!cp)
            break;
        if (cp >= character_codes_.size())
            return false;
        const uint16_t code = character_codes_[cp];
        const unsigned width = code ? code_bits : code_bits + 21;
        if (shift + width > 64)
            return false;
        packed |= (code ? uint64_t(code) : uint64_t(cp) << code_bits) << shift;
        shift += width;
    }
    return packed != 0;
}

void Model::compile(const std::string &raw_path, const std::string &path) {
    static_assert(std::endian::native == std::endian::little);
    struct Raw {
        detail::Key key;
        double value;
        uint64_t language;
    };
    std::ifstream in(utf8_path(raw_path), std::ios::binary | std::ios::ate);
    if (!in || in.tellg() <= 0 || uint64_t(in.tellg()) % sizeof(Raw))
        throw std::runtime_error("Invalid raw model");
    const size_t count = size_t(in.tellg()) / sizeof(Raw);
    if (count > UINT32_MAX)
        throw std::runtime_error("Model too large");
    std::vector<Raw> raw(count);
    in.seekg(0);
    read(in, raw.data(), count);
    Model model;
    std::vector<uint64_t> frequency(character_count), value_bits;
    value_bits.reserve(count);
    for (const auto &row : raw) {
        if (row.language >= language_count || !std::isfinite(row.value) || row.value > 0)
            throw std::runtime_error("Invalid probability");
        value_bits.push_back(std::bit_cast<uint64_t>(row.value));
        bool ended = false;
        for (unsigned i = 0; i < 5; ++i) {
            const uint32_t cp = uint32_t(
                (i < 3 ? row.key.lo >> (21 * i) : row.key.hi >> (21 * (i - 3))) & 0x1fffff);
            if (!cp) {
                ended = true;
                continue;
            }
            if (ended || cp >= character_count || (cp >= 0xd801 && cp <= 0xe000))
                throw std::runtime_error("Invalid Unicode key");
            ++frequency[cp];
        }
        if (!row.key.lo || row.key.lo >> 63 || row.key.hi >> 42)
            throw std::runtime_error("Invalid n-gram key");
    }
    std::vector<uint32_t> characters;
    for (uint32_t cp = 1; cp < character_count; ++cp)
        if (frequency[cp])
            characters.push_back(cp);
    std::sort(characters.begin(), characters.end(), [&](auto a, auto b) {
        return frequency[a] != frequency[b] ? frequency[a] > frequency[b] : a < b;
    });
    if (characters.size() > common_count)
        characters.resize(common_count);
    model.character_codes_.resize(character_count);
    for (size_t i = 0; i < characters.size(); ++i)
        model.character_codes_[characters[i]] = uint16_t(i + 1);

    // Deduplicate bit patterns, not rounded floating-point values.
    std::sort(value_bits.begin(), value_bits.end());
    value_bits.erase(std::unique(value_bits.begin(), value_bits.end()), value_bits.end());
    if (value_bits.size() > (uint64_t(1) << 25))
        throw std::runtime_error("Too many distinct probabilities");
    model.probabilities_.reserve(value_bits.size());
    for (auto bits : value_bits)
        model.probabilities_.push_back(std::bit_cast<double>(bits));
    for (auto &row : raw) {
        uint64_t packed;
        if (!model.encode(row.key, packed))
            throw std::runtime_error("Stored n-gram does not fit the compact encoding");
        row.key = {packed, 0};
        const auto id = std::lower_bound(value_bits.begin(), value_bits.end(),
                                         std::bit_cast<uint64_t>(row.value)) -
                        value_bits.begin();
        row.language = (uint64_t(id) << 7) | row.language;
    }
    std::sort(raw.begin(), raw.end(), [](const Raw &a, const Raw &b) {
        return a.key.lo != b.key.lo ? a.key.lo < b.key.lo : (a.language & 127) < (b.language & 127);
    });
    size_t unique = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!i || raw[i].key.lo != raw[i - 1].key.lo)
            ++unique;
        else if ((raw[i].language & 127) == (raw[i - 1].language & 127))
            throw std::runtime_error("Duplicate model entry");
    }
    // Dense buckets average about two entries: no empty key slots or probes.
    size_t buckets = 1;
    while (buckets < unique / 2)
        buckets *= 2;
    if (buckets > 1 && buckets > unique / 1.5)
        buckets /= 2;
    model.buckets_.resize(buckets + 1);
    for (size_t i = 0; i < count; ++i)
        if (!i || raw[i].key.lo != raw[i - 1].key.lo)
            ++model.buckets_[bucket_for(raw[i].key.lo, buckets) + 1];
    std::partial_sum(model.buckets_.begin(), model.buckets_.end(), model.buckets_.begin());
    auto cursor = model.buckets_;
    model.entries_.resize(unique);
    model.postings_.reserve(count);
    for (size_t i = 0; i < count;) {
        size_t end = i + 1;
        while (end < count && raw[end].key.lo == raw[i].key.lo)
            ++end;
        const auto bucket = bucket_for(raw[i].key.lo, buckets);
        model.entries_[cursor[bucket]++] = {raw[i].key.lo, uint32_t(i), uint32_t(end - i)};
        for (size_t j = i; j < end; ++j)
            model.postings_.push_back(uint32_t(raw[j].language));
        i = end;
    }
    std::ofstream out(utf8_path(path), std::ios::binary);
    const uint64_t header[] = {
        magic, buckets, unique, count, value_bits.size(), characters.size(), language_count};
    write(out, header, 7);
    write(out, characters.data(), characters.size());
    write(out, model.buckets_.data(), buckets + 1);
    write(out, model.entries_.data(), unique);
    write(out, model.postings_.data(), count);
    write(out, model.probabilities_.data(), value_bits.size());
}

std::shared_ptr<const Model> Model::load(const std::string &path) {
    static_assert(sizeof(detail::Entry) == 16 && sizeof(double) == 8 &&
                  std::numeric_limits<double>::is_iec559);
    static_assert(std::endian::native == std::endian::little);
    std::ifstream in(utf8_path(path), std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("Cannot open model: " + path);
    const uint64_t bytes = uint64_t(in.tellg());
    in.seekg(0);
    uint64_t header[7];
    read(in, header, 7);
    const auto &h = header;
    const uint64_t expected_bytes =
        56 + h[5] * 4 + (h[1] + 1) * 4 + h[2] * 16 + h[3] * 4 + h[4] * 8;
    if (h[0] != magic || h[6] != language_count || !std::has_single_bit(h[1]) ||
        h[1] > UINT32_MAX || !h[2] || h[2] > UINT32_MAX || h[3] > UINT32_MAX || !h[4] ||
        h[4] > (uint64_t(1) << 25) || h[5] > common_count || bytes != expected_bytes)
        throw std::runtime_error("Invalid compact model header; regenerate the model");

    auto model = std::shared_ptr<Model>(new Model);
    std::vector<uint32_t> characters(static_cast<size_t>(h[5]));
    read(in, characters.data(), characters.size());
    model->character_codes_.resize(character_count);
    for (size_t i = 0; i < characters.size(); ++i) {
        const auto cp = characters[i];
        if (!cp || cp >= character_count || (cp >= 0xd801 && cp <= 0xe000) ||
            model->character_codes_[cp])
            throw std::runtime_error("Invalid character dictionary");
        model->character_codes_[cp] = uint16_t(i + 1);
    }
    model->buckets_.resize(size_t(h[1] + 1));
    model->entries_.resize(size_t(h[2]));
    model->postings_.resize(size_t(h[3]));
    model->probabilities_.resize(size_t(h[4]));
    read(in, model->buckets_.data(), model->buckets_.size());
    read(in, model->entries_.data(), model->entries_.size());
    read(in, model->postings_.data(), model->postings_.size());
    read(in, model->probabilities_.data(), model->probabilities_.size());

    if (model->buckets_.front() != 0 || model->buckets_.back() != h[2] ||
        !std::is_sorted(model->buckets_.begin(), model->buckets_.end()))
        throw std::runtime_error("Invalid bucket directory");
    for (size_t bucket = 0; bucket < h[1]; ++bucket) {
        uint64_t previous = 0;
        for (uint32_t i = model->buckets_[bucket]; i < model->buckets_[bucket + 1]; ++i) {
            const auto &entry = model->entries_[i];
            if (entry.key <= previous || bucket_for(entry.key, size_t(h[1])) != bucket ||
                !entry.count || entry.count > language_count ||
                uint64_t(entry.offset) + entry.count > h[3])
                throw std::runtime_error("Invalid compact entry");
            previous = entry.key;
        }
    }
    for (auto posting : model->postings_)
        if ((posting & 127) >= language_count || (posting >> 7) >= h[4])
            throw std::runtime_error("Invalid posting");
    for (double probability : model->probabilities_)
        if (!std::isfinite(probability) || probability > 0)
            throw std::runtime_error("Invalid probability");
    return model;
}

const detail::Entry *Model::find(detail::Key key) const {
    uint64_t packed;
    if (!encode(key, packed))
        return nullptr;
    const auto bucket = bucket_for(packed, buckets_.size() - 1);
    for (uint32_t i = buckets_[bucket]; i < buckets_[bucket + 1]; ++i)
        if (entries_[i].key == packed)
            return &entries_[i];
    return nullptr;
}

size_t Model::memory_bytes() const {
    return entries_.capacity() * sizeof(detail::Entry) +
           (buckets_.capacity() + postings_.capacity()) * 4 + probabilities_.capacity() * 8 +
           character_codes_.capacity() * 2;
}
} // namespace lingua
