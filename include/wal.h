#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lsm {

// Tombstone sentinel: MSB of key_size is hijacked.
// MSB = 1 → deleted record (tombstone); MSB = 0 → live record.
constexpr uint32_t TOMBSTONE_MASK = 0x80000000u;

struct WALRecord {
    std::string key;
    std::string value;
    bool is_tombstone;
};

/// Append-only Write-Ahead Log with length-prefixed binary records.
///
/// Binary format per record:
///   [KeySize (uint32_t)][Key bytes][ValueSize (uint32_t)][Value bytes]
///
/// Tombstones set MSB of KeySize to 1 and carry an empty value.
/// Recovery replays from byte 0 to EOF, discarding truncated tail records.
class WAL {
public:
    /// Opens (or creates) the WAL file at the given path.
    explicit WAL(const std::string& path);

    /// Closes the file descriptor.
    ~WAL();

    // Non-copyable, non-movable.
    WAL(const WAL&) = delete;
    WAL& operator=(const WAL&) = delete;

    /// Appends a live PUT record. fsync'd for durability.
    void append(const std::string& key, const std::string& value);

    /// Appends a tombstone DELETE record (empty value, MSB set). fsync'd.
    void appendTombstone(const std::string& key);

    /// Replays the entire WAL from byte 0, returning all recoverable records.
    /// Truncated/corrupt tail records are silently discarded.
    std::vector<WALRecord> recover();

    /// Truncates the WAL to zero length (called after successful MemTable flush).
    void clear();

private:
    std::string path_;
    int fd_;

    /// Writes raw bytes and fsyncs.
    void writeRecord(const std::string& key, const std::string& value, bool tombstone);
};

} // namespace lsm
