#ifndef SHARED_MEMORY_H_
#define SHARED_MEMORY_H_

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <string>

namespace spms_ring_buffer {

enum class Mode { ReadWrite, ReadOnly };

class SharedMemory {
 public:
  SharedMemory() = default;
  ~SharedMemory() { Detach(); }

  explicit SharedMemory(const std::string& name, Mode mode, uint64_t control_block_size, uint64_t data_capacity) {
    Open(name, mode, control_block_size, data_capacity);
  }

  SharedMemory(const SharedMemory&) = delete;
  SharedMemory& operator=(const SharedMemory&) = delete;

  void Open(const std::string& name, Mode mode, uint64_t control_block_size, uint64_t data_capacity) {
    Detach();

    name_ = "/dev/shm/" + name;
    mode_ = mode;
    control_block_size_ = control_block_size;

    int open_flags = (mode == Mode::ReadWrite) ? O_RDWR : O_RDONLY;
    int prot = (mode == Mode::ReadWrite) ? (PROT_READ | PROT_WRITE) : PROT_READ;

    fd_ = open(name_.c_str(), open_flags, 0666);
    if (fd_ < 0) {
      if (errno == ENOENT && mode == Mode::ReadWrite && data_capacity > 0) {
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
    size_ = is_new ? (control_block_size + data_capacity) : stat_buf.st_size;

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
      if (data_capacity == 0 || (data_capacity & (data_capacity - 1)) != 0) {
        throw std::runtime_error("Data capacity must be a non-zero power of two");
      }
      memset(addr_, 0, size_);
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
    return static_cast<char*>(addr_) + control_block_size_;
  }

  [[nodiscard]] void* GetBaseAddr() const { return addr_; }

  [[nodiscard]] uint64_t GetSize() const { return size_; }

 private:
  void* addr_ = nullptr;
  int fd_ = -1;
  uint64_t size_ = 0;
  uint64_t control_block_size_ = 0;
  std::string name_;
  Mode mode_ = Mode::ReadWrite;
};

}  // namespace spms_ring_buffer

#endif  // SHARED_MEMORY_H_
