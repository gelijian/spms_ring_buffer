#ifndef SHARED_MEMORY_H_
#define SHARED_MEMORY_H_

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>

namespace spms_ring_buffer {

constexpr uint64_t kShmMagic = 0x53504D5342425253ULL;

struct SpmsRingBufferControlBlock {
  uint64_t magic = 0;
  uint64_t data_capacity = 0;
  std::atomic<uint64_t> publish_offset{0};

  [[nodiscard]] uint64_t MaskOffset(uint64_t logical_offset) const { return logical_offset & (data_capacity - 1); }
};

enum class Mode { ReadWrite, ReadOnly };

class SharedMemory {
 public:
  SharedMemory() = default;
  ~SharedMemory() { Detach(); }

  explicit SharedMemory(const std::string& name, Mode mode, uint64_t capacity = 0) { Open(name, mode, capacity); }

  SharedMemory(const SharedMemory&) = delete;
  SharedMemory& operator=(const SharedMemory&) = delete;

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
    size_ = is_new ? (sizeof(SpmsRingBufferControlBlock) + capacity) : stat_buf.st_size;

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
      auto* cb = static_cast<SpmsRingBufferControlBlock*>(addr_);
      cb->magic = kShmMagic;
      cb->data_capacity = capacity;
      cb->publish_offset.store(0, std::memory_order_release);
    } else {
      auto* cb = static_cast<SpmsRingBufferControlBlock*>(addr_);
      if (cb->magic != kShmMagic) {
        munmap(addr_, size_);
        close(fd_);
        throw std::runtime_error("Invalid shm magic");
      }
      if (mode == Mode::ReadWrite && capacity > 0 && cb->data_capacity != capacity) {
        munmap(addr_, size_);
        close(fd_);
        throw std::runtime_error("Capacity mismatch: expected " + std::to_string(capacity) + ", got " +
                                 std::to_string(cb->data_capacity));
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

  [[nodiscard]] void* GetDataStart() const {
    return static_cast<char*>(addr_) + sizeof(SpmsRingBufferControlBlock);
  }

  [[nodiscard]] uint64_t GetCapacity() const {
    auto* cb = static_cast<SpmsRingBufferControlBlock*>(addr_);
    return cb->data_capacity;
  }

  [[nodiscard]] void* GetBaseAddr() const { return addr_; }

 private:
  void* addr_ = nullptr;
  int fd_ = -1;
  uint64_t size_ = 0;
  std::string name_;
  Mode mode_ = Mode::ReadWrite;
};

}  // namespace spms_ring_buffer

#endif  // SHARED_MEMORY_H_
