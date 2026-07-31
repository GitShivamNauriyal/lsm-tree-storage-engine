#include "bloom_filter.h"
#include "memtable.h"
#include "sstable.h"
#include "sstable_reader.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static const std::string TEST_DIR = "test_data_sstable";
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

static std::vector<std::pair<std::string, std::string>> generateSortedEntries(int n) {
    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(n);
    for (int i = 0; i < n; i++) {
        // Zero-padded keys ensure natural sort == lexicographic sort.
        char key[32], val[32];
        std::snprintf(key, sizeof(key), "key_%08d", i);
        std::snprintf(val, sizeof(val), "val_%08d", i);
        entries.emplace_back(key, val);
    }
    return entries;
}

// ==========================================================================
// Bloom Filter Tests
// ==========================================================================

/// Zero false negatives: every added key must be found.
void test_bloom_zero_false_negatives() {
    const int N = 10000;
    lsm::BloomFilter bf(N, 0.01);

    for (int i = 0; i < N; i++) {
        bf.add("key_" + std::to_string(i));
    }

    for (int i = 0; i < N; i++) {
        ASSERT_TRUE(bf.mayContain("key_" + std::to_string(i)));
    }
}

/// False-positive rate should be < 2% for keys that were never added.
void test_bloom_false_positive_rate() {
    const int N = 10000;
    lsm::BloomFilter bf(N, 0.01);

    for (int i = 0; i < N; i++) {
        bf.add("key_" + std::to_string(i));
    }

    // Test 10K keys that were never inserted.
    int false_positives = 0;
    for (int i = N; i < 2 * N; i++) {
        if (bf.mayContain("key_" + std::to_string(i))) {
            false_positives++;
        }
    }

    double fp_rate = static_cast<double>(false_positives) / N;
    std::cout << "    Bloom FP rate: " << (fp_rate * 100.0) << "%\n";
    ASSERT_TRUE(fp_rate < 0.02); // Must be under 2%.
}

/// Serialization round-trip: serialize → deserialize → same behavior.
void test_bloom_serialization_roundtrip() {
    const int N = 1000;
    lsm::BloomFilter bf(N, 0.01);

    for (int i = 0; i < N; i++) {
        bf.add("item_" + std::to_string(i));
    }

    auto data = bf.serialize();
    lsm::BloomFilter bf2(data.data(), data.size(), bf.numHashes(), bf.numBits());

    // All original keys should still be found.
    for (int i = 0; i < N; i++) {
        ASSERT_TRUE(bf2.mayContain("item_" + std::to_string(i)));
    }
}

// ==========================================================================
// SSTable Writer + Reader Tests
// ==========================================================================

/// Write 10K sorted entries → read back every key → verify values.
void test_sstable_write_read_10k() {
    cleanTestDir();
    const std::string path = TEST_DIR + "/test.sst";
    const int N = 10000;

    auto entries = generateSortedEntries(N);
    lsm::SSTableWriter::write(path, entries);

    lsm::SSTableReader reader(path);
    ASSERT_EQ(reader.numEntries(), static_cast<size_t>(N));

    for (const auto& [key, val] : entries) {
        auto result = reader.get(key);
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result.value(), val);
    }
}

/// Lookup nonexistent key → bloom rejects without full disk read.
void test_sstable_bloom_rejects_missing() {
    cleanTestDir();
    const std::string path = TEST_DIR + "/bloom_reject.sst";

    auto entries = generateSortedEntries(1000);
    lsm::SSTableWriter::write(path, entries);

    lsm::SSTableReader reader(path);

    // Keys outside the range should be rejected (most by bloom, some by index).
    int rejected = 0;
    for (int i = 1000; i < 2000; i++) {
        char key[32];
        std::snprintf(key, sizeof(key), "key_%08d", i);
        auto result = reader.get(key);
        if (!result.has_value()) rejected++;
    }

    // All 1000 missing keys must return nullopt.
    ASSERT_EQ(rejected, 1000);
}

/// mmap bloom: verify the mmap'd region works after reopening the file.
void test_sstable_mmap_bloom_reopen() {
    cleanTestDir();
    const std::string path = TEST_DIR + "/mmap_bloom.sst";

    auto entries = generateSortedEntries(500);
    lsm::SSTableWriter::write(path, entries);

    // Open, close, reopen — mmap should work each time.
    for (int trial = 0; trial < 3; trial++) {
        lsm::SSTableReader reader(path);
        auto result = reader.get("key_00000000");
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result.value(), "val_00000000");

        auto missing = reader.get("nonexistent_key");
        ASSERT_TRUE(!missing.has_value());
    }
}

/// getAllEntries returns all entries in sorted order.
void test_sstable_get_all_entries() {
    cleanTestDir();
    const std::string path = TEST_DIR + "/all_entries.sst";

    auto entries = generateSortedEntries(100);
    lsm::SSTableWriter::write(path, entries);

    lsm::SSTableReader reader(path);
    auto all = reader.getAllEntries();
    ASSERT_EQ(all.size(), 100u);

    for (size_t i = 0; i < all.size(); i++) {
        ASSERT_EQ(all[i].first, entries[i].first);
        ASSERT_EQ(all[i].second, entries[i].second);
    }
}

/// Tombstone entries are preserved through write/read cycle.
void test_sstable_tombstone_preservation() {
    cleanTestDir();
    const std::string path = TEST_DIR + "/tombstones.sst";

    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back("alive_key", "value");
    entries.emplace_back("dead_key", lsm::MemTable::TOMBSTONE_VALUE);
    entries.emplace_back("another_alive", "data");

    // Sort entries.
    std::sort(entries.begin(), entries.end());

    lsm::SSTableWriter::write(path, entries);

    lsm::SSTableReader reader(path);

    auto r1 = reader.get("alive_key");
    ASSERT_TRUE(r1.has_value());
    ASSERT_EQ(r1.value(), "value");

    auto r2 = reader.get("dead_key");
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(lsm::MemTable::isTombstone(r2.value()));

    auto r3 = reader.get("another_alive");
    ASSERT_TRUE(r3.has_value());
    ASSERT_EQ(r3.value(), "data");
}

// ==========================================================================
// Main
// ==========================================================================

int main() {
    int prev_failed = 0;

    std::cout << "========================================\n";
    std::cout << "  Bloom Filter Unit Tests\n";
    std::cout << "========================================\n";

    RUN_TEST(test_bloom_zero_false_negatives);
    RUN_TEST(test_bloom_false_positive_rate);
    RUN_TEST(test_bloom_serialization_roundtrip);

    std::cout << "\n========================================\n";
    std::cout << "  SSTable Unit Tests\n";
    std::cout << "========================================\n";

    RUN_TEST(test_sstable_write_read_10k);
    RUN_TEST(test_sstable_bloom_rejects_missing);
    RUN_TEST(test_sstable_mmap_bloom_reopen);
    RUN_TEST(test_sstable_get_all_entries);
    RUN_TEST(test_sstable_tombstone_preservation);

    std::cout << "\n========================================\n";
    std::cout << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    std::cout << "========================================\n";

    // Cleanup.
    fs::remove_all(TEST_DIR);

    return tests_failed > 0 ? 1 : 0;
}
