#pragma once

#include "skip_list.h"

#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace lsm {

/// Thread-safe MemTable backed by a SkipList.
///
/// Concurrency model: std::shared_mutex (reader-writer lock).
///   - get()       → shared (read) lock — multiple concurrent readers allowed.
///   - put()/remove() → exclusive (write) lock.
///
/// Capacity: byte-counted. When memoryUsage() >= capacity threshold, the
/// MemTable is considered full and should be frozen + flushed to an SSTable.
///
/// Tombstones: A delete is stored as a key with an empty sentinel value
/// and is_tombstone=true, propagated through to SSTables for correct
/// compaction semantics.
class MemTable {
public:
    /// Tombstone sentinel value stored in the skip list.
    /// We use a special 1-byte marker that cannot collide with real values.
    static constexpr char TOMBSTONE_MARKER = '\xff';
    static const std::string TOMBSTONE_VALUE;

    /// Construct a MemTable with a given capacity threshold (bytes).
    explicit MemTable(size_t capacity_bytes = 4 * 1024 * 1024 /* 4 MB */);

    ~MemTable() = default;

    // Non-copyable.
    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    /// Insert or update a key-value pair.
    void put(const std::string& key, const std::string& value);

    /// Mark a key as deleted (inserts a tombstone entry).
    void remove(const std::string& key);

    /// Lookup a key. Returns:
    ///   - {value, false} if key found with a live value
    ///   - {"", true}     if key found as a tombstone (definitively deleted)
    ///   - std::nullopt   if key not found in this MemTable at all
    struct LookupResult {
        std::string value;
        bool is_tombstone;
    };
    std::optional<LookupResult> get(const std::string& key) const;

    /// True when the MemTable has reached or exceeded its capacity threshold.
    bool isFull() const;

    /// Number of entries (including tombstones).
    size_t size() const;

    /// Approximate memory usage in bytes.
    size_t memoryUsage() const;

    /// Capacity threshold in bytes.
    size_t capacity() const { return capacity_bytes_; }

    /// Collect all entries in sorted order for flush to SSTable.
    /// Each entry is {key, value} where tombstones have value == TOMBSTONE_VALUE.
    using Entry = std::pair<std::string, std::string>;
    std::vector<Entry> getEntries() const;

    /// Check if a value represents a tombstone.
    static bool isTombstone(const std::string& value) {
        return value == TOMBSTONE_VALUE;
    }

private:
    SkipList<std::string, std::string> skiplist_;
    size_t capacity_bytes_;
    mutable std::shared_mutex mutex_;
};

} // namespace lsm
