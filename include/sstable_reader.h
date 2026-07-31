#pragma once

#include "bloom_filter.h"
#include "sstable.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lsm {

/// Reads an SSTable file. Uses mmap() for the Bloom filter region to avoid
/// heap allocation and leverage the Linux page cache for lazy loading.
class SSTableReader {
public:
    /// Open an SSTable file for reading.
    explicit SSTableReader(const std::string& path);

    /// Unmap and close.
    ~SSTableReader();

    // Non-copyable, movable.
    SSTableReader(const SSTableReader&) = delete;
    SSTableReader& operator=(const SSTableReader&) = delete;
    SSTableReader(SSTableReader&& other) noexcept;
    SSTableReader& operator=(SSTableReader&& other) noexcept;

    /// Lookup a key. Returns the value if found, std::nullopt otherwise.
    /// Flow: Bloom filter check → binary search index → read data block.
    std::optional<std::string> get(const std::string& key) const;

    /// Get all entries in sorted order (for compaction merging).
    std::vector<std::pair<std::string, std::string>> getAllEntries() const;

    /// File path of this SSTable.
    const std::string& path() const { return path_; }

    /// Number of entries in this SSTable (from index).
    size_t numEntries() const { return index_.size(); }

private:
    std::string path_;
    int fd_;

    SSTableFooter footer_;
    std::vector<IndexEntry> index_;

    // mmap'd bloom filter.
    void*  bloom_mmap_addr_;
    size_t bloom_mmap_len_;
    std::unique_ptr<BloomFilter> bloom_;

    /// Read the footer from the end of the file.
    void readFooter();

    /// Read and parse the index block.
    void readIndex();

    /// mmap the bloom filter region.
    void mmapBloom();

    /// Read the value at a given data-block byte offset.
    std::pair<std::string, std::string> readDataEntry(uint64_t offset) const;
};

} // namespace lsm
