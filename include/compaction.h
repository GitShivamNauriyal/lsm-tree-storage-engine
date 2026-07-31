#pragma once

#include "memtable.h"
#include "sstable.h"
#include "sstable_reader.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lsm {

/// Background compaction engine.
///
/// Strategy: Single-threshold merging.
/// When the number of SSTables reaches threshold N, the compaction thread
/// wakes via condition_variable, performs a K-way merge sort via min-heap,
/// deduplicates overlapping keys, drops tombstones/stale data, writes a
/// new SSTable, and atomically swaps/deletes old files.
///
/// The compaction thread is detached and does NOT block active reads/writes.
class CompactionEngine {
public:
    /// Callback invoked after compaction completes.
    /// Arguments: (new_sstable_path, list_of_old_sstable_paths_deleted)
    using CompactionCallback = std::function<void(const std::string&, const std::vector<std::string>&)>;

    /// Construct with the data directory, compaction threshold, and optional callback.
    CompactionEngine(const std::string& data_dir, size_t threshold,
                     CompactionCallback callback = nullptr);

    ~CompactionEngine();

    // Non-copyable.
    CompactionEngine(const CompactionEngine&) = delete;
    CompactionEngine& operator=(const CompactionEngine&) = delete;

    /// Notify the compaction engine that a new SSTable has been added.
    /// If the total count reaches the threshold, trigger compaction.
    void notifyNewSSTable(const std::string& path);

    /// Get the current list of SSTable paths (thread-safe).
    std::vector<std::string> getSSTables() const;

    /// Force-set the SSTable list (used during recovery).
    void setSSTables(const std::vector<std::string>& paths);

    /// Wait until the next compaction cycle completes (for testing).
    /// Blocks until a compaction that was triggered after this call finishes.
    void waitForCompaction();

    /// Check if compaction is currently running.
    bool isCompacting() const { return compacting_.load(); }

private:
    std::string data_dir_;
    size_t threshold_;
    CompactionCallback callback_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> sstable_paths_;

    std::thread compaction_thread_;
    std::atomic<bool> shutdown_;
    std::atomic<bool> compacting_;

    // Generation counter incremented after each compaction completes.
    uint64_t compaction_gen_{0};
    std::condition_variable done_cv_;

    /// Background thread loop.
    void compactionLoop();

    /// Perform the actual K-way merge compaction.
    /// Returns the path of the new merged SSTable.
    std::string performCompaction(const std::vector<std::string>& input_paths);

    /// Generate a unique SSTable filename.
    std::string generateSSTablePath();

    /// Atomic counter for unique filenames.
    static std::atomic<uint64_t> file_counter_;
};

} // namespace lsm
