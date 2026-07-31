#include "sstable_reader.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

namespace lsm {

// ---------------------------------------------------------------------------
// Helper: read exactly `count` bytes from fd at `offset` (pread).
// ---------------------------------------------------------------------------
static bool preadExact(int fd, void* buf, size_t count, off_t offset) {
    size_t total = 0;
    auto* p = static_cast<char*>(buf);
    while (total < count) {
        ssize_t n = ::pread(fd, p + total, count - total, offset + static_cast<off_t>(total));
        if (n <= 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

SSTableReader::SSTableReader(const std::string& path)
    : path_(path), fd_(-1), footer_{}, bloom_mmap_addr_(nullptr), bloom_mmap_len_(0) {

    fd_ = ::open(path_.c_str(), O_RDONLY);
    if (fd_ < 0) {
        throw std::runtime_error("SSTableReader::open failed: " +
                                 std::string(std::strerror(errno)));
    }

    readFooter();
    readIndex();
    mmapBloom();
}

SSTableReader::~SSTableReader() {
    if (bloom_mmap_addr_ && bloom_mmap_addr_ != MAP_FAILED) {
        ::munmap(bloom_mmap_addr_, bloom_mmap_len_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

SSTableReader::SSTableReader(SSTableReader&& other) noexcept
    : path_(std::move(other.path_)),
      fd_(other.fd_),
      footer_(other.footer_),
      index_(std::move(other.index_)),
      bloom_mmap_addr_(other.bloom_mmap_addr_),
      bloom_mmap_len_(other.bloom_mmap_len_),
      bloom_(std::move(other.bloom_)) {
    other.fd_ = -1;
    other.bloom_mmap_addr_ = nullptr;
    other.bloom_mmap_len_ = 0;
}

SSTableReader& SSTableReader::operator=(SSTableReader&& other) noexcept {
    if (this != &other) {
        if (bloom_mmap_addr_ && bloom_mmap_addr_ != MAP_FAILED) {
            ::munmap(bloom_mmap_addr_, bloom_mmap_len_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }

        path_ = std::move(other.path_);
        fd_ = other.fd_;
        footer_ = other.footer_;
        index_ = std::move(other.index_);
        bloom_mmap_addr_ = other.bloom_mmap_addr_;
        bloom_mmap_len_ = other.bloom_mmap_len_;
        bloom_ = std::move(other.bloom_);

        other.fd_ = -1;
        other.bloom_mmap_addr_ = nullptr;
        other.bloom_mmap_len_ = 0;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Footer
// ---------------------------------------------------------------------------

void SSTableReader::readFooter() {
    struct stat st{};
    if (::fstat(fd_, &st) < 0) {
        throw std::runtime_error("SSTableReader::fstat failed");
    }

    off_t file_size = st.st_size;
    if (static_cast<size_t>(file_size) < SSTableFooter::SIZE) {
        throw std::runtime_error("SSTableReader: file too small for footer");
    }

    off_t footer_offset = file_size - static_cast<off_t>(SSTableFooter::SIZE);
    if (!preadExact(fd_, &footer_, SSTableFooter::SIZE, footer_offset)) {
        throw std::runtime_error("SSTableReader: failed to read footer");
    }
}

// ---------------------------------------------------------------------------
// Index
// ---------------------------------------------------------------------------

void SSTableReader::readIndex() {
    // Index block spans [footer_.index_offset, footer_.bloom_offset).
    uint64_t index_size = footer_.bloom_offset - footer_.index_offset;
    if (index_size == 0) return;

    std::vector<char> buf(index_size);
    if (!preadExact(fd_, buf.data(), index_size,
                    static_cast<off_t>(footer_.index_offset))) {
        throw std::runtime_error("SSTableReader: failed to read index block");
    }

    // Parse: [KeySize(4B)][Key][Offset(8B)] ...
    const char* p = buf.data();
    const char* end = p + index_size;

    while (p + sizeof(uint32_t) <= end) {
        uint32_t key_size;
        std::memcpy(&key_size, p, sizeof(key_size));
        p += sizeof(key_size);

        if (p + key_size + sizeof(uint64_t) > end) break;

        std::string key(p, key_size);
        p += key_size;

        uint64_t offset;
        std::memcpy(&offset, p, sizeof(offset));
        p += sizeof(offset);

        index_.push_back({std::move(key), offset});
    }
}

// ---------------------------------------------------------------------------
// Bloom filter (mmap)
// ---------------------------------------------------------------------------

void SSTableReader::mmapBloom() {
    if (footer_.bloom_data_size == 0) return;

    // mmap must be page-aligned. We map the page-aligned region that covers
    // the bloom data and compute the internal offset.
    long page_size = ::sysconf(_SC_PAGESIZE);
    off_t bloom_file_offset = static_cast<off_t>(footer_.bloom_offset);
    off_t page_aligned_offset = (bloom_file_offset / page_size) * page_size;
    size_t offset_within_page = static_cast<size_t>(bloom_file_offset - page_aligned_offset);

    bloom_mmap_len_ = offset_within_page + footer_.bloom_data_size;

    bloom_mmap_addr_ = ::mmap(nullptr, bloom_mmap_len_, PROT_READ,
                              MAP_PRIVATE, fd_, page_aligned_offset);
    if (bloom_mmap_addr_ == MAP_FAILED) {
        throw std::runtime_error("SSTableReader::mmap bloom failed: " +
                                 std::string(std::strerror(errno)));
    }

    // Construct BloomFilter from the mmap'd data.
    auto* bloom_data = static_cast<uint8_t*>(bloom_mmap_addr_) + offset_within_page;
    bloom_ = std::make_unique<BloomFilter>(bloom_data, footer_.bloom_data_size,
                                           footer_.bloom_num_hashes,
                                           footer_.bloom_num_bits);
}

// ---------------------------------------------------------------------------
// Data entry read
// ---------------------------------------------------------------------------

std::pair<std::string, std::string> SSTableReader::readDataEntry(uint64_t offset) const {
    // Read: [KeySize(4B)][Key][ValueSize(4B)][Value]
    uint32_t key_size;
    if (!preadExact(fd_, &key_size, sizeof(key_size), static_cast<off_t>(offset))) {
        throw std::runtime_error("SSTableReader: failed to read key_size");
    }
    offset += sizeof(key_size);

    std::string key(key_size, '\0');
    if (!preadExact(fd_, key.data(), key_size, static_cast<off_t>(offset))) {
        throw std::runtime_error("SSTableReader: failed to read key");
    }
    offset += key_size;

    uint32_t val_size;
    if (!preadExact(fd_, &val_size, sizeof(val_size), static_cast<off_t>(offset))) {
        throw std::runtime_error("SSTableReader: failed to read val_size");
    }
    offset += sizeof(val_size);

    std::string value(val_size, '\0');
    if (val_size > 0 && !preadExact(fd_, value.data(), val_size, static_cast<off_t>(offset))) {
        throw std::runtime_error("SSTableReader: failed to read value");
    }

    return {std::move(key), std::move(value)};
}

// ---------------------------------------------------------------------------
// Get (point lookup)
// ---------------------------------------------------------------------------

std::optional<std::string> SSTableReader::get(const std::string& key) const {
    // 1. Bloom filter check — if filter says "no", skip disk entirely.
    if (bloom_ && !bloom_->mayContain(key)) {
        return std::nullopt;
    }

    // 2. Binary search the index for the key.
    auto it = std::lower_bound(index_.begin(), index_.end(), key,
        [](const IndexEntry& entry, const std::string& target) {
            return entry.key < target;
        });

    if (it == index_.end() || it->key != key) {
        return std::nullopt; // Key not in index (bloom false positive).
    }

    // 3. Read the data entry at the index offset.
    auto [found_key, value] = readDataEntry(it->offset);
    return value;
}

// ---------------------------------------------------------------------------
// Get all entries (for compaction)
// ---------------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> SSTableReader::getAllEntries() const {
    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(index_.size());

    for (const auto& ie : index_) {
        entries.push_back(readDataEntry(ie.offset));
    }

    return entries;
}

} // namespace lsm
