CXX      := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -Werror -O2 -pthread
INCLUDES := -Iinclude

# Source directories
SRC_WAL      := src/wal
SRC_MEMTABLE := src/memtable
SRC_SSTABLE  := src/sstable
SRC_ENGINE   := src/engine

# Object files (accumulated per phase)
OBJ_WAL      := $(SRC_WAL)/wal.o
OBJ_MEMTABLE := $(SRC_MEMTABLE)/skip_list.o $(SRC_MEMTABLE)/memtable.o
OBJ_SSTABLE  := $(SRC_SSTABLE)/bloom_filter.o $(SRC_SSTABLE)/sstable.o $(SRC_SSTABLE)/sstable_reader.o
OBJ_ENGINE   := $(SRC_ENGINE)/compaction.o $(SRC_ENGINE)/engine.o

OBJ_ALL := $(OBJ_WAL) $(OBJ_MEMTABLE) $(OBJ_SSTABLE) $(OBJ_ENGINE)

# Test binaries
TEST_WAL        := tests/test_wal
TEST_MEMTABLE   := tests/test_memtable
TEST_SSTABLE    := tests/test_sstable
TEST_COMPACTION := tests/test_compaction
TEST_ENGINE     := tests/test_engine
BENCHMARK       := tests/benchmark

# Main binary
MAIN := lsm_engine

# ==============================================================================
# Build rules
# ==============================================================================

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

all: $(OBJ_ALL) src/main.o
	$(CXX) $(CXXFLAGS) $(OBJ_ALL) src/main.o -o $(MAIN)

# ==============================================================================
# Phase-specific test targets
# ==============================================================================

test_wal: $(OBJ_WAL) tests/test_wal.o
	$(CXX) $(CXXFLAGS) $(OBJ_WAL) tests/test_wal.o -o $(TEST_WAL)
	./$(TEST_WAL)

test_memtable: $(OBJ_WAL) $(OBJ_MEMTABLE) tests/test_memtable.o
	$(CXX) $(CXXFLAGS) $(OBJ_WAL) $(OBJ_MEMTABLE) tests/test_memtable.o -o $(TEST_MEMTABLE)
	./$(TEST_MEMTABLE)

test_sstable: $(OBJ_WAL) $(OBJ_MEMTABLE) $(OBJ_SSTABLE) tests/test_sstable.o
	$(CXX) $(CXXFLAGS) $(OBJ_WAL) $(OBJ_MEMTABLE) $(OBJ_SSTABLE) tests/test_sstable.o -o $(TEST_SSTABLE)
	./$(TEST_SSTABLE)

test_compaction: $(OBJ_WAL) $(OBJ_MEMTABLE) $(OBJ_SSTABLE) $(OBJ_ENGINE) tests/test_compaction.o
	$(CXX) $(CXXFLAGS) $(OBJ_ALL) tests/test_compaction.o -o $(TEST_COMPACTION)
	./$(TEST_COMPACTION)

test_engine: $(OBJ_ALL) tests/test_engine.o
	$(CXX) $(CXXFLAGS) $(OBJ_ALL) tests/test_engine.o -o $(TEST_ENGINE)
	./$(TEST_ENGINE)

test: test_wal test_memtable test_sstable test_compaction test_engine

# ==============================================================================
# Benchmark
# ==============================================================================

benchmark: $(OBJ_ALL) tests/benchmark.o
	$(CXX) $(CXXFLAGS) $(OBJ_ALL) tests/benchmark.o -o $(BENCHMARK)
	./$(BENCHMARK)

# ==============================================================================
# Clean
# ==============================================================================

clean:
	rm -f $(OBJ_ALL) src/main.o tests/*.o
	rm -f $(MAIN) $(TEST_WAL) $(TEST_MEMTABLE) $(TEST_SSTABLE) $(TEST_COMPACTION) $(TEST_ENGINE) $(BENCHMARK)
	rm -rf data/ test_data_*

.PHONY: all test test_wal test_memtable test_sstable test_compaction test_engine benchmark clean
