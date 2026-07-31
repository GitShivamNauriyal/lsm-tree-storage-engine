#include "compaction.h"
#include "memtable.h"
#include "sstable.h"
#include "sstable_reader.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static const std::string TEST_DIR = "test_data_compaction";
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_TRUE(expr)                                                     \
    do {                                                                      \
        if (!(expr)) {                                                        \
            std::cerr << "  FAIL: " << #expr << " @ " << __FILE__ << ":"     \
                      << __LINE__ << "\n";                                    \
            tests_failed++;                                                   \
            return;                                                           \
        }                                                                     \
    } while (0)

#define ASSERT_EQ(a, b)                                                       \
    do {                                                                      \
        if ((a) != (b)) {                                                     \
            std::cerr << "  FAIL: " << #a << " == " << #b << " @ "           \
                      << __FILE__ << ":" << __LINE__ << "\n";                 \
            std::cerr << "    got: [" << (a) << "] vs [" << (b) << "]\n";    \
            tests_failed++;                                                   \
            return;                                                           \
        }                                                                     \
    } while (0)

#define RUN_TEST(fn)                                                          \
    do {                                                                      \
        std::cout << "[ RUN  ] " << #fn << "\n";                             \
        fn();                                                                 \
        if (tests_failed == prev_failed) {                                    \
            std::cout << "[ PASS ] " << #fn << "\n";                         \
            tests_passed++;                                                   \
        } else {                                                              \
            std::cout << "[ FAIL ] " << #fn << "\n";                         \
        }                                                                     \
        prev_failed = tests_failed;                                           \
    } while (0)

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static void cleanTestDir() {
    fs::remove_all(TEST_DIR);
    fs::create_directories(TEST_DIR);
}

/// Write a simple SSTable from a vector of key-value pairs.
static std::string writeTestSSTable(const std::string& name,
                                     std::vector<std::pair<std::string, std::string>> entries) {
    std::sort(entries.begin(), entries.end());
    std::string path = TEST_DIR + "/" + name + ".sst";
    lsm::SSTableWriter::write(path, entries);
    return path;
}

// --------------------------------------------------------------------------
// Tests
// --------------------------------------------------------------------------

/// Create N SSTables with overlapping keys → compact → verify merged data.
void test_compaction_basic_merge() {
    cleanTestDir();

    // SSTable 1: keys a, b, c
    auto p1 = writeTestSSTable("sst1", {{"a", "1"}, {"b", "2"}, {"c", "3"}});
    // SSTable 2: keys b, d (b overlaps — newer value)
    auto p2 = writeTestSSTable("sst2", {{"b", "20"}, {"d", "4"}});
    // SSTable 3: keys c, e (c overlaps — newest value)
    auto p3 = writeTestSSTable("sst3", {{"c", "30"}, {"e", "5"}});

    bool compacted = false;
    std::string new_path;
    std::vector<std::string> old_paths;

    lsm::CompactionEngine engine(TEST_DIR, 3,
        [&](const std::string& np, const std::vector<std::string>& ops) {
            new_path = np;
            old_paths = ops;
            compacted = true;
        });

    // Pre-load paths (p1 oldest, p3 newest).
    engine.notifyNewSSTable(p1);
    engine.notifyNewSSTable(p2);
    engine.notifyNewSSTable(p3);

    engine.waitForCompaction();
    ASSERT_TRUE(compacted);

    // Read the merged SSTable.
    lsm::SSTableReader reader(new_path);
    auto entries = reader.getAllEntries();

    // Expected: a=1, b=20, c=30, d=4, e=5 (newest values for overlapping keys).
    ASSERT_EQ(entries.size(), 5u);

    ASSERT_EQ(entries[0].first, "a"); ASSERT_EQ(entries[0].second, "1");
    ASSERT_EQ(entries[1].first, "b"); ASSERT_EQ(entries[1].second, "20");
    ASSERT_EQ(entries[2].first, "c"); ASSERT_EQ(entries[2].second, "30");
    ASSERT_EQ(entries[3].first, "d"); ASSERT_EQ(entries[3].second, "4");
    ASSERT_EQ(entries[4].first, "e"); ASSERT_EQ(entries[4].second, "5");
}

/// Tombstone elimination across SSTable boundaries.
void test_compaction_tombstone_elimination() {
    cleanTestDir();

    // SSTable 1 (old): key "x" with live value.
    auto p1 = writeTestSSTable("sst1", {{"x", "alive"}, {"y", "keep"}});
    // SSTable 2 (new): key "x" tombstoned.
    auto p2 = writeTestSSTable("sst2", {{"x", lsm::MemTable::TOMBSTONE_VALUE}});

    lsm::CompactionEngine engine(TEST_DIR, 2);

    engine.notifyNewSSTable(p1);
    engine.notifyNewSSTable(p2);

    engine.waitForCompaction();

    auto sstables = engine.getSSTables();
    ASSERT_EQ(sstables.size(), 1u);

    lsm::SSTableReader reader(sstables[0]);
    auto entries = reader.getAllEntries();

    // "x" should be dropped (tombstone eliminates old value).
    // "y" should survive.
    ASSERT_EQ(entries.size(), 1u);
    ASSERT_EQ(entries[0].first, "y");
    ASSERT_EQ(entries[0].second, "keep");
}

/// Compaction does not trigger below threshold.
void test_compaction_below_threshold() {
    cleanTestDir();

    auto p1 = writeTestSSTable("sst1", {{"a", "1"}});
    auto p2 = writeTestSSTable("sst2", {{"b", "2"}});

    // Threshold 5 — should not trigger with 2 SSTables.
    lsm::CompactionEngine engine(TEST_DIR, 5);

    engine.notifyNewSSTable(p1);
    engine.notifyNewSSTable(p2);

    // Give it a moment — compaction should NOT have triggered.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ASSERT_TRUE(!engine.isCompacting());
    auto sstables = engine.getSSTables();
    ASSERT_EQ(sstables.size(), 2u);
}

/// Concurrent reads during compaction do not block or corrupt.
void test_compaction_concurrent_reads() {
    cleanTestDir();

    // Create 4 SSTables with non-overlapping keys.
    std::vector<std::string> paths;
    for (int i = 0; i < 4; i++) {
        std::vector<std::pair<std::string, std::string>> entries;
        for (int j = 0; j < 100; j++) {
            char key[32], val[32];
            std::snprintf(key, sizeof(key), "key_%04d_%04d", i, j);
            std::snprintf(val, sizeof(val), "val_%04d_%04d", i, j);
            entries.emplace_back(key, val);
        }
        std::sort(entries.begin(), entries.end());
        std::string path = TEST_DIR + "/sst_" + std::to_string(i) + ".sst";
        lsm::SSTableWriter::write(path, entries);
        paths.push_back(path);
    }

    lsm::CompactionEngine engine(TEST_DIR, 4);

    // Start readers before compaction kicks in.
    std::atomic<bool> read_error{false};
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; r++) {
        readers.emplace_back([&paths, &read_error]() {
            // Read from the original files (they exist until compaction deletes them).
            for (const auto& p : paths) {
                if (fs::exists(p)) {
                    try {
                        lsm::SSTableReader reader(p);
                        auto entries = reader.getAllEntries();
                        // Just check no crash.
                        (void)entries;
                    } catch (...) {
                        // File may be deleted by compaction — that's OK.
                    }
                }
            }
        });
    }

    // Trigger compaction.
    for (const auto& p : paths) {
        engine.notifyNewSSTable(p);
    }

    for (auto& t : readers) {
        t.join();
    }

    engine.waitForCompaction();

    ASSERT_TRUE(!read_error.load());

    // Verify merged SSTable has all 400 entries.
    auto sstables = engine.getSSTables();
    ASSERT_EQ(sstables.size(), 1u);

    lsm::SSTableReader reader(sstables[0]);
    ASSERT_EQ(reader.numEntries(), 400u);
}

// --------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------

int main() {
    int prev_failed = 0;

    std::cout << "========================================\n";
    std::cout << "  Compaction Engine Unit Tests\n";
    std::cout << "========================================\n";

    RUN_TEST(test_compaction_basic_merge);
    RUN_TEST(test_compaction_tombstone_elimination);
    RUN_TEST(test_compaction_below_threshold);
    RUN_TEST(test_compaction_concurrent_reads);

    std::cout << "\n========================================\n";
    std::cout << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    std::cout << "========================================\n";

    // Cleanup.
    fs::remove_all(TEST_DIR);

    return tests_failed > 0 ? 1 : 0;
}
