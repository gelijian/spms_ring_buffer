# Technical Specification: SpmsBroadcastRingBuffer

## Overview
Implement a high-performance **Single Publisher Multi Subscriber (SPMS)** broadcast ring buffer for Linux on x86-64 architecture. The system supports variable-length frames and cross-process communication via shared memory.

## 1. Technical Requirements

### 1.1 Architecture & Performance
*   **Standard:** C++20.
*   **Locality:** Use a **single data-ring** design (header and data in one contiguous shared memory segment) to ensure optimal CPU cache locality.
*   **Memory Model:** Strictly use **Acquire/Release** semantics. Do **not** use `fetch_add`, `compare_exchange`, or other heavy atomic instructions in the hot path.
*   **Coding Style:** Follow the **Google C++ Coding Style**.

### 1.2 Communication & Concurrency
*   **IPC:** Support both multi-process (Shared Memory) and multi-thread communication.
*   **Encapsulation:** The Shared Memory (SHM) module should be independently encapsulated.
*   **Exclusivity:** Provide a `FileLock` for process-level mutual exclusion to ensure only one Publisher instance writes to the buffer.
*   **Semantics:** Support Pub-Sub Fan-out (Broadcast) semantics.

### 1.3 Frame & Buffer Management
*   **Variable Length:** Support variable-length frames.
*   **Wrap-around Handling:** When `(frame_total_len + sizeof(FrameHeader)) > remaining_space`, a **Padding Frame** must be appended to fill the tail, and the actual data frame must be written at the beginning of the buffer.
*   **Alignment:** Data frames should be rounded to 8-byte boundaries (except for padding).

### 1.4 Robustness & Recovery
*   **Overrun Handling:** If a subscriber is too slow and its read position is overwritten by the publisher, the API must throw an exception. This must not impact the publisher or other subscribers.
*   **Fault Recovery:** 
    *   **Publisher:** Resume from the current `publish_offset` in SHM after a crash.
    *   **Subscriber:** Resume from the latest available `publish_offset` after a crash.

## 2. Data Structures

```cpp
namespace spms_ring_buffer {

constexpr uint32_t kFrameHeaderMagic = 0x42524246; // "BRBF"
constexpr size_t kCacheLineSize = 64;

struct FrameHeader {
  enum class Type : uint8_t {
    kMessage = 1,
    kPadding = 2,
  };

  uint64_t data_offset = 0;  // Monotonically increasing offset BEFORE this frame
  uint32_t frame_len = 0;    // Total len (excl. header) rounded to 8 bytes (except padding)
  uint32_t payload_len = 0;  // Actual data len; <= frame_len
  uint32_t magic = kFrameHeaderMagic; 
  Type frame_type = Type::kMessage;
  std::array<uint8_t, 11> reserved; 
};
static_assert(sizeof(FrameHeader) == 32);

struct alignas(kCacheLineSize) ShmHeader {
  uint64_t magic = 0;
  uint64_t data_capacity = 0; // Must be power of two
  std::atomic<uint64_t> publish_offset{0}; // Global write cursor
};

} // namespace spms_ring_buffer
```

## 3. Class Interfaces

### 3.1 Publisher Interface
```cpp
namespace spms_ring_buffer {

class Publisher {
 public:
  // capacity > 0: Init/Truncate SHM. capacity == 0: Attach to existing.
  explicit Publisher(const std::string& shm_name, uint64_t capacity = 0);
  ~Publisher();

  Publisher(const Publisher&) = delete;
  Publisher& operator=(const Publisher&) = delete;

  // Handles wrapping, padding, and Acquire/Release ordering.
  void Publish(const void* payload, uint32_t length);
};

} // namespace spms_ring_buffer
```

### 3.2 Subscriber Interface
```cpp
namespace spms_ring_buffer {

class Subscriber {
 public:
  explicit Subscriber(const std::string& shm_name);
  ~Subscriber();

  Subscriber(const Subscriber&) = delete;
  Subscriber& operator=(const Subscriber&) = delete;

  /**
   * Tries to read the next frame.
   * @return:
   *   - > 0: Length of the payload successfully read.
   *   - == 0: No new data available OR a Padding frame was encountered and skipped.
   * @throws:
   *   - OverrunException: If the publisher has overwritten the subscriber's position.
   *   - BufferTooSmallException: If dest_capacity < payload_len.
   * 
   * Note: The subscriber uses cache_publish_offset_ to minimize atomic loads.
   * It only updates from ShmHeader when subscribe_offset_ == cache_publish_offset_.
   */
  uint32_t TryRead(void* dest_buffer, uint32_t dest_capacity);

 private:
  uint64_t subscribe_offset_ = 0;
  uint64_t cache_publish_offset_ = 0; 
};

} // namespace spms_ring_buffer
```

## 4. Deliverables Requirement

1.  **`spms_broadcast_ring_buffer.h`**: Core implementation.
2.  **`publisher.cpp` / `subscriber.cpp`**: Demo applications.
3.  **Error Handling**: Use C++ exceptions. No `std::format`.
4.  **No Loop Guarantee**: `TryRead` must not contain internal `while` loops for skipping padding; it should process one frame (either Message or Padding) per call.
5.  **Fault Recovery Test**: Demonstrate killing and restarting the publisher/subscriber without corrupting the ring buffer.
