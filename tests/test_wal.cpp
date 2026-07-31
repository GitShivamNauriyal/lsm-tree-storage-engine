#include "wal.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static const std::string TEST_DIR = "test_data_wal";
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

// --------------------------------------------------------------------------
// Tests
// --------------------------------------------------------------------------

/// Write N records, close, reopen, replay — verify exact key/value pairs.
void test_basic_put_and_recovery() {
    cleanTestDir();
    const std::string path = TEST_DIR + "/wal_basic.log";

    {
        lsm::WAL wal(path);
        wal.append("key1", "value1");
        wal.append("key2", "value2");
        wal.append("key3", "value3");
    }

    // Reopen and recover.
    {
        lsm::WAL wal(path);
        auto records = wal.recover();
        ASSERT_EQ(records.size(), 3u);

        ASSERT_EQ(records[0].key, "key1");
        ASSERT_EQ(records[0].value, "value1");
        ASSERT_TRUE(!records[0].is_tombstone);

        ASSERT_EQ(records[1].key, "key2");
        ASSERT_EQ(records[1].value, "value2");
        ASSERT_TRUE(!records[1].is_tombstone);

        ASSERT_EQ(records[2].key, "key3");
        ASSERT_EQ(records[2].value, "value3");
        ASSERT_TRUE(!records[2].is_tombstone);
    }
}

/// Interleave puts and deletes — replay — verify tombstone flags.
void test_tombstone_interleave() {
    cleanTestDir();
    const std::string path = TEST_DIR + "/wal_tombstone.log";

    {
        lsm::WAL wal(path);
        wal.append("alpha", "100");
        wal.appendTombstone("beta");
        wal.append("gamma", "300");
        wal.appendTombstone("alpha");
        wal.append("delta", "400");
    }

    {
        lsm::WAL wal(path);
        auto records = wal.recover();
        ASSERT_EQ(records.size(), 5u);

        // alpha → live
        ASSERT_EQ(records[0].key, "alpha");
        ASSERT_EQ(records[0].value, "100");
        ASSERT_TRUE(!records[0].is_tombstone);

        // beta → tombstone
        ASSERT_EQ(records[1].key, "beta");
        ASSERT_TRUE(records[1].is_tombstone);
        ASSERT_TRUE(records[1].value.empty());

        // gamma → live
        ASSERT_EQ(records[2].key, "gamma");
        ASSERT_EQ(records[2].value, "300");
        ASSERT_TRUE(!records[2].is_tombstone);

        // alpha → tombstone
        ASSERT_EQ(records[3].key, "alpha");
        ASSERT_TRUE(records[3].is_tombstone);

        // delta → live
        ASSERT_EQ(records[4].key, "delta");
        ASSERT_EQ(records[4].value, "400");
        ASSERT_TRUE(!records[4].is_tombstone);
    }
}

/// Write many records, verify high-volume recovery.
void test_large_volume_recovery() {
    cleanTestDir();
    const std::string path = TEST_DIR + "/wal_large.log";
    const int N = 5000;

    {
        lsm::WAL wal(path);
        for (int i = 0; i < N; i++) {
            wal.append("key_" + std::to_string(i), "val_" + std::to_string(i));
        }
    }

    {
        lsm::WAL wal(path);
        auto records = wal.recover();
        ASSERT_EQ(records.size(), static_cast<size_t>(N));

        for (int i = 0; i < N; i++) {
            ASSERT_EQ(records[i].key, "key_" + std::to_string(i));
            ASSERT_EQ(records[i].value, "val_" + std::to_string(i));
            ASSERT_TRUE(!records[i].is_tombstone);
        }
    }
}

/// Corruption resilience: truncate last record mid-write.
/// Recovery should return only the complete records before the corruption.
void test_truncated_tail_resilience() {
    cleanTestDir();
    const std::string path = TEST_DIR + "/wal_corrupt.log";

    // Write 3 good records.
    {
        lsm::WAL wal(path);
        wal.append("good1", "val1");
        wal.append("good2", "val2");
        wal.append("good3", "val3");
    }

    // Manually truncate the file to simulate a crash mid-write.
    // Remove the last 4 bytes (partial value_size or value of "good3").
    {
        auto file_size = fs::file_size(path);
        fs::resize_file(path, file_size - 4);
    }

    // Recovery should return the 2 complete records; the truncated 3rd is discarded.
    {
        lsm::WAL wal(path);
        auto records = wal.recover();
        ASSERT_EQ(records.size(), 2u);
        ASSERT_EQ(records[0].key, "good1");
        ASSERT_EQ(records[1].key, "good2");
    }
}

/// Test clear() — after clear, recovery yields zero records.
void test_clear() {
    cleanTestDir();
    const std::string path = TEST_DIR + "/wal_clear.log";

    {
        lsm::WAL wal(path);
        wal.append("a", "1");
        wal.append("b", "2");
        wal.clear();

        // After clear, recovery from same fd should be empty.
        auto records = wal.recover();
        ASSERT_EQ(records.size(), 0u);
    }

    // Reopen — still empty.
    {
        lsm::WAL wal(path);
        auto records = wal.recover();
        ASSERT_EQ(records.size(), 0u);
    }
}

/// Test append after clear — new records survive recovery.
void test_append_after_clear() {
    cleanTestDir();
    const std::string path = TEST_DIR + "/wal_append_after_clear.log";

    {
        lsm::WAL wal(path);
        wal.append("old", "data");
        wal.clear();
        wal.append("new1", "fresh1");
        wal.append("new2", "fresh2");
    }

    {
        lsm::WAL wal(path);
        auto records = wal.recover();
        ASSERT_EQ(records.size(), 2u);
        ASSERT_EQ(records[0].key, "new1");
        ASSERT_EQ(records[0].value, "fresh1");
        ASSERT_EQ(records[1].key, "new2");
        ASSERT_EQ(records[1].value, "fresh2");
    }
}

/// Test empty WAL recovery — no records, no crash.
void test_empty_wal_recovery() {
    cleanTestDir();
    const std::string path = TEST_DIR + "/wal_empty.log";

    {
        lsm::WAL wal(path);
        auto records = wal.recover();
        ASSERT_EQ(records.size(), 0u);
    }
}

// --------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------

int main() {
    int prev_failed = 0;

    std::cout << "========================================\n";
    std::cout << "  WAL Unit Tests\n";
    std::cout << "========================================\n";

    RUN_TEST(test_basic_put_and_recovery);
    RUN_TEST(test_tombstone_interleave);
    RUN_TEST(test_large_volume_recovery);
    RUN_TEST(test_truncated_tail_resilience);
    RUN_TEST(test_clear);
    RUN_TEST(test_append_after_clear);
    RUN_TEST(test_empty_wal_recovery);

    std::cout << "========================================\n";
    std::cout << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    std::cout << "========================================\n";

    // Cleanup test artifacts.
    fs::remove_all(TEST_DIR);

    return tests_failed > 0 ? 1 : 0;
}
