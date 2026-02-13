#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "spms_broadcast_ring_buffer.h"

using namespace spms_ring_buffer;

int main(int argc, char* argv[]) {
  std::string shm_name = "spms_test";

  if (argc > 1) {
    shm_name = argv[1];
  }

  std::cout << "Starting Subscriber with shm_name=" << shm_name << std::endl;

  Subscriber subscriber(shm_name);

  std::vector<char> buffer(4096);

  while (true) {
    try {
      uint32_t len =
          subscriber.TryRead(buffer.data(), static_cast<uint32_t>(buffer.size()));

      if (len > 0) {
        std::string message(buffer.data(), len);
        std::cout << "Received: " << message << std::endl;
      } else {
        // No new data or padding frame skipped
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    } catch (const OverrunException& e) {
      std::cerr << "Overrun detected: " << e.what() << std::endl;
    } catch (const BufferTooSmallException& e) {
      std::cerr << "Buffer too small: " << e.what() << std::endl;
    }
  }

  return 0;
}
