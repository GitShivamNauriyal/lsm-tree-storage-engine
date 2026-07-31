#include "compaction.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <queue>
#include <stdexcept>
#include <tuple>

namespace fs = std::filesystem;

namespace lsm {

std::atomic<uint64_t> CompactionEngine::file_counter_{0};

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

CompactionEngine::CompactionEngine(const std::string& data_dir, size_t threshold,
                                   CompactionCallback callback)
    : data_dir_(data_dir),
      threshold_(threshold),
      callback_(std::move(callback)),
      shutdown_(false),
      compacting_(false) {

    fs::create_directories(data_dir_);

    compaction_thread_ = std::thread(&CompactionEngine::compactionLoop, this);
}

CompactionEngine::~CompactionEngine() {
    {
        std::lock_guard lock(mutex_);
        shutdown_ = true;
    }
    cv_.notify_one();

    if (compaction_thread_.joinable()) {
        compaction_thread_.join();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void CompactionEngine::notifyNewSSTable(const std::string& path) {
    {
        std::lock_guard lock(mutex_);
        sstable_paths_.push_back(path);
    }
    cv_.notify_one();
}

std::vector<std::string> CompactionEngine::getSSTables() const {
    std::lock_guard lock(mutex_);
    return sstable_paths_;
}

void CompactionEngine::setSSTables(const std::vector<std::string>& paths) {
    std::lock_guard lock(mutex_);
    sstable_paths_ = paths;
}

void CompactionEngine::waitForCompaction() {
    std::unique_lock lock(mutex_);
    uint64_t gen = compaction_gen_;
    // Wait until a compaction finishes (generation advances) or
    // SSTable count is below threshold and no compaction is running.
    done_cv_.wait(lock, [this, gen]() {
        return compaction_gen_ > gen ||
               (sstable_paths_.size() < threshold_ && !compacting_.load());
    });
}

// ---------------------------------------------------------------------------
// Background thread
// ---------------------------------------------------------------------------

void CompactionEngine::compactionLoop() {
    while (true) {
        std::vector<std::string> paths_to_compact;

        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this]() {
                return shutdown_.load() || sstable_paths_.size() >= threshold_;
            });

            if (shutdown_.load()) return;

            if (sstable_paths_.size() >= threshold_) {
                paths_to_compact = sstable_paths_;
                compacting_ = true;
            }
        }

        if (!paths_to_compact.empty()) {
            // Perform compaction outside the lock.
            std::string new_path = performCompaction(paths_to_compact);

            {
                std::lock_guard lock(mutex_);
                // Remove old paths and add the new one.
                sstable_paths_.clear();
                sstable_paths_.push_back(new_path);
                compacting_ = false;
            }

            // Delete old SSTable files.
            for (const auto& old_path : paths_to_compact) {
                fs::remove(old_path);
            }

            // Invoke callback if set.
            if (callback_) {
                callback_(new_path, paths_to_compact);
            }

            {
                std::lock_guard lock2(mutex_);
                compaction_gen_++;
            }
            done_cv_.notify_all();
        }
    }
}

// ---------------------------------------------------------------------------
// K-way merge compaction
// ---------------------------------------------------------------------------

std::string CompactionEngine::performCompaction(const std::vector<std::string>& input_paths) {
    // Open all input SSTables.
    std::vector<std::vector<std::pair<std::string, std::string>>> all_entries;
    all_entries.reserve(input_paths.size());

    for (const auto& path : input_paths) {
        SSTableReader reader(path);
        all_entries.push_back(reader.getAllEntries());
    }

    // K-way merge using a min-heap.
    // Each heap element: (key, value, file_index, entry_index_within_file)
    // Files are ordered newest-first (last in input_paths = newest).
    using HeapEntry = std::tuple<std::string, std::string, int, size_t>;

    // Min-heap: smallest key first. On ties, higher file_index (newer) wins.
    auto cmp = [](const HeapEntry& a, const HeapEntry& b) {
        if (std::get<0>(a) != std::get<0>(b))
            return std::get<0>(a) > std::get<0>(b); // Smallest key first.
        return std::get<2>(a) < std::get<2>(b);     // Newer file (higher index) first on tie.
    };

    std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)> heap(cmp);

    // Initialize heap with the first entry from each file.
    for (size_t i = 0; i < all_entries.size(); i++) {
        if (!all_entries[i].empty()) {
            heap.push({all_entries[i][0].first, all_entries[i][0].second,
                       static_cast<int>(i), 0});
        }
    }

    // Merge.
    std::vector<std::pair<std::string, std::string>> merged;
    std::string last_key;
    bool has_last_key = false;

    while (!heap.empty()) {
        auto [key, value, file_idx, entry_idx] = heap.top();
        heap.pop();

        // Push next entry from the same file.
        if (entry_idx + 1 < all_entries[file_idx].size()) {
            size_t next = entry_idx + 1;
            heap.push({all_entries[file_idx][next].first,
                       all_entries[file_idx][next].second,
                       file_idx, next});
        }

        // Deduplicate: skip if we already processed this key (the first
        // occurrence from the heap is the newest because of tie-breaking).
        if (has_last_key && key == last_key) {
            continue;
        }

        last_key = key;
        has_last_key = true;

        // Drop tombstones during compaction (they've served their purpose
        // once all older SSTables with the key are merged).
        if (MemTable::isTombstone(value)) {
            continue;
        }

        merged.emplace_back(std::move(key), std::move(value));
    }

    // Write the merged output to a new SSTable.
    std::string output_path = generateSSTablePath();
    SSTableWriter::write(output_path, merged);

    return output_path;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string CompactionEngine::generateSSTablePath() {
    auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    uint64_t counter = file_counter_.fetch_add(1);
    return data_dir_ + "/sstable_" + std::to_string(ts) + "_" +
           std::to_string(counter) + ".sst";
}

} // namespace lsm
