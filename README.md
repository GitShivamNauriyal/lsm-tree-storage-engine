# LSM-Tree Key-Value Storage Engine

A high-performance Log-Structured Merge (LSM) Tree Key-Value Storage Engine built from scratch in C++20. Designed for write-heavy workloads, this project demonstrates core principles of modern database architectures, such as RocksDB and LevelDB.

## Features

- **Write-Ahead Log (WAL)**: Ensures durability against crashes by appending operations to disk before memory modification.
- **MemTable**: Thread-safe, lock-free (concurrent) memory buffer backed by a generic SkipList with `O(log N)` operations.
- **SSTable (Sorted String Table)**: Immutable, on-disk file format storing sorted data with a memory-mapped Bloom Filter block and Index block to avoid full disk scans.
- **Compaction Engine**: Background daemon thread that triggers size-tiered K-way merges using min-heaps to eliminate tombstones, drop stale data, and minimize read amplification.
- **Crash Recovery**: Fully recovers state upon startup by replaying WAL and scanning the filesystem for existing SSTables.

## Architecture

1. **Put(k, v) / Delete(k)**: Appends an entry (or tombstone) to the WAL, then inserts it into the active `MemTable`. When the MemTable reaches capacity, it is flushed to an `SSTable` on disk, and a new MemTable/WAL is provisioned.
2. **Get(k)**: Lookups search the `MemTable` first. If not found, they iterate through all active `SSTables` from newest to oldest. To prevent full disk access during reads, the Bloom Filter (`mmap`) is queried first, followed by the Index Block.
3. **Compaction**: When the count of `SSTables` exceeds a given threshold, the background engine runs a K-way merge using a priority queue, keeping only the most recent values for keys and discarding fully superseded ones or tombstones.

## Project Structure

```text
├── Makefile                   # Build definitions
├── include/                   # Header files
│   ├── wal.h
│   ├── skip_list.h
│   ├── memtable.h
│   ├── bloom_filter.h
│   ├── sstable.h
│   ├── sstable_reader.h
│   ├── compaction.h
│   └── engine.h
├── src/                       # Implementation files
│   ├── wal/wal.cpp
│   ├── memtable/skip_list.cpp
│   ├── memtable/memtable.cpp
│   ├── sstable/bloom_filter.cpp
│   ├── sstable/sstable.cpp
│   ├── sstable/sstable_reader.cpp
│   ├── engine/compaction.cpp
│   ├── engine/engine.cpp
│   └── main.cpp
└── tests/                     # Unit tests & Benchmark
    ├── test_wal.cpp
    ├── test_memtable.cpp
    ├── test_sstable.cpp
    ├── test_compaction.cpp
    ├── test_engine.cpp
    └── benchmark.cpp
```

## Requirements

- **Compiler:** GCC with `C++20` support
- **OS:** Linux (or WSL2) - utilizes POSIX `mmap`, `writev`, and atomic operations.

## Build and Run

To compile the primary application entry point:
```bash
make all
./lsm_engine
```

To compile and execute the complete test suite (Phase-by-phase TDD validation):
```bash
make test
```

To compile and run the benchmark suite:
```bash
make benchmark
```

## Performance Note
Running via WSL/Windows Filesystems can introduce significant I/O overhead on `std::ofstream` flush/write ops. True performance benchmarks should be evaluated in a native Linux EXT4 environment.
