#include "memtable.h"

namespace lsm {

// Tombstone sentinel — a single 0xFF byte that cannot appear in normal UTF-8 values.
const std::string MemTable::TOMBSTONE_VALUE(1, TOMBSTONE_MARKER);

MemTable::MemTable(size_t capacity_bytes)
    : capacity_bytes_(capacity_bytes) {}

void MemTable::put(const std::string& key, const std::string& value) {
    std::unique_lock lock(mutex_);
    skiplist_.insert(key, value);
}

void MemTable::remove(const std::string& key) {
    std::unique_lock lock(mutex_);
    skiplist_.insert(key, TOMBSTONE_VALUE);
}

std::optional<MemTable::LookupResult> MemTable::get(const std::string& key) const {
    std::shared_lock lock(mutex_);
    std::string value;
    if (skiplist_.search(key, value)) {
        if (value == TOMBSTONE_VALUE) {
            return LookupResult{"", true};
        }
        return LookupResult{std::move(value), false};
    }
    return std::nullopt;
}

bool MemTable::isFull() const {
    std::shared_lock lock(mutex_);
    return skiplist_.memoryUsage() >= capacity_bytes_;
}

size_t MemTable::size() const {
    std::shared_lock lock(mutex_);
    return skiplist_.size();
}

size_t MemTable::memoryUsage() const {
    std::shared_lock lock(mutex_);
    return skiplist_.memoryUsage();
}

std::vector<MemTable::Entry> MemTable::getEntries() const {
    std::shared_lock lock(mutex_);
    return skiplist_.collectEntries();
}

} // namespace lsm
