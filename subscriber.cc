#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "spms_ring_buffer.h"

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
    ReadResult result = subscriber.TryRead();

    if (!result.payload.empty()) {
      std::string message(result.payload.data(), result.payload.size());
      std::cout << "Received: " << message << std::endl;
      std::cout << "  Header: " << result.header << std::endl;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  return 0;
}
