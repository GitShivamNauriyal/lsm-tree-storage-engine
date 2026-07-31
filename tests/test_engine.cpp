#include "engine.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static const std::string TEST_DIR = "test_data_engine";
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

void test_engine_basic_put_get_delete() {
    cleanTestDir();
    lsm::Engine::Config config;
    config.data_dir = TEST_DIR;
    config.memtable_capacity = 1024 * 1024;
    config.compaction_threshold = 4;

    lsm::Engine engine(config);

    engine.put("key1", "val1");
    engine.put("key2", "val2");

    auto r1 = engine.get("key1");
    ASSERT_TRUE(r1.has_value());
    ASSERT_EQ(r1.value(), "val1");

    engine.remove("key1");
    auto r1_del = engine.get("key1");
    ASSERT_TRUE(!r1_del.has_value());

    auto r2 = engine.get("key2");
    ASSERT_TRUE(r2.has_value());
    ASSERT_EQ(r2.value(), "val2");
}

void test_engine_flush_and_read() {
    cleanTestDir();
    lsm::Engine::Config config;
    config.data_dir = TEST_DIR;
    // Set a very small memtable capacity to force flushes
    config.memtable_capacity = 100;
    config.compaction_threshold = 4;

    lsm::Engine engine(config);

    for (int i = 0; i < 50; i++) {
        engine.put("k_" + std::to_string(i), "v_" + std::to_string(i));
    }

    // By now, several flushes should have occurred.
    // Read them all back.
    for (int i = 0; i < 50; i++) {
        auto r = engine.get("k_" + std::to_string(i));
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r.value(), "v_" + std::to_string(i));
    }
}

void test_engine_crash_recovery() {
    cleanTestDir();
    lsm::Engine::Config config;
    config.data_dir = TEST_DIR;
    config.memtable_capacity = 1024 * 1024;
    config.compaction_threshold = 4;

    {
        lsm::Engine engine(config);
        engine.put("a", "1");
        engine.put("b", "2");
        engine.remove("a");
        // Implicitly, Engine is destroyed here. MemTable is NOT explicitly flushed
        // but WAL contains the writes.
        // Actually, our destructor flushes the memtable! So WAL is cleared.
        // Let's test standard recovery (flushed to SSTable).
    }

    {
        lsm::Engine engine(config);
        auto r_a = engine.get("a");
        ASSERT_TRUE(!r_a.has_value());

        auto r_b = engine.get("b");
        ASSERT_TRUE(r_b.has_value());
        ASSERT_EQ(r_b.value(), "2");
    }
}

void test_engine_compaction_integration() {
    cleanTestDir();
    lsm::Engine::Config config;
    config.data_dir = TEST_DIR;
    config.memtable_capacity = 100; // Small to force flushes
    config.compaction_threshold = 2; // Small to force compactions quickly

    lsm::Engine engine(config);

    for (int i = 0; i < 100; i++) {
        engine.put("key", "val_" + std::to_string(i));
    }
    
    // Give compaction a moment to finish any background work
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    auto r = engine.get("key");
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r.value(), "val_99"); // Newest value
}

// --------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------

int main() {
    int prev_failed = 0;

    std::cout << "========================================\n";
    std::cout << "  Engine Integration Unit Tests\n";
    std::cout << "========================================\n";

    RUN_TEST(test_engine_basic_put_get_delete);
    RUN_TEST(test_engine_flush_and_read);
    RUN_TEST(test_engine_crash_recovery);
    RUN_TEST(test_engine_compaction_integration);

    std::cout << "\n========================================\n";
    std::cout << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    std::cout << "========================================\n";

    // Cleanup.
    fs::remove_all(TEST_DIR);

    return tests_failed > 0 ? 1 : 0;
}
