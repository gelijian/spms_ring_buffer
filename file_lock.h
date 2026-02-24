#ifndef FILE_LOCK_H_
#define FILE_LOCK_H_

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace spms_ring_buffer {

class FileLock {
 public:
  explicit FileLock(const std::string& lock_path) {
    std::filesystem::path lp(lock_path);
    if (lp.is_absolute()) {
      lock_path_ = lp;
    } else {
      lock_path_ = std::filesystem::path{"/dev/shm"} / lock_path;
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

    if (fcntl(fd_, F_SETLK, &fl) < 0) {
      if (errno == EACCES || errno == EAGAIN) {
        throw std::runtime_error("Failed to acquire lock: already held by another process");
      }
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

}  // namespace spms_ring_buffer

#endif  // FILE_LOCK_H_
