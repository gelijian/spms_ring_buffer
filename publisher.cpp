#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "spms_broadcast_ring_buffer.h"

using namespace spms_ring_buffer;

int main(int argc, char* argv[]) {
  std::string shm_name = "spms_test";
  uint64_t capacity = 1024 * 1024;  // 1MB

  if (argc > 1) {
    shm_name = argv[1];
  }
  if (argc > 2) {
    capacity = std::stoull(argv[2]);
  }

  std::cout << "Starting Publisher with shm_name=" << shm_name << ", capacity=" << capacity << std::endl;

  Publisher publisher(shm_name, capacity);

  int frame_count = 0;
  while (true) {
    std::string message = "Frame #" + std::to_string(frame_count++) + " - Message from publisher at " +
                          std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

    publisher.Publish(message.data(), static_cast<uint32_t>(message.size()));

    std::cout << "Published: " << message << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  return 0;
}
