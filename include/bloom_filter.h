#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lsm {

/// Custom probabilistic Bloom filter using double-hashing (two independent
/// hashes combined to simulate k hash functions).
///
/// Hash scheme: h_i(x) = h1(x) + i * h2(x)  for i in [0, k)
/// where h1 = MurmurHash3-like finalizer and h2 = FNV-1a.
///
/// Provides:
///   - Zero false negatives (if key was added, membership test always returns true).
///   - Configurable false-positive rate (~1% default at optimal sizing).
///
/// Serialization: raw bit-vector + metadata for embedding in SSTable footer.
class BloomFilter {
public:
    /// Construct a Bloom filter sized for `expected_elements` with target
    /// false-positive rate `fp_rate`.
    BloomFilter(size_t expected_elements, double fp_rate = 0.01);

    /// Construct from a serialized byte buffer (deserialization).
    BloomFilter(const uint8_t* data, size_t data_size, size_t num_hashes, size_t num_bits);

    /// Add a key to the filter.
    void add(const std::string& key);

    /// Test membership. Returns true if the key *might* exist, false if
    /// the key *definitely* does not exist.
    bool mayContain(const std::string& key) const;

    /// Serialize the bit-vector to a byte buffer for writing to disk.
    std::vector<uint8_t> serialize() const;

    /// Number of hash functions used.
    size_t numHashes() const { return num_hashes_; }

    /// Size of the bit-vector in bits.
    size_t numBits() const { return num_bits_; }

    /// Size of the serialized data in bytes.
    size_t serializedSize() const { return bits_.size(); }

private:
    std::vector<uint8_t> bits_;  // Bit-vector, packed into bytes.
    size_t num_bits_;
    size_t num_hashes_;

    /// MurmurHash3 32-bit finalizer (hash1).
    static uint32_t murmurHash3(const std::string& key, uint32_t seed);

    /// FNV-1a 32-bit (hash2).
    static uint32_t fnv1a(const std::string& key);

    /// Set bit at position `pos`.
    void setBit(size_t pos);

    /// Test bit at position `pos`.
    bool getBit(size_t pos) const;
};

} // namespace lsm
