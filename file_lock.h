#ifndef FILE_LOCK_H_
#define FILE_LOCK_H_

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <string>

namespace spms_ring_buffer {

class FileLock {
 public:
  explicit FileLock(const std::string& lock_path) {
    std::string full_path;
    if (lock_path.front() == '/') {
      full_path = lock_path;
    } else {
      full_path = "/dev/shm/" + lock_path;
    }

    fd_ = open(full_path.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd_ < 0) {
      throw std::runtime_error("Failed to open lock file: " + full_path +
                               ", errno=" + std::to_string(errno));
    }

    struct flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(fd_, F_SETLK, &fl) < 0) {
      close(fd_);
      fd_ = -1;
      if (errno == EACCES || errno == EAGAIN) {
        throw std::runtime_error("Failed to acquire lock: file is locked by another process");
      }
      throw std::runtime_error("Failed to lock file: " + full_path +
                               ", errno=" + std::to_string(errno));
    }
  }

  ~FileLock() {
    if (fd_ >= 0) {
      struct flock fl{};
      fl.l_type = F_UNLCK;
      fl.l_whence = SEEK_SET;
      fl.l_start = 0;
      fl.l_len = 0;
      fcntl(fd_, F_SETLK, &fl);
      close(fd_);
    }
  }

  void Lock() {
    struct flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(fd_, F_SETLKW, &fl) < 0) {
      throw std::runtime_error("Failed to acquire lock: errno=" + std::to_string(errno));
    }
  }

  void Unlock() {
    struct flock fl{};
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fcntl(fd_, F_SETLK, &fl);
  }

  FileLock(const FileLock&) = delete;

  FileLock& operator=(const FileLock&) = delete;

 private:
  int fd_ = -1;
};

}  // namespace spms_ring_buffer

#endif  // FILE_LOCK_H_
