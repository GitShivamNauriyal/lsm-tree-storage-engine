#include "bloom_filter.h"

#include <cmath>
#include <cstring>

namespace lsm {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

BloomFilter::BloomFilter(size_t expected_elements, double fp_rate) {
    // Optimal number of bits: m = -(n * ln(p)) / (ln(2)^2)
    double ln2 = std::log(2.0);
    auto m = static_cast<size_t>(
        -static_cast<double>(expected_elements) * std::log(fp_rate) / (ln2 * ln2));
    if (m == 0) m = 1;
    num_bits_ = m;

    // Optimal number of hashes: k = (m / n) * ln(2)
    auto k = static_cast<size_t>(
        (static_cast<double>(num_bits_) / static_cast<double>(expected_elements)) * ln2);
    if (k == 0) k = 1;
    num_hashes_ = k;

    // Allocate byte-packed bit vector, zero-initialized.
    bits_.resize((num_bits_ + 7) / 8, 0);
}

BloomFilter::BloomFilter(const uint8_t* data, size_t data_size, size_t num_hashes, size_t num_bits)
    : bits_(data, data + data_size),
      num_bits_(num_bits),
      num_hashes_(num_hashes) {}

// ---------------------------------------------------------------------------
// Hash functions
// ---------------------------------------------------------------------------

/// MurmurHash3 32-bit finalizer applied to key bytes.
uint32_t BloomFilter::murmurHash3(const std::string& key, uint32_t seed) {
    uint32_t h = seed;
    const auto* data = reinterpret_cast<const uint8_t*>(key.data());
    size_t len = key.size();

    // Body: process 4-byte chunks.
    size_t nblocks = len / 4;
    for (size_t i = 0; i < nblocks; i++) {
        uint32_t k;
        std::memcpy(&k, data + i * 4, sizeof(k));

        k *= 0xcc9e2d51u;
        k = (k << 15) | (k >> 17);
        k *= 0x1b873593u;

        h ^= k;
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xe6546b64u;
    }

    // Tail: remaining bytes.
    const uint8_t* tail = data + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3) {
        case 3: k1 ^= static_cast<uint32_t>(tail[2]) << 16; [[fallthrough]];
        case 2: k1 ^= static_cast<uint32_t>(tail[1]) << 8;  [[fallthrough]];
        case 1: k1 ^= static_cast<uint32_t>(tail[0]);
                k1 *= 0xcc9e2d51u;
                k1 = (k1 << 15) | (k1 >> 17);
                k1 *= 0x1b873593u;
                h ^= k1;
    }

    // Finalization mix.
    h ^= static_cast<uint32_t>(len);
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;

    return h;
}

/// FNV-1a 32-bit hash.
uint32_t BloomFilter::fnv1a(const std::string& key) {
    uint32_t hash = 0x811c9dc5u; // FNV offset basis.
    for (char c : key) {
        hash ^= static_cast<uint32_t>(static_cast<uint8_t>(c));
        hash *= 0x01000193u; // FNV prime.
    }
    return hash;
}

// ---------------------------------------------------------------------------
// Bit manipulation
// ---------------------------------------------------------------------------

void BloomFilter::setBit(size_t pos) {
    bits_[pos / 8] |= (1u << (pos % 8));
}

bool BloomFilter::getBit(size_t pos) const {
    return (bits_[pos / 8] & (1u << (pos % 8))) != 0;
}

// ---------------------------------------------------------------------------
// Core operations
// ---------------------------------------------------------------------------

void BloomFilter::add(const std::string& key) {
    uint32_t h1 = murmurHash3(key, 0);
    uint32_t h2 = fnv1a(key);

    for (size_t i = 0; i < num_hashes_; i++) {
        size_t pos = (h1 + i * h2) % num_bits_;
        setBit(pos);
    }
}

bool BloomFilter::mayContain(const std::string& key) const {
    uint32_t h1 = murmurHash3(key, 0);
    uint32_t h2 = fnv1a(key);

    for (size_t i = 0; i < num_hashes_; i++) {
        size_t pos = (h1 + i * h2) % num_bits_;
        if (!getBit(pos)) {
            return false; // Definitely not present.
        }
    }
    return true; // Might be present.
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::vector<uint8_t> BloomFilter::serialize() const {
    return bits_;
}

} // namespace lsm
