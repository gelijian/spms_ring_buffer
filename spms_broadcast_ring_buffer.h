#ifndef SPMS_BROADCAST_RING_BUFFER_H_
#define SPMS_BROADCAST_RING_BUFFER_H_

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>

namespace spms_ring_buffer {

constexpr uint32_t kFrameHeaderMagic = 0x42524246;
constexpr size_t kCacheLineSize = 64;

constexpr uint64_t kShmMagic = 0x53504D5342425253ULL;

#pragma pack(push, 1)
struct FrameHeader {
  enum class Type : uint8_t {
    kMessage = 1,
    kPadding = 2,
  };

  uint64_t data_offset = 0;
  uint32_t frame_len = 0;
  uint32_t payload_len = 0;
  uint32_t magic = kFrameHeaderMagic;
  Type frame_type = Type::kMessage;
  std::array<uint8_t, 11> reserved{};
};
static_assert(sizeof(FrameHeader) == 32);
#pragma pack(pop)

struct alignas(kCacheLineSize) ShmHeader {
  uint64_t magic = 0;
  uint64_t data_capacity = 0;
  std::atomic<uint64_t> publish_offset{0};
};

class OverrunException : public std::runtime_error {
 public:
  OverrunException() : std::runtime_error("Subscriber read position overrun by publisher") {}
};

class BufferTooSmallException : public std::runtime_error {
 public:
  BufferTooSmallException() : std::runtime_error("Destination buffer too small") {}
};

class FileLock {
 public:
  explicit FileLock(const std::string& lock_path) {
    std::filesystem::path lp(lock_path);
    if (lp.is_absolute()) {
      lock_path_ = lp;
    } else {
      lock_path_ = std::filesystem::path{"/dev/shm"} / lp;
    }
    fd_ = open(lock_path_.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd_ < 0) {
      throw std::runtime_error("Failed to open lock file: " + lock_path_ + ", errno=" + std::to_string(errno));
    }
  }

  ~FileLock() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  void Lock() {
    struct flock fl{};
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(fd_, F_SETLKW, &fl) < 0) {
      throw std::runtime_error("Failed to lock file: " + lock_path_ + ", errno=" + std::to_string(errno));
    }
  }

  void Unlock() {
    struct flock fl{};
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(fd_, F_SETLK, &fl) < 0) {
      throw std::runtime_error("Failed to unlock file: " + lock_path_ + ", errno=" + std::to_string(errno));
    }
  }

 private:
  std::string lock_path_;
  int fd_ = -1;
};

enum class Mode { ReadWrite, ReadOnly };

class ShmManager {
 public:
  ShmManager() = default;
  ~ShmManager() { Detach(); }

  explicit ShmManager(const std::string& name, Mode mode, uint64_t capacity = 0) { Open(name, mode, capacity); }

  ShmManager(const ShmManager&) = delete;
  ShmManager& operator=(const ShmManager&) = delete;

  void Open(const std::string& name, Mode mode, uint64_t capacity = 0) {
    Detach();

    name_ = "/dev/shm/" + name;
    mode_ = mode;

    int open_flags = (mode == Mode::ReadWrite) ? O_RDWR : O_RDONLY;
    int prot = (mode == Mode::ReadWrite) ? (PROT_READ | PROT_WRITE) : PROT_READ;

    fd_ = open(name_.c_str(), open_flags, 0666);
    if (fd_ < 0) {
      if (errno == ENOENT && mode == Mode::ReadWrite && capacity > 0) {
        fd_ = open(name_.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) {
          throw std::runtime_error("Failed to create file: " + name_ + ", errno=" + std::to_string(errno));
        }
      } else {
        throw std::runtime_error("Failed to open file: " + name_ + ", errno=" + std::to_string(errno));
      }
    }

    struct stat stat_buf{};
    if (fstat(fd_, &stat_buf) < 0) {
      close(fd_);
      throw std::runtime_error("Failed to stat file: " + name_ + ", errno=" + std::to_string(errno));
    }

    bool is_new = stat_buf.st_size == 0;
    size_ = is_new ? (sizeof(ShmHeader) + capacity) : stat_buf.st_size;

    if (is_new) {
      if (ftruncate(fd_, size_) < 0) {
        close(fd_);
        if (mode == Mode::ReadWrite) {
          unlink(name_.c_str());
        }
        throw std::runtime_error("Failed to truncate file: " + name_ + ", errno=" + std::to_string(errno));
      }
    }

    addr_ = mmap(nullptr, size_, prot, MAP_SHARED, fd_, 0);
    if (addr_ == MAP_FAILED) {
      close(fd_);
      if (is_new && mode == Mode::ReadWrite) {
        unlink(name_.c_str());
      }
      throw std::runtime_error("Failed to mmap: " + name_ + ", errno=" + std::to_string(errno));
    }

    if (is_new) {
      if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
        throw std::runtime_error("Capacity must be a non-zero power of two");
      }
      memset(addr_, 0, size_);
      ShmHeader* header = GetHeader();
      header->magic = kShmMagic;
      header->data_capacity = capacity;
      header->publish_offset.store(0, std::memory_order_release);
    } else {
      ShmHeader* header = GetHeader();
      if (header->magic != kShmMagic) {
        munmap(addr_, size_);
        close(fd_);
        throw std::runtime_error("Invalid shm magic");
      }
      if (mode == Mode::ReadWrite && capacity > 0 && header->data_capacity != capacity) {
        munmap(addr_, size_);
        close(fd_);
        throw std::runtime_error("Capacity mismatch: expected " + std::to_string(capacity) + ", got " +
                                 std::to_string(header->data_capacity));
      }
    }
  }

  void Detach() {
    if (addr_ != nullptr && addr_ != MAP_FAILED) {
      munmap(addr_, size_);
      addr_ = nullptr;
    }
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

  [[nodiscard]] void* GetDataStart() const { return static_cast<char*>(addr_) + sizeof(ShmHeader); }

  [[nodiscard]] ShmHeader* GetHeader() const { return static_cast<ShmHeader*>(addr_); }

  [[nodiscard]] uint64_t GetCapacity() const { return GetHeader()->data_capacity; }

 private:
  void* addr_ = nullptr;
  int fd_ = -1;
  uint64_t size_ = 0;
  std::string name_;
  Mode mode_ = Mode::ReadWrite;
};

class Publisher {
 public:
  explicit Publisher(const std::string& shm_name, uint64_t capacity = 0)
      : shm_(shm_name, Mode::ReadWrite, capacity), lock_(shm_name + ".lock") {
    lock_.Lock();
  }

  ~Publisher() { lock_.Unlock(); }

  Publisher(const Publisher&) = delete;
  Publisher& operator=(const Publisher&) = delete;

  void Publish(const void* payload, uint32_t length) {
    ShmHeader* header = shm_.GetHeader();
    uint64_t data_capacity = header->data_capacity;
    void* data_start = shm_.GetDataStart();

    uint32_t rounded_payload_len = (length + 7) & ~7;
    uint64_t total_frame_size = sizeof(FrameHeader) + rounded_payload_len;

    uint64_t current_offset = header->publish_offset.load(std::memory_order_acquire);
    uint64_t remaining_space = data_capacity - (current_offset & (data_capacity - 1));

    if (rounded_payload_len + sizeof(FrameHeader) + sizeof(FrameHeader) > remaining_space) {
      uint32_t padding_len = (remaining_space + 7) & ~7;
      WritePaddingFrame(current_offset, padding_len, data_start, data_capacity);
      current_offset = header->publish_offset.load(std::memory_order_acquire);
    }

    WriteMessageFrame(current_offset, payload, length, rounded_payload_len, data_start, data_capacity, header);
  }

 private:
  void WritePaddingFrame(uint64_t offset, uint32_t padding_len, void* data_start, uint64_t data_capacity) {
    uint64_t masked_offset = offset & (data_capacity - 1);
    char* data_ptr = static_cast<char*>(data_start) + masked_offset;

    auto* header = static_cast<FrameHeader*>(static_cast<void*>(data_ptr));
    header->data_offset = offset;
    header->frame_len = padding_len;
    header->payload_len = 0;
    header->magic = kFrameHeaderMagic;
    header->frame_type = FrameHeader::Type::kPadding;
    memset(header->reserved.data(), 0, header->reserved.size());

    uint64_t total_written = sizeof(FrameHeader) + padding_len;
    auto* shm_header = reinterpret_cast<ShmHeader*>(static_cast<char*>(data_start) - sizeof(ShmHeader));
    uint64_t new_offset = offset + total_written;
    shm_header->publish_offset.store(new_offset, std::memory_order_release);
  }

  void WriteMessageFrame(uint64_t offset, const void* payload, uint32_t length, uint32_t frame_len, void* data_start,
                         uint64_t data_capacity, ShmHeader* header) {
    uint64_t masked_offset = offset & (data_capacity - 1);
    char* data_ptr = static_cast<char*>(data_start) + masked_offset;

    auto* frame_header = static_cast<FrameHeader*>(static_cast<void*>(data_ptr));
    frame_header->data_offset = offset;
    frame_header->frame_len = frame_len;
    frame_header->payload_len = length;
    frame_header->magic = kFrameHeaderMagic;
    frame_header->frame_type = FrameHeader::Type::kMessage;
    memset(frame_header->reserved.data(), 0, frame_header->reserved.size());

    char* payload_ptr = data_ptr + sizeof(FrameHeader);
    memcpy(payload_ptr, payload, length);

    if (frame_len > length) {
      memset(payload_ptr + length, 0, frame_len - length);
    }

    uint64_t total_written = sizeof(FrameHeader) + frame_len;
    uint64_t new_offset = offset + total_written;
    header->publish_offset.store(new_offset, std::memory_order_release);
  }

  ShmManager shm_;
  FileLock lock_;
};

class Subscriber {
 public:
  explicit Subscriber(const std::string& shm_name) : shm_(shm_name, Mode::ReadOnly, 0) {
    ShmHeader* header = shm_.GetHeader();
    cache_publish_offset_ = header->publish_offset.load(std::memory_order_acquire);
    subscribe_offset_ = cache_publish_offset_;
  }

  ~Subscriber() = default;

  Subscriber(const Subscriber&) = delete;
  Subscriber& operator=(const Subscriber&) = delete;

  [[nodiscard]] std::span<const char> TryRead() {
    ShmHeader* header = shm_.GetHeader();
    uint64_t data_capacity = header->data_capacity;
    void* data_start = shm_.GetDataStart();

    if (subscribe_offset_ == cache_publish_offset_) {
      cache_publish_offset_ = header->publish_offset.load(std::memory_order_acquire);
    }

    if (subscribe_offset_ == cache_publish_offset_) {
      return {};
    }

    if (cache_publish_offset_ - subscribe_offset_ > data_capacity) {
      subscribe_offset_ = cache_publish_offset_;
      return {};
    }

    uint64_t masked_offset = subscribe_offset_ & (data_capacity - 1);
    char* data_ptr = static_cast<char*>(data_start) + masked_offset;

    auto* frame_header = static_cast<FrameHeader*>(static_cast<void*>(data_ptr));

    if (frame_header->magic != kFrameHeaderMagic) {
      subscribe_offset_ = cache_publish_offset_;
      return {};
    }

    uint32_t total_len = sizeof(FrameHeader) + frame_header->frame_len;
    subscribe_offset_ += total_len;

    if (frame_header->frame_type == FrameHeader::Type::kPadding) {
      return {};
    }

    char* payload_ptr = data_ptr + sizeof(FrameHeader);
    return {payload_ptr, static_cast<size_t>(frame_header->payload_len)};
  }

 private:
  ShmManager shm_;
  uint64_t subscribe_offset_ = 0;
  uint64_t cache_publish_offset_ = 0;
};

}  // namespace spms_ring_buffer

#endif  // SPMS_BROADCAST_RING_BUFFER_H_
