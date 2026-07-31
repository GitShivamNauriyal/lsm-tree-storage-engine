#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace lsm {

/// Node in the skip list. Each node holds a key-value pair and an array of
/// forward pointers (one per level the node participates in).
template <typename K, typename V>
struct SkipListNode {
    K key;
    V value;
    std::vector<SkipListNode*> forward;

    SkipListNode(K k, V v, int level)
        : key(std::move(k)), value(std::move(v)), forward(level, nullptr) {}

    /// Sentinel head node constructor (no key/value).
    explicit SkipListNode(int level)
        : key(), value(), forward(level, nullptr) {}
};

/// A probabilistic skip list providing O(log n) search, insertion, and deletion.
///
/// Parameters:
///   MAX_LEVEL  — maximum height of a node (default 12).
///   PROBABILITY — promotion probability per level (default 0.5).
///
/// Thread safety: None. External locking is required (see MemTable).
template <typename K, typename V>
class SkipList {
public:
    static constexpr int    MAX_LEVEL   = 12;
    static constexpr double PROBABILITY = 0.5;

    SkipList();
    ~SkipList();

    // Non-copyable.
    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    /// Insert or update a key-value pair.
    void insert(const K& key, const V& value);

    /// Search for key. Returns true and fills `value` if found.
    bool search(const K& key, V& value) const;

    /// Remove a key. Returns true if the key existed.
    bool remove(const K& key);

    /// Number of entries.
    size_t size() const { return size_; }

    /// Approximate memory usage in bytes (keys + values + node overhead).
    size_t memoryUsage() const { return memory_usage_; }

    // ------------------------------------------------------------------
    // In-order iteration (for flush to SSTable)
    // ------------------------------------------------------------------

    class Iterator {
    public:
        using Node = SkipListNode<K, V>;

        explicit Iterator(Node* node) : current_(node) {}

        std::pair<K, V> operator*() const {
            return {current_->key, current_->value};
        }

        Iterator& operator++() {
            if (current_) current_ = current_->forward[0];
            return *this;
        }

        bool operator!=(const Iterator& other) const {
            return current_ != other.current_;
        }

        bool operator==(const Iterator& other) const {
            return current_ == other.current_;
        }

    private:
        Node* current_;
    };

    Iterator begin() const { return Iterator(head_->forward[0]); }
    Iterator end()   const { return Iterator(nullptr); }

    /// Collect all entries in sorted order (convenience for flush).
    std::vector<std::pair<K, V>> collectEntries() const;

private:
    using Node = SkipListNode<K, V>;

    Node*  head_;
    int    current_level_;
    size_t size_;
    size_t memory_usage_;

    std::mt19937 rng_;

    int randomLevel();
};

// ==========================================================================
// Implementation (template — must be in header)
// ==========================================================================

template <typename K, typename V>
SkipList<K, V>::SkipList()
    : head_(new Node(MAX_LEVEL)),
      current_level_(1),
      size_(0),
      memory_usage_(0),
      rng_(std::random_device{}()) {}

template <typename K, typename V>
SkipList<K, V>::~SkipList() {
    Node* current = head_;
    while (current) {
        Node* next = current->forward[0];
        delete current;
        current = next;
    }
}

template <typename K, typename V>
int SkipList<K, V>::randomLevel() {
    int level = 1;
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    while (dist(rng_) < PROBABILITY && level < MAX_LEVEL) {
        level++;
    }
    return level;
}

template <typename K, typename V>
void SkipList<K, V>::insert(const K& key, const V& value) {
    // Build update array: update[i] is the rightmost node at level i
    // whose forward pointer at level i needs to be updated.
    std::vector<Node*> update(MAX_LEVEL, nullptr);
    Node* current = head_;

    for (int i = current_level_ - 1; i >= 0; i--) {
        while (current->forward[i] && current->forward[i]->key < key) {
            current = current->forward[i];
        }
        update[i] = current;
    }

    current = current->forward[0];

    // Key already exists — update value in place.
    if (current && current->key == key) {
        // Adjust memory usage for value change.
        memory_usage_ -= current->value.size();
        memory_usage_ += value.size();
        current->value = value;
        return;
    }

    // New node.
    int new_level = randomLevel();

    // If the new level exceeds current_level_, the extra update entries
    // point to head (they'll be the first nodes at those levels).
    if (new_level > current_level_) {
        for (int i = current_level_; i < new_level; i++) {
            update[i] = head_;
        }
        current_level_ = new_level;
    }

    Node* new_node = new Node(key, value, new_level);

    for (int i = 0; i < new_level; i++) {
        new_node->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = new_node;
    }

    size_++;
    // Memory: key bytes + value bytes + node overhead (pointers).
    memory_usage_ += key.size() + value.size()
                   + sizeof(Node)
                   + new_level * sizeof(Node*);
}

template <typename K, typename V>
bool SkipList<K, V>::search(const K& key, V& value) const {
    Node* current = head_;

    for (int i = current_level_ - 1; i >= 0; i--) {
        while (current->forward[i] && current->forward[i]->key < key) {
            current = current->forward[i];
        }
    }

    current = current->forward[0];

    if (current && current->key == key) {
        value = current->value;
        return true;
    }
    return false;
}

template <typename K, typename V>
bool SkipList<K, V>::remove(const K& key) {
    std::vector<Node*> update(MAX_LEVEL, nullptr);
    Node* current = head_;

    for (int i = current_level_ - 1; i >= 0; i--) {
        while (current->forward[i] && current->forward[i]->key < key) {
            current = current->forward[i];
        }
        update[i] = current;
    }

    current = current->forward[0];

    if (!current || current->key != key) {
        return false; // Key not found.
    }

    // Unlink from each level.
    for (int i = 0; i < current_level_; i++) {
        if (update[i]->forward[i] != current) break;
        update[i]->forward[i] = current->forward[i];
    }

    // Adjust memory.
    memory_usage_ -= current->key.size() + current->value.size()
                   + sizeof(Node)
                   + static_cast<size_t>(current->forward.size()) * sizeof(Node*);

    delete current;
    size_--;

    // Reduce current_level_ if top levels are now empty.
    while (current_level_ > 1 && head_->forward[current_level_ - 1] == nullptr) {
        current_level_--;
    }

    return true;
}

template <typename K, typename V>
std::vector<std::pair<K, V>> SkipList<K, V>::collectEntries() const {
    std::vector<std::pair<K, V>> entries;
    entries.reserve(size_);
    Node* current = head_->forward[0];
    while (current) {
        entries.emplace_back(current->key, current->value);
        current = current->forward[0];
    }
    return entries;
}

} // namespace lsm
