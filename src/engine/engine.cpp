#include "engine.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace lsm {

std::atomic<uint64_t> Engine::flush_counter_{0};

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

Engine::Engine(const Config& config)
    : config_(config) {

    fs::create_directories(config_.data_dir);

    // Initialize MemTable
    memtable_ = std::make_unique<MemTable>(config_.memtable_capacity);

    // Initialize CompactionEngine
    compaction_ = std::make_unique<CompactionEngine>(
        config_.data_dir, config_.compaction_threshold,
        [this](const std::string& /*new_path*/, const std::vector<std::string>& /*old_paths*/) {
            this->reloadSSTableReaders();
        });

    // Recover state from disk
    recover();
}

Engine::~Engine() {
    // Flush any remaining data in MemTable to disk
    if (memtable_->size() > 0) {
        flushMemTable();
    }
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

void Engine::recover() {
    std::string wal_path = config_.data_dir + "/wal.log";

    // 1. Recover MemTable from WAL
    wal_ = std::make_unique<WAL>(wal_path);
    auto records = wal_->recover();

    for (const auto& record : records) {
        if (record.is_tombstone) {
            memtable_->remove(record.key);
        } else {
            memtable_->put(record.key, record.value);
        }
    }

    // 2. Discover existing SSTables in data directory
    std::vector<std::string> sstable_paths;
    for (const auto& entry : fs::directory_iterator(config_.data_dir)) {
        if (entry.path().extension() == ".sst") {
            sstable_paths.push_back(entry.path().string());
        }
    }

    // Sort paths lexicographically (our naming scheme ensures this is chronological)
    std::sort(sstable_paths.begin(), sstable_paths.end());

    // Register with compaction engine
    compaction_->setSSTables(sstable_paths);

    // Load readers
    reloadSSTableReaders();
}

// ---------------------------------------------------------------------------
// SSTable Management
// ---------------------------------------------------------------------------

void Engine::reloadSSTableReaders() {
    auto paths = compaction_->getSSTables();

    // The compaction engine returns paths from oldest to newest.
    // We want newest first for reads.
    std::reverse(paths.begin(), paths.end());

    std::vector<std::unique_ptr<SSTableReader>> new_readers;
    for (const auto& path : paths) {
        if (fs::exists(path)) {
            new_readers.push_back(std::make_unique<SSTableReader>(path));
        }
    }

    std::lock_guard lock(sstable_mutex_);
    sstable_readers_ = std::move(new_readers);
}

std::string Engine::generateSSTablePath() {
    auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    uint64_t counter = flush_counter_.fetch_add(1);
    return config_.data_dir + "/sstable_flush_" + std::to_string(ts) + "_" +
           std::to_string(counter) + ".sst";
}

void Engine::flushMemTable() {
    auto entries = memtable_->getEntries();
    if (entries.empty()) {
        return;
    }

    // Write to a new SSTable
    std::string sst_path = generateSSTablePath();
    SSTableWriter::write(sst_path, entries);

    // Notify compaction engine (which might trigger a compaction)
    compaction_->notifyNewSSTable(sst_path);

    // Update readers immediately so the new SSTable is available for reads
    reloadSSTableReaders();

    // Clear WAL and rotate MemTable
    wal_->clear();
    memtable_ = std::make_unique<MemTable>(config_.memtable_capacity);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Engine::put(const std::string& key, const std::string& value) {
    wal_->append(key, value);
    memtable_->put(key, value);

    if (memtable_->isFull()) {
        flushMemTable();
    }
}

void Engine::remove(const std::string& key) {
    wal_->appendTombstone(key);
    memtable_->remove(key);

    if (memtable_->isFull()) {
        flushMemTable();
    }
}

std::optional<std::string> Engine::get(const std::string& key) {
    // 1. Check MemTable
    auto mem_res = memtable_->get(key);
    if (mem_res.has_value()) {
        if (mem_res->is_tombstone) {
            return std::nullopt; // Definitively deleted
        }
        return mem_res->value;
    }

    // 2. Check SSTables (newest to oldest)
    std::lock_guard lock(sstable_mutex_);
    for (const auto& reader : sstable_readers_) {
        auto sst_res = reader->get(key);
        if (sst_res.has_value()) {
            if (MemTable::isTombstone(sst_res.value())) {
                return std::nullopt; // Definitively deleted in an SSTable
            }
            return sst_res.value();
        }
    }

    // 3. Not found
    return std::nullopt;
}

} // namespace lsm
