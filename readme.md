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
*   **Encapsulation:** The Shared Memory (SHM) module should be independently encapsulated (`shared_memory.h`).
*   **Exclusivity:** Provide a `FileLock` for process-level mutual exclusion to ensure only one Publisher instance writes to the buffer (`file_lock.h`).
*   **Semantics:** Support Pub-Sub Fan-out (Broadcast) semantics.

### 1.3 Frame & Buffer Management
*   **Variable Length:** Support variable-length frames.
*   **Wrap-around Handling:** When `(rounded_payload_len + sizeof(FrameHeader) + sizeof(FrameHeader)) > remaining_space`, a **Padding Frame** must be appended to ensure remaining > sizeof(FrameHeader), and the actual data frame must be written at the beginning of the buffer.
*   **Alignment:** Data frames should be rounded to 8-byte boundaries (except for padding).

### 1.4 Robustness & Recovery
*   **Overrun Handling:** If a subscriber is too slow and its read position is overwritten by the publisher, the subscriber silently resyncs to the latest publish_offset. This must not impact the publisher or other subscribers.
*   **Fault Recovery:** 
    *   **Publisher:** Resume from the current `publish_offset` in SHM after a crash.
    *   **Subscriber:** Resume from the latest available `publish_offset` after a crash.

## 2. Data Structures

```cpp
namespace spms_ring_buffer {

constexpr uint32_t kFrameHeaderMagic = 0x42524246; // "BRBF"
constexpr size_t kCacheLineSize = 64;
constexpr uint64_t kHugePageSize = 2 * 1024 * 1024;

struct FrameHeader {
  enum class Type : uint8_t {
    kMessage = 1,
    kPadding = 2,
  };

  uint64_t logical_offset = 0;  // Monotonically increasing offset BEFORE this frame
  uint32_t frame_len = 0;    // Total len (excl. header) rounded to 8 bytes (except padding)
  uint32_t payload_len = 0;  // Actual data len; <= frame_len
  uint32_t magic = kFrameHeaderMagic; 
  Type frame_type = Type::kMessage;
  std::array<uint8_t, 11> reserved; 

  [[nodiscard]] uint32_t TotalFrameLen() const { return sizeof(FrameHeader) + frame_len; }

  [[nodiscard]] uint64_t OffsetEnd() const { return logical_offset + TotalFrameLen(); }
};
static_assert(sizeof(FrameHeader) == 32);

struct alignas(kCacheLineSize) SpmsRingBufferControlBlock {
  uint64_t magic = 0;
  uint64_t data_capacity = 0; // Must be power of two
  std::atomic<uint64_t> publish_offset{0}; // Global write cursor

  [[nodiscard]] uint64_t PhysicalOffset(uint64_t logical_offset) const { return logical_offset & (data_capacity - 1); }

  [[nodiscard]] static uint64_t ComputeRequiredSize(uint64_t data_capacity) {
    return (sizeof(SpmsRingBufferControlBlock) + data_capacity + kHugePageSize - 1) & ~(kHugePageSize - 1);
  }
};

} // namespace spms_ring_buffer
```

## 3. Class Interfaces

### 3.1 SharedMemory Interface
```cpp
namespace spms_ring_buffer {

enum class Mode { ReadWrite, ReadOnly };

class SharedMemory {
 public:
  SharedMemory() = default;
  ~SharedMemory();

  explicit SharedMemory(const std::string& name, Mode mode, uint64_t size);
  
  void Open(const std::string& name, Mode mode, uint64_t size);
  void Detach();

  [[nodiscard]] void* GetBaseAddr() const;
  [[nodiscard]] uint64_t GetSize() const;
  [[nodiscard]] bool IsCreated() const;

 private:
  // ...
};

} // namespace spms_ring_buffer
```

### 3.2 FileLock Interface
```cpp
namespace spms_ring_buffer {

class FileLock {
 public:
  explicit FileLock(const std::string& lock_path);
  ~FileLock();

  void Lock();
  void Unlock();

 private:
  // ...
};

} // namespace spms_ring_buffer
```

### 3.3 Publisher Interface
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
  // Returns FrameHeader for debugging purposes.
  [[nodiscard]] FrameHeader Publish(std::span<const char> payload);
};

} // namespace spms_ring_buffer
```

### 3.4 Subscriber Interface
```cpp
namespace spms_ring_buffer {

class Subscriber {
 public:
  struct ReadResult {
    FrameHeader header;
    std::span<const char> payload;
  };

  explicit Subscriber(const std::string& shm_name);
  ~Subscriber();

  Subscriber(const Subscriber&) = delete;
  Subscriber& operator=(const Subscriber&) = delete;

  /**
   * Tries to read the next frame.
   * @return: ReadResult containing FrameHeader and payload span.
   *   - payload.empty(): No new data available OR a Padding frame was encountered.
   *   - !payload.empty(): Valid message payload.
   *
   * Note: The subscriber uses cache_publish_offset_ to minimize atomic loads.
   * It only updates from SpmsRingBufferControlBlock when subscribe_offset_ == cache_publish_offset_.
   * On overrun, subscriber silently resyncs to latest publish_offset.
   */
  [[nodiscard]] ReadResult TryRead();

 private:
  uint64_t subscribe_offset_ = 0;
  uint64_t cache_publish_offset_ = 0; 
};

} // namespace spms_ring_buffer
```

## 4. Deliverables Requirement

1.  **`spms_ring_buffer.h`**: Core library (Publisher, Subscriber, FrameHeader, ReadResult).
2.  **`shared_memory.h`**: Shared memory wrapper (SharedMemory class).
3.  **`file_lock.h`**: File lock wrapper (FileLock class).
4.  **`publisher.cc` / `subscriber.cc`**: Demo applications.
5.  **Error Handling**: Use C++ exceptions. No `std::format`.
6.  **No Loop Guarantee**: `TryRead` must not contain internal `while` loops for skipping padding; it should process one frame (either Message or Padding) per call.
7.  **Fault Recovery Test**: Demonstrate killing and restarting the publisher/subscriber without corrupting the ring buffer.
