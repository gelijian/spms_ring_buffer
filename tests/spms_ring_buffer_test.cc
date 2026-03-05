#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <sys/wait.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "file_lock.h"
#include "shared_memory.h"
#include "spms_ring_buffer.h"

using namespace spms_ring_buffer;

namespace spms_ring_buffer {

class ShmCleanupFixture {
 public:
  explicit ShmCleanupFixture(const std::string& shm_name) : shm_name_(shm_name) {}

  ~ShmCleanupFixture() {
    std::string shm_path = std::string("/dev/shm/") + shm_name_;
    unlink(shm_path.c_str());
    std::string lock_path = std::string("/dev/shm/") + shm_name_ + ".lock";
    unlink(lock_path.c_str());
  }

  ShmCleanupFixture(const ShmCleanupFixture&) = delete;
  ShmCleanupFixture& operator=(const ShmCleanupFixture&) = delete;

  const std::string& shm_name() const { return shm_name_; }

 private:
  std::string shm_name_;
};

static int test_counter = 0;

}  // namespace spms_ring_buffer

TEST_CASE("test_shared_memory_create") {
  std::string shm_name = "test_shm_util_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);

  SharedMemory shm(shm_name, SharedMemory::Mode::ReadWrite, 4096);

  CHECK(shm.IsCreated() == true);
  CHECK(shm.GetSize() >= 4096);
}

TEST_CASE("test_shared_memory_open_existing") {
  std::string shm_name = "test_shm_util_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);

  // Create new shared memory
  SharedMemory shm1(shm_name, SharedMemory::Mode::ReadWrite, 4096);
  CHECK(shm1.IsCreated() == true);

  // Open existing shared memory
  SharedMemory shm2(shm_name, SharedMemory::Mode::ReadWrite, 4096);
  CHECK(shm2.IsCreated() == false);
}

TEST_CASE("test_shared_memory_read_write") {
  std::string shm_name = "test_shm_util_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);

  SharedMemory shm(shm_name, SharedMemory::Mode::ReadWrite, 4096);

  // Write data
  const char* write_data = "Hello Shared Memory";
  std::memcpy(shm.GetBaseAddr(), write_data, std::strlen(write_data) + 1);

  // Read back and verify
  const char* read_data = static_cast<const char*>(shm.GetBaseAddr());
  CHECK_EQ(std::strcmp(read_data, write_data), 0);
}

TEST_CASE("test_shared_memory_read_only_mode") {
  std::string shm_name = "test_shm_util_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);

  // Create RW shared memory first
  SharedMemory shm_rw(shm_name, SharedMemory::Mode::ReadWrite, 4096);
  const char* write_data = "test data";
  std::memcpy(shm_rw.GetBaseAddr(), write_data, std::strlen(write_data) + 1);

  // Open ReadOnly
  SharedMemory shm_ro(shm_name, SharedMemory::Mode::ReadOnly, 4096);

  CHECK(shm_ro.GetBaseAddr() != nullptr);
  CHECK(shm_ro.GetSize() >= 4096);
}

TEST_CASE("test_shared_memory_get_size") {
  std::string shm_name = "test_shm_util_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);

  const uint64_t kTestSize = 8192;
  SharedMemory shm(shm_name, SharedMemory::Mode::ReadWrite, kTestSize);

  CHECK(shm.GetSize() >= kTestSize);
}

TEST_CASE("test_publish_single_message") {
  std::string shm_name = "test_shm_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);

  Publisher publisher(shm_name, 1024);
  std::string payload = "test message";
  std::span<const char> payload_span(payload);
  FrameHeader header = publisher.Publish(payload_span);

  CHECK(header.logical_offset >= 0);
  CHECK(header.frame_type == FrameHeader::Type::kMessage);
  CHECK(header.payload_len == payload.size());
}

TEST_CASE("test_subscriber_read_message") {
  std::string shm_name = "test_shm_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);

  Publisher publisher(shm_name, 1024);
  Subscriber subscriber(shm_name);
  
  std::string payload = "hello";
  (void)publisher.Publish(std::span<const char>(payload));

  std::this_thread::sleep_for(std::chrono::microseconds(100));

  auto result = subscriber.TryRead();

  CHECK(result.header.frame_type == FrameHeader::Type::kMessage);
  CHECK(result.header.payload_len == payload.size());
  CHECK(result.payload.size() == payload.size());
}

TEST_CASE("test_publish_multiple_messages") {
  std::string shm_name = "test_shm_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);

  Publisher publisher(shm_name, 512);

  for (int i = 0; i < 5; ++i) {
    std::string payload = "msg" + std::to_string(i);
    (void)publisher.Publish(std::span<const char>(payload));
  }

  Subscriber subscriber(shm_name);
  for (int i = 0; i < 5; ++i) {
    auto result = subscriber.TryRead();
    CHECK(result.header.frame_type == FrameHeader::Type::kMessage);
  }
}

TEST_CASE("test_payload_integrity") {
  std::string shm_name = "test_shm_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);

  Publisher publisher(shm_name, 1024);
  Subscriber subscriber(shm_name);
  
  std::string original_payload = "Hello, World!";
  (void)publisher.Publish(std::span<const char>(original_payload));

  std::this_thread::sleep_for(std::chrono::microseconds(100));

  auto result = subscriber.TryRead();

  CHECK_EQ(result.payload.size(), original_payload.size());
  std::string received_payload(result.payload.begin(), result.payload.end());
  CHECK_EQ(received_payload, original_payload);
}

TEST_CASE("test_empty_payload") {
  std::string shm_name = "test_shm_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);

  Publisher publisher(shm_name, 1024);
  Subscriber subscriber(shm_name);
  
  std::string empty_payload = "";
  (void)publisher.Publish(std::span<const char>(empty_payload));

  std::this_thread::sleep_for(std::chrono::microseconds(100));

  auto result = subscriber.TryRead();

  CHECK(result.header.frame_type == FrameHeader::Type::kMessage);
  CHECK_EQ(result.payload.size(), 0);
}

TEST_CASE("test_padding_frame_generated") {
  std::string shm_name = "test_shm_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);

  Publisher publisher(shm_name, 512);

  for (int i = 0; i < 12; i++) {
    std::string payload = "msg";
    (void)publisher.Publish(std::span<const char>(payload));
  }

  Subscriber subscriber(shm_name);

  for (int i = 0; i < 12; i++) {
    (void)subscriber.TryRead();
  }

  std::string large_payload(400, 'x');
  (void)publisher.Publish(std::span<const char>(large_payload));

  std::this_thread::sleep_for(std::chrono::microseconds(100));

  int total_frames = 0;
  for (int i = 0; i < 20; i++) {
    auto result = subscriber.TryRead();
    if (result.header.frame_type == FrameHeader::Type::kPadding) {
      total_frames++;
      break;
    }
    if (result.header.frame_type == FrameHeader::Type::kMessage && result.payload.size() > 0) {
      total_frames++;
    }
    if (result.header.frame_type == FrameHeader::Type::kMessage && result.payload.size() == 0) {
      break;
    }
  }

  CHECK(total_frames > 0);
}

TEST_CASE("test_subscriber_skips_padding") {
  std::string shm_name = "test_shm_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);

  Publisher publisher(shm_name, 256);

  for (int i = 0; i < 3; i++) {
    std::string payload = "msg";
    (void)publisher.Publish(std::span<const char>(payload));
  }

  std::string large_msg(150, 'y');
  (void)publisher.Publish(std::span<const char>(large_msg));

  Subscriber subscriber(shm_name);

  std::this_thread::sleep_for(std::chrono::microseconds(100));

  int frame_count = 0;
  for (int i = 0; i < 20; i++) {
    auto result = subscriber.TryRead();
    if (result.header.frame_type == FrameHeader::Type::kPadding) {
      frame_count++;
    }
    if (result.header.frame_type == FrameHeader::Type::kMessage) {
      frame_count++;
    }
    if (result.header.frame_type == FrameHeader::Type::kMessage && result.payload.size() == 0) {
      break;
    }
  }

  CHECK(frame_count > 0);
}

TEST_CASE("test_padding_frame_header_fields") {
  std::string shm_name = "test_shm_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);

  Publisher publisher(shm_name, 512);

  for (int i = 0; i < 8; i++) {
    std::string msg = "msg";
    (void)publisher.Publish(std::span<const char>(msg));
  }

  Subscriber subscriber(shm_name);

  for (int i = 0; i < 8; i++) {
    (void)subscriber.TryRead();
  }

  std::string large_payload(250, 'x');
  (void)publisher.Publish(std::span<const char>(large_payload));

  bool found_padding = false;
  for (int i = 0; i < 20; i++) {
    auto result = subscriber.TryRead();
    if (result.header.frame_type == FrameHeader::Type::kPadding) {
      CHECK(result.header.payload_len == 0);
      found_padding = true;
      break;
    }
    if (result.header.frame_type == FrameHeader::Type::kMessage && result.payload.size() == 0) {
      break;
    }
  }

  CHECK(found_padding);
}

TEST_CASE("test_no_padding_when_exact_fit") {
  std::string shm_name = "test_shm_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);

  Publisher publisher(shm_name, 1024);
  Subscriber subscriber(shm_name);

  std::vector<std::string> messages = {"a", "bb", "ccc", "dddd", "eeeee"};

  for (const auto& msg : messages) {
    (void)publisher.Publish(std::span<const char>(msg));
  }

  std::this_thread::sleep_for(std::chrono::microseconds(100));

  int count = 0;
  for (int i = 0; i < 20; i++) {
    auto result = subscriber.TryRead();
    if (result.header.frame_type == FrameHeader::Type::kMessage && result.payload.size() > 0) {
      count++;
    }
    CHECK_FALSE(result.header.frame_type == FrameHeader::Type::kPadding);
    if (result.header.frame_type == FrameHeader::Type::kMessage && result.payload.size() == 0) {
      break;
    }
  }

    CHECK(count == messages.size());
}

TEST_CASE("test_slow_subscriber_overrun_detection") {
    std::string shm_name = "test_shm_" + std::to_string(test_counter++);
    ShmCleanupFixture cleanup(shm_name);

    // Create Publisher with small capacity (256)
    Publisher publisher(shm_name, 256);

    // Create Subscriber first (for memory visibility)
    Subscriber subscriber(shm_name);

    // Publish 25 messages - each ~16 bytes payload + 24 bytes header = ~40 bytes
    // 25 * 40 = 1000 bytes > 256 capacity, should trigger overrun
    for (int i = 0; i < 25; ++i) {
        std::string payload = "msg_payload_16byte";  // 16 bytes
        (void)publisher.Publish(std::span<const char>(payload));
    }

    std::this_thread::sleep_for(std::chrono::microseconds(100));

    // Try to read - should throw OverrunException
    CHECK_THROWS_AS((void)subscriber.TryRead(), OverrunException);
}

TEST_CASE("test_file_lock_constructor_locks") {
  std::string lock_name = "test_lock_" + std::to_string(test_counter++);
  std::string lock_path = "/dev/shm/" + lock_name;

  // Should not throw - lock acquired
  FileLock lock(lock_name);

  // Cleanup
  unlink(lock_path.c_str());
}

TEST_CASE("test_file_lock_destructor_unlocks") {
  std::string lock_name = "test_lock_" + std::to_string(test_counter++);
  std::string lock_path = "/dev/shm/" + lock_name;

  {
    // Create and hold lock
    FileLock lock1(lock_name);
  }  // lock1 destroyed here, lock released

  // Should not throw - lock was released by destructor
  FileLock lock2(lock_name);

  // Cleanup
  unlink(lock_path.c_str());
}

TEST_CASE("test_file_lock_throws_if_locked") {
  std::string lock_name = "test_lock_" + std::to_string(test_counter++);
  std::string lock_path = "/dev/shm/" + lock_name;

  // Create first lock (A) to ensure lock is in a child process held by different process
  pid_t pid = fork();
  if (pid == 0) {
    // Child process: acquire lock and hold it
    FileLock lock_a(lock_name);
    // Sleep to keep lock held while parent tries to acquire
    sleep(2);
    _exit(0);
  }

  // Parent process: wait a moment for child to acquire lock
  usleep(100000);  // 100ms

  // Try to create second lock (B) - should throw because child holds it
  CHECK_THROWS(([&] { FileLock lock_b(lock_name); })());

  // Wait for child to finish
  int status;
  waitpid(pid, &status, 0);

  // Cleanup
  unlink(lock_path.c_str());
}

TEST_CASE("test_file_lock_manual_lock_unlock") {
  std::string lock_name = "test_lock_manual_" + std::to_string(test_counter++);
  std::string lock_path = "/dev/shm/" + lock_name;

  FileLock lock(lock_name);

  lock.Unlock();
  lock.Lock();  // Should not throw

  // Cleanup
  unlink(lock_path.c_str());
}

TEST_CASE("test_publish_string_view_zero_copy") {
  std::string shm_name = "test_shm_zc_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  Publisher publisher(shm_name, 1024);
  
  std::string payload = "zero_copy_test";
  std::string_view sv(payload);
  FrameHeader header = publisher.Publish(sv);
  
  CHECK(header.payload_len == payload.size());
  CHECK(header.frame_type == FrameHeader::Type::kMessage);
}

TEST_CASE("test_metrics_api") {
  std::string shm_name = "test_shm_metrics_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  Publisher publisher(shm_name, 1024);
  Subscriber subscriber(shm_name);  // Create subscriber BEFORE publishing
  
  auto batch = publisher.CreateBatch();
  batch.Add(std::span<const char>("test", 4));
  batch.Commit();
  
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  
  auto pub_stats = publisher.GetStats();
  CHECK(pub_stats.messages_published > 0);
  CHECK(pub_stats.publish_offset > 0);
  
  auto result = subscriber.TryRead();
  
  CHECK(result.header.frame_type == FrameHeader::Type::kMessage);
  CHECK(result.payload.size() == 4);
  
  auto sub_stats = subscriber.GetStats();
  CHECK(sub_stats.messages_read > 0);
}

TEST_CASE("test_multi_process_publisher_subscriber") {
  std::string shm_name = "test_shm_mp_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  {
    Publisher init_pub(shm_name, 1024);
  }
  
  pid_t pid = fork();
  
  if (pid == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Publisher publisher(shm_name, 1024);
    for (int i = 0; i < 10; ++i) {
      std::string msg = "msg_" + std::to_string(i);
      (void)publisher.Publish(std::span<const char>(msg));
    }
    _exit(0);
  }
  
  Subscriber subscriber(shm_name);
  
  int status;
  waitpid(pid, &status, 0);
  CHECK(WIFEXITED(status));
  
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  
  int read_count = 0;
  for (int i = 0; i < 20; ++i) {
    auto result = subscriber.TryRead();
    if (result.header.frame_type == FrameHeader::Type::kMessage && !result.payload.empty()) {
      read_count++;
    }
  }
  CHECK(read_count == 10);
}

TEST_CASE("test_publisher_crash_recovery") {
  std::string shm_name = "test_shm_recovery_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  {
    Publisher init_pub(shm_name, 1024);
  }
  
  Subscriber subscriber(shm_name);
  
  {
    Publisher publisher1(shm_name, 1024);
    (void)publisher1.Publish(std::span<const char>("msg1", 4));
    (void)publisher1.Publish(std::span<const char>("msg2", 4));
  }
  
  Publisher publisher2(shm_name, 1024);
  auto stats = publisher2.GetStats();
  CHECK(stats.publish_offset > 0);
  
  (void)publisher2.Publish(std::span<const char>("msg3", 4));
  
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  
  int count = 0;
  for (int i = 0; i < 10; ++i) {
    auto result = subscriber.TryRead();
    if (result.header.frame_type == FrameHeader::Type::kMessage && !result.payload.empty()) {
      count++;
    }
  }
  CHECK(count == 3);
}

TEST_CASE("test_subscriber_crash_recovery") {
  std::string shm_name = "test_shm_sub_recovery_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  Publisher publisher(shm_name, 1024);
  
  Subscriber subscriber(shm_name);
  
  (void)publisher.Publish(std::span<const char>("msg1", 4));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  (void)subscriber.TryRead();
  
  (void)publisher.Publish(std::span<const char>("msg2", 4));
  (void)publisher.Publish(std::span<const char>("msg3", 4));
  
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  
  int count = 0;
  for (int i = 0; i < 10; ++i) {
    auto result = subscriber.TryRead();
    if (result.header.frame_type == FrameHeader::Type::kMessage && !result.payload.empty()) {
      count++;
    }
  }
  CHECK(count == 2);
}

TEST_CASE("test_max_payload_size") {
  std::string shm_name = "test_shm_max_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  Publisher publisher(shm_name, 1024);
  Subscriber subscriber(shm_name);
  
  std::string large_payload(500, 'x');
  (void)publisher.Publish(std::span<const char>(large_payload));
  
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  auto result = subscriber.TryRead();
  
  CHECK(result.header.frame_type == FrameHeader::Type::kMessage);
  CHECK(result.payload.size() == large_payload.size());
}

TEST_CASE("test_exact_capacity_boundary") {
  std::string shm_name = "test_shm_boundary_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  Publisher publisher(shm_name, 512);
  Subscriber subscriber(shm_name);
  
  std::string msg1(100, 'a');
  (void)publisher.Publish(std::span<const char>(msg1));
  
  std::string msg2(100, 'b');
  (void)publisher.Publish(std::span<const char>(msg2));
  
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  
  int count = 0;
  for (int i = 0; i < 5; ++i) {
    auto result = subscriber.TryRead();
    if (result.header.frame_type == FrameHeader::Type::kMessage && !result.payload.empty()) {
      count++;
    }
  }
  CHECK(count == 2);
}
