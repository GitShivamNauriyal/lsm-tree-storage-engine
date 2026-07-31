#include "engine.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static const std::string BENCH_DIR = "test_data_benchmark";

void run_benchmark(int num_ops) {
    fs::remove_all(BENCH_DIR);
    
    lsm::Engine::Config config;
    config.data_dir = BENCH_DIR;
    config.memtable_capacity = 4 * 1024 * 1024; // 4MB
    config.compaction_threshold = 4;
    
    {
        lsm::Engine engine(config);
        
        std::vector<std::string> keys;
        keys.reserve(num_ops);
        for (int i = 0; i < num_ops; ++i) {
            keys.push_back("key_" + std::to_string(i));
        }
        
        // Setup random generator for reads
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, num_ops - 1);

        std::cout << "Starting benchmark with " << num_ops << " operations..." << std::endl << std::endl;

        // ---------------------------------------------------------
        // WRITE BENCHMARK
        // ---------------------------------------------------------
        auto start_write = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < num_ops; ++i) {
            engine.put(keys[i], "value_" + std::to_string(i));
        }
        auto end_write = std::chrono::high_resolution_clock::now();
        
        std::chrono::duration<double> write_diff = end_write - start_write;
        double writes_per_sec = num_ops / write_diff.count();
        
        std::cout << "Write Performance:" << std::endl;
        std::cout << "  Total Time : " << write_diff.count() << " seconds" << std::endl;
        std::cout << "  Throughput : " << static_cast<int>(writes_per_sec) << " ops/sec" << std::endl << std::endl;

        // ---------------------------------------------------------
        // READ BENCHMARK (RANDOM)
        // ---------------------------------------------------------
        auto start_read = std::chrono::high_resolution_clock::now();
        int found = 0;
        for (int i = 0; i < num_ops; ++i) {
            int idx = dis(gen);
            auto val = engine.get(keys[idx]);
            if (val.has_value()) {
                found++;
            }
        }
        auto end_read = std::chrono::high_resolution_clock::now();
        
        std::chrono::duration<double> read_diff = end_read - start_read;
        double reads_per_sec = num_ops / read_diff.count();
        
        std::cout << "Read Performance (Random):" << std::endl;
        std::cout << "  Total Time : " << read_diff.count() << " seconds" << std::endl;
        std::cout << "  Throughput : " << static_cast<int>(reads_per_sec) << " ops/sec" << std::endl;
        std::cout << "  Found      : " << found << " / " << num_ops << std::endl << std::endl;
    } // Engine destructor is called here and writes final MemTable

    // Cleanup
    fs::remove_all(BENCH_DIR);
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  LSM-Tree Benchmark" << std::endl;
    std::cout << "========================================" << std::endl;
    
    run_benchmark(10000); // 10k operations
    
    return 0;
}
