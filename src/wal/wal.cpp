#include "wal.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>

namespace lsm {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

WAL::WAL(const std::string& path) : path_(path), fd_(-1) {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("WAL::open failed: " + std::string(std::strerror(errno)));
    }
}

WAL::~WAL() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

// ---------------------------------------------------------------------------
// Write helpers
// ---------------------------------------------------------------------------

/// Writes a single record in the binary format:
///   [KeySize (4B)][Key][ValueSize (4B)][Value]
/// For tombstones, MSB of KeySize is set and Value is empty.
/// Uses writev() for atomic scatter-gather I/O.
void WAL::writeRecord(const std::string& key, const std::string& value, bool tombstone) {
    uint32_t key_size = static_cast<uint32_t>(key.size());
    if (tombstone) {
        key_size |= TOMBSTONE_MASK;
    }
    uint32_t val_size = static_cast<uint32_t>(value.size());

    struct iovec iov[4];
    int iovcnt = 0;

    iov[iovcnt].iov_base = &key_size;
    iov[iovcnt].iov_len  = sizeof(key_size);
    iovcnt++;

    iov[iovcnt].iov_base = const_cast<char*>(key.data());
    iov[iovcnt].iov_len  = key.size();
    iovcnt++;

    iov[iovcnt].iov_base = &val_size;
    iov[iovcnt].iov_len  = sizeof(val_size);
    iovcnt++;

    if (val_size > 0) {
        iov[iovcnt].iov_base = const_cast<char*>(value.data());
        iov[iovcnt].iov_len  = value.size();
        iovcnt++;
    }

    ssize_t written = ::writev(fd_, iov, iovcnt);
    if (written < 0) {
        throw std::runtime_error("WAL::writev failed: " + std::string(std::strerror(errno)));
    }

    // Durability: force to disk.
    if (::fsync(fd_) < 0) {
        throw std::runtime_error("WAL::fsync failed: " + std::string(std::strerror(errno)));
    }
}

void WAL::append(const std::string& key, const std::string& value) {
    writeRecord(key, value, /*tombstone=*/false);
}

void WAL::appendTombstone(const std::string& key) {
    writeRecord(key, "", /*tombstone=*/true);
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

/// Helper: read exactly `count` bytes from fd at current position.
/// Returns false on short read / EOF (indicating truncated record).
static bool readExact(int fd, void* buf, size_t count) {
    size_t total_read = 0;
    auto* p = static_cast<char*>(buf);
    while (total_read < count) {
        ssize_t n = ::read(fd, p + total_read, count - total_read);
        if (n <= 0) {
            return false; // EOF or error
        }
        total_read += static_cast<size_t>(n);
    }
    return true;
}

std::vector<WALRecord> WAL::recover() {
    // Open a separate read-only fd so we can seek from byte 0
    // without disturbing the append-mode write fd.
    int rfd = ::open(path_.c_str(), O_RDONLY);
    if (rfd < 0) {
        throw std::runtime_error("WAL::recover open failed: " + std::string(std::strerror(errno)));
    }

    std::vector<WALRecord> records;

    while (true) {
        // Read key_size (4 bytes).
        uint32_t raw_key_size = 0;
        if (!readExact(rfd, &raw_key_size, sizeof(raw_key_size))) {
            break; // Clean EOF or truncated — done.
        }

        bool tombstone = (raw_key_size & TOMBSTONE_MASK) != 0;
        uint32_t key_size = raw_key_size & ~TOMBSTONE_MASK;

        // Read key.
        std::string key(key_size, '\0');
        if (!readExact(rfd, key.data(), key_size)) {
            break; // Truncated record — discard.
        }

        // Read value_size (4 bytes).
        uint32_t val_size = 0;
        if (!readExact(rfd, &val_size, sizeof(val_size))) {
            break;
        }

        // Read value.
        std::string value(val_size, '\0');
        if (val_size > 0 && !readExact(rfd, value.data(), val_size)) {
            break;
        }

        records.push_back({std::move(key), std::move(value), tombstone});
    }

    ::close(rfd);
    return records;
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

void WAL::clear() {
    // Truncate the file to zero.
    if (::ftruncate(fd_, 0) < 0) {
        throw std::runtime_error("WAL::clear ftruncate failed: " + std::string(std::strerror(errno)));
    }
    // Reset the file offset (O_APPEND will still seek to end on next write).
    ::lseek(fd_, 0, SEEK_SET);
}

} // namespace lsm
