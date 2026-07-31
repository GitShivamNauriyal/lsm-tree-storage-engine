#include "memtable.h"
#include "skip_list.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

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

// ==========================================================================
// Skip List Tests
// ==========================================================================

/// Basic insert and search.
void test_skiplist_basic_insert_search() {
    lsm::SkipList<std::string, std::string> sl;

    sl.insert("banana", "yellow");
    sl.insert("apple", "red");
    sl.insert("cherry", "dark_red");

    std::string val;
    ASSERT_TRUE(sl.search("apple", val));
    ASSERT_EQ(val, "red");

    ASSERT_TRUE(sl.search("banana", val));
    ASSERT_EQ(val, "yellow");

    ASSERT_TRUE(sl.search("cherry", val));
    ASSERT_EQ(val, "dark_red");

    ASSERT_TRUE(!sl.search("grape", val));
    ASSERT_EQ(sl.size(), 3u);
}

/// Update existing key.
void test_skiplist_update() {
    lsm::SkipList<std::string, std::string> sl;

    sl.insert("key1", "old");
    sl.insert("key1", "new");

    std::string val;
    ASSERT_TRUE(sl.search("key1", val));
    ASSERT_EQ(val, "new");
    ASSERT_EQ(sl.size(), 1u); // No duplicates.
}

/// Remove.
void test_skiplist_remove() {
    lsm::SkipList<std::string, std::string> sl;

    sl.insert("a", "1");
    sl.insert("b", "2");
    sl.insert("c", "3");

    ASSERT_TRUE(sl.remove("b"));
    ASSERT_EQ(sl.size(), 2u);

    std::string val;
    ASSERT_TRUE(!sl.search("b", val));
    ASSERT_TRUE(sl.search("a", val));
    ASSERT_TRUE(sl.search("c", val));

    // Remove nonexistent.
    ASSERT_TRUE(!sl.remove("z"));
}

/// In-order iteration produces sorted output.
void test_skiplist_sorted_iteration() {
    lsm::SkipList<std::string, std::string> sl;

    // Insert in random order.
    sl.insert("delta", "4");
    sl.insert("alpha", "1");
    sl.insert("gamma", "3");
    sl.insert("beta", "2");

    auto entries = sl.collectEntries();
    ASSERT_EQ(entries.size(), 4u);
    ASSERT_EQ(entries[0].first, "alpha");
    ASSERT_EQ(entries[1].first, "beta");
    ASSERT_EQ(entries[2].first, "delta");
    ASSERT_EQ(entries[3].first, "gamma");
}

/// Insert 10K random keys → verify all retrievable.
void test_skiplist_10k_random() {
    lsm::SkipList<std::string, std::string> sl;

    const int N = 10000;
    std::vector<std::string> keys;
    keys.reserve(N);

    for (int i = 0; i < N; i++) {
        keys.push_back("key_" + std::to_string(i));
    }

    // Shuffle for random insertion order.
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (const auto& k : keys) {
        sl.insert(k, "val_" + k);
    }

    ASSERT_EQ(sl.size(), static_cast<size_t>(N));

    // Verify all keys are retrievable.
    for (const auto& k : keys) {
        std::string val;
        ASSERT_TRUE(sl.search(k, val));
        ASSERT_EQ(val, "val_" + k);
    }

    // Verify sorted order via collectEntries.
    auto entries = sl.collectEntries();
    ASSERT_EQ(entries.size(), static_cast<size_t>(N));
    for (size_t i = 1; i < entries.size(); i++) {
        ASSERT_TRUE(entries[i - 1].first < entries[i].first);
    }
}

/// Memory usage tracks insertions.
void test_skiplist_memory_usage() {
    lsm::SkipList<std::string, std::string> sl;
    ASSERT_EQ(sl.memoryUsage(), 0u);

    sl.insert("key", "value");
    ASSERT_TRUE(sl.memoryUsage() > 0u);

    size_t after_one = sl.memoryUsage();
    sl.insert("another_key", "another_value");
    ASSERT_TRUE(sl.memoryUsage() > after_one);
}

// ==========================================================================
// MemTable Tests
// ==========================================================================

/// Basic put and get.
void test_memtable_put_get() {
    lsm::MemTable mt(1024 * 1024);

    mt.put("hello", "world");
    mt.put("foo", "bar");

    auto r1 = mt.get("hello");
    ASSERT_TRUE(r1.has_value());
    ASSERT_EQ(r1->value, "world");
    ASSERT_TRUE(!r1->is_tombstone);

    auto r2 = mt.get("foo");
    ASSERT_TRUE(r2.has_value());
    ASSERT_EQ(r2->value, "bar");

    auto r3 = mt.get("missing");
    ASSERT_TRUE(!r3.has_value());
}

/// Delete marks tombstone, subsequent get returns tombstone result.
void test_memtable_delete_tombstone() {
    lsm::MemTable mt(1024 * 1024);

    mt.put("key1", "val1");
    mt.remove("key1");

    auto r = mt.get("key1");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->is_tombstone);
    ASSERT_EQ(r->value, "");
}

/// Capacity threshold triggers isFull().
void test_memtable_capacity_threshold() {
    // Tiny threshold: 256 bytes.
    lsm::MemTable mt(256);

    ASSERT_TRUE(!mt.isFull());

    // Fill with enough data to exceed 256 bytes.
    for (int i = 0; i < 100; i++) {
        mt.put("key_" + std::to_string(i), "value_" + std::to_string(i));
        if (mt.isFull()) break;
    }

    ASSERT_TRUE(mt.isFull());
}

/// In-order getEntries produces sorted output.
void test_memtable_sorted_entries() {
    lsm::MemTable mt(1024 * 1024);

    mt.put("zebra", "z");
    mt.put("apple", "a");
    mt.put("mango", "m");
    mt.put("banana", "b");

    auto entries = mt.getEntries();
    ASSERT_EQ(entries.size(), 4u);
    ASSERT_EQ(entries[0].first, "apple");
    ASSERT_EQ(entries[1].first, "banana");
    ASSERT_EQ(entries[2].first, "mango");
    ASSERT_EQ(entries[3].first, "zebra");
}

/// Tombstone entries appear in getEntries with the sentinel value.
void test_memtable_tombstone_in_entries() {
    lsm::MemTable mt(1024 * 1024);

    mt.put("a", "1");
    mt.put("b", "2");
    mt.remove("a"); // tombstone

    auto entries = mt.getEntries();
    ASSERT_EQ(entries.size(), 2u);

    // "a" should have the tombstone sentinel.
    ASSERT_EQ(entries[0].first, "a");
    ASSERT_TRUE(lsm::MemTable::isTombstone(entries[0].second));

    // "b" should have its live value.
    ASSERT_EQ(entries[1].first, "b");
    ASSERT_EQ(entries[1].second, "2");
}

/// Concurrent read/write stress test — multiple threads.
void test_memtable_concurrent_read_write() {
    lsm::MemTable mt(64 * 1024 * 1024); // 64 MB — large enough for the test.

    const int NUM_WRITERS = 4;
    const int NUM_READERS = 4;
    const int OPS_PER_THREAD = 2000;

    std::vector<std::thread> threads;

    // Writer threads: each writes a unique key range.
    for (int w = 0; w < NUM_WRITERS; w++) {
        threads.emplace_back([&mt, w]() {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                std::string key = "w" + std::to_string(w) + "_k" + std::to_string(i);
                mt.put(key, "val_" + std::to_string(i));
            }
        });
    }

    // Reader threads: read random keys (some exist, some don't).
    for (int r = 0; r < NUM_READERS; r++) {
        threads.emplace_back([&mt, r]() {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                std::string key = "w" + std::to_string(r) + "_k" + std::to_string(i);
                mt.get(key); // Result is non-deterministic; just checking no crash/deadlock.
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Verify: all writer keys should be present.
    for (int w = 0; w < NUM_WRITERS; w++) {
        for (int i = 0; i < OPS_PER_THREAD; i++) {
            std::string key = "w" + std::to_string(w) + "_k" + std::to_string(i);
            auto r = mt.get(key);
            ASSERT_TRUE(r.has_value());
            ASSERT_TRUE(!r->is_tombstone);
        }
    }

    ASSERT_EQ(mt.size(), static_cast<size_t>(NUM_WRITERS * OPS_PER_THREAD));
}

// ==========================================================================
// Main
// ==========================================================================

int main() {
    int prev_failed = 0;

    std::cout << "========================================\n";
    std::cout << "  SkipList Unit Tests\n";
    std::cout << "========================================\n";

    RUN_TEST(test_skiplist_basic_insert_search);
    RUN_TEST(test_skiplist_update);
    RUN_TEST(test_skiplist_remove);
    RUN_TEST(test_skiplist_sorted_iteration);
    RUN_TEST(test_skiplist_10k_random);
    RUN_TEST(test_skiplist_memory_usage);

    std::cout << "\n========================================\n";
    std::cout << "  MemTable Unit Tests\n";
    std::cout << "========================================\n";

    RUN_TEST(test_memtable_put_get);
    RUN_TEST(test_memtable_delete_tombstone);
    RUN_TEST(test_memtable_capacity_threshold);
    RUN_TEST(test_memtable_sorted_entries);
    RUN_TEST(test_memtable_tombstone_in_entries);
    RUN_TEST(test_memtable_concurrent_read_write);

    std::cout << "\n========================================\n";
    std::cout << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    std::cout << "========================================\n";

    return tests_failed > 0 ? 1 : 0;
}
