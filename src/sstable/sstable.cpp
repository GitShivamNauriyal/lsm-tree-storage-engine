#include "sstable.h"

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
// SSTableWriter
// ---------------------------------------------------------------------------

size_t SSTableWriter::write(
        const std::string& path,
        const std::vector<std::pair<std::string, std::string>>& entries) {

    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::runtime_error("SSTableWriter::open failed: " +
                                 std::string(std::strerror(errno)));
    }

    // Track current file offset for index building.
    uint64_t current_offset = 0;

    // Index entries to write after data block.
    std::vector<IndexEntry> index;
    index.reserve(entries.size());

    // Bloom filter: size for number of entries.
    BloomFilter bloom(entries.empty() ? 1 : entries.size(), 0.01);

    // ---- Data Block ----
    for (const auto& [key, value] : entries) {
        // Record the offset for the index.
        index.push_back({key, current_offset});

        // Add to bloom filter.
        bloom.add(key);

        // Write: [KeySize(4B)][Key][ValueSize(4B)][Value]
        uint32_t key_size = static_cast<uint32_t>(key.size());
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

        ssize_t written = ::writev(fd, iov, iovcnt);
        if (written < 0) {
            ::close(fd);
            throw std::runtime_error("SSTableWriter::writev data failed");
        }

        current_offset += sizeof(key_size) + key.size() + sizeof(val_size) + value.size();
    }

    // ---- Index Block ----
    uint64_t index_offset = current_offset;

    for (const auto& ie : index) {
        uint32_t key_size = static_cast<uint32_t>(ie.key.size());
        uint64_t offset = ie.offset;

        struct iovec iov[3];
        iov[0].iov_base = &key_size;
        iov[0].iov_len  = sizeof(key_size);

        iov[1].iov_base = const_cast<char*>(ie.key.data());
        iov[1].iov_len  = ie.key.size();

        iov[2].iov_base = &offset;
        iov[2].iov_len  = sizeof(offset);

        ssize_t written = ::writev(fd, iov, 3);
        if (written < 0) {
            ::close(fd);
            throw std::runtime_error("SSTableWriter::writev index failed");
        }

        current_offset += sizeof(key_size) + ie.key.size() + sizeof(offset);
    }

    // ---- Bloom Filter Block ----
    uint64_t bloom_offset = current_offset;
    auto bloom_data = bloom.serialize();

    if (!bloom_data.empty()) {
        ssize_t written = ::write(fd, bloom_data.data(), bloom_data.size());
        if (written < 0) {
            ::close(fd);
            throw std::runtime_error("SSTableWriter::write bloom failed");
        }
        current_offset += bloom_data.size();
    }

    // ---- Footer (24 bytes) ----
    SSTableFooter footer{};
    footer.index_offset     = index_offset;
    footer.bloom_offset     = bloom_offset;
    footer.bloom_num_hashes = static_cast<uint32_t>(bloom.numHashes());
    footer.bloom_num_bits   = static_cast<uint32_t>(bloom.numBits());
    footer.bloom_data_size  = static_cast<uint32_t>(bloom_data.size());

    ssize_t written = ::write(fd, &footer, SSTableFooter::SIZE);
    if (written < 0 || static_cast<size_t>(written) != SSTableFooter::SIZE) {
        ::close(fd);
        throw std::runtime_error("SSTableWriter::write footer failed");
    }

    ::fsync(fd);
    ::close(fd);

    return entries.size();
}

} // namespace lsm
