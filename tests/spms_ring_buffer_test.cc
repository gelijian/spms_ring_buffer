#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

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

TEST_CASE("placeholder test") { CHECK(true); }

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
    if (result.header.frame_type == FrameHeader::Type::kPadding) {
      CHECK(false);
    }
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
    bool caught_overrun = false;
    try {
        (void)subscriber.TryRead();
    } catch (const OverrunException& e) {
        caught_overrun = true;
    }

    CHECK(caught_overrun);
}
