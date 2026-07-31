#pragma once

#include "bloom_filter.h"
#include "memtable.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lsm {

/// SSTable file layout:
///
///   [Data Block]         Sorted key-value records:
///                          [KeySize(4B)][Key][ValueSize(4B)][Value] ...
///   [Index Block]        Key → byte-offset mapping:
///                          [KeySize(4B)][Key][Offset(8B)] ...
///   [Bloom Filter Block] Serialized bloom filter bit-vector
///   [Footer (28B)]       [index_offset(8B)][bloom_offset(8B)][bloom_num_hashes(4B)][bloom_num_bits(4B)][bloom_data_size(4B)]
///
/// SSTable files are immutable once written.

/// Footer stored at the end of the SSTable file.
struct SSTableFooter {
    uint64_t index_offset;        // Byte offset where the index block starts.
    uint64_t bloom_offset;        // Byte offset where the bloom filter data starts.
    uint32_t bloom_num_hashes;    // Number of hash functions used in the bloom filter.
    uint32_t bloom_num_bits;      // Number of bits in the bloom filter.
    uint32_t bloom_data_size;     // Size of the serialized bloom filter in bytes.

    static constexpr size_t SIZE = sizeof(uint64_t) * 2 + sizeof(uint32_t) * 3; // 28 bytes
};

/// Index entry mapping a key to its byte offset in the data block.
struct IndexEntry {
    std::string key;
    uint64_t offset;
};

/// Writes a new SSTable file from sorted entries (flushed from MemTable).
class SSTableWriter {
public:
    /// Write sorted entries to an SSTable file at `path`.
    /// Entries must be pre-sorted by key (as produced by MemTable::getEntries()).
    /// Returns the number of entries written.
    static size_t write(const std::string& path,
                        const std::vector<std::pair<std::string, std::string>>& entries);
};

} // namespace lsm
