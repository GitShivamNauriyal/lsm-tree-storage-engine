#pragma once

#include "compaction.h"
#include "memtable.h"
#include "sstable.h"
#include "sstable_reader.h"
#include "wal.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lsm {

/// Integrated LSM-Tree Key-Value Storage Engine.
///
/// Write path:  Put(k,v) → WAL append → MemTable insert → flush to SSTable on capacity.
/// Delete path: Delete(k) → WAL appendTombstone → MemTable insert tombstone.
/// Read path:   Get(k) → MemTable → newest-to-oldest SSTables (bloom → index → data).
/// Recovery:    Replay WAL → reconstruct MemTable → scan data/ for existing SSTables.
class Engine {
public:
    struct Config {
        std::string data_dir = "data";
        size_t memtable_capacity = 4 * 1024 * 1024; // 4 MB
        size_t compaction_threshold = 4;
    };

    explicit Engine(const Config& config);
    ~Engine();

    // Non-copyable.
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Insert or update a key-value pair.
    void put(const std::string& key, const std::string& value);

    /// Delete a key.
    void remove(const std::string& key);

    /// Lookup a key. Returns the value if found, std::nullopt if not found or deleted.
    std::optional<std::string> get(const std::string& key);

private:
    Config config_;

    std::unique_ptr<WAL> wal_;
    std::unique_ptr<MemTable> memtable_;
    std::unique_ptr<CompactionEngine> compaction_;

    /// Ordered list of SSTable readers (newest first).
    std::vector<std::unique_ptr<SSTableReader>> sstable_readers_;
    mutable std::mutex sstable_mutex_;

    /// Flush the current MemTable to an SSTable and reset.
    void flushMemTable();

    /// Recover state on startup.
    void recover();

    /// Reload SSTable readers from the compaction engine's file list.
    void reloadSSTableReaders();

    /// Generate a unique SSTable filename.
    std::string generateSSTablePath();

    static std::atomic<uint64_t> flush_counter_;
};

} // namespace lsm
