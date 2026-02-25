# AGENTS.md - Development Guidelines for spms_ring_buffer

## Overview

C++20 single-publisher multi-subscriber (SPMS) broadcast ring buffer library using shared memory for Linux x86-64. Core implementation in header files with demo publisher/subscriber applications and latency profiler.

---

## 1. Build, Lint, and Test Commands

### Building

```bash
# Debug build (default if CMAKE_BUILD_TYPE not set)
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build

# Release build (default - uses -O3 optimization)
cmake -B build && cmake --build build

# Clean rebuild
rm -rf build && cmake -B build && cmake --build build
```

### Running Demo

```bash
# Terminal 1: Start publisher
./build/publisher [shm_name] [capacity]
# Example: ./build/publisher my_shm 1048576

# Terminal 2: Start subscriber
./build/subscriber [shm_name]
```

### Running Profiler (Latency Benchmarking)

```bash
# Run profiler with default settings (1 subscriber, 64-byte messages, 100k messages)
./build/profiler

# Custom configuration
./build/profiler --shm-name test_shm --subscriber-count 4 --message-size 128 --message-count 1000000 --rate 50000

# Options:
#   --shm-name <name>       Shared memory name (default: profiler_shm)
#   --subscriber-count <N>  Number of subscribers (default: 1)
#   --message-size <bytes> Message payload size (default: 64)
#   --message-count <N>     Total messages to publish (default: 100000)
#   --warmup <N>           Warmup messages (default: 1000)
#   --rate <N>             Messages per second (0=unlimited, default: 0)
```

### Testing

No formal test framework. To test:
1. Build the project
2. Run publisher in one terminal
3. Run subscriber in another terminal
4. Verify messages are received

Or use the profiler for automated latency testing.

### Clean Up

```bash
# Remove shared memory files
rm -f /dev/shm/*

# Remove lock files (if any)
rm -f *.lock
```

### Code Formatting

```bash
# Format with clang-format (Google style)
clang-format -i --style=file spms_ring_buffer.h shared_memory.h file_lock.h publisher.cc subscriber.cc profiler.cc

# Check without modifying
clang-format --style=file --dry-run spms_ring_buffer.h shared_memory.h file_lock.h publisher.cc subscriber.cc profiler.cc
```

---

## 2. Code Style Guidelines

### General

- **Standard**: C++20 (strictly required)
- **Style**: Google C++ Coding Style
- **Column Limit**: 120 characters
- **Core Library**: Single header (`spms_ring_buffer.h`)

### Naming Conventions

| Element | Convention | Example |
|---------|------------|---------|
| Classes | PascalCase | `Publisher`, `Subscriber`, `Batch` |
| Structs | PascalCase | `FrameHeader`, `SpmsRingBufferControlBlock` |
| Enums | PascalCase | `enum class Type` |
| Constants | kCamelCase | `kFrameHeaderMagic`, `kCacheLineSize` |
| Member variables | snake_case_ | `shm_name_`, `subscribe_offset_` |
| Functions | PascalCase | `Publish()`, `TryRead()`, `Add()`, `Commit()` |
| Namespaces | snake_case | `spms_ring_buffer` |

### File Organization

Include order: corresponding header, C stdlib, C++ stdlib, project headers.
```cpp
#include "spms_ring_buffer.h"
#include "shared_memory.h"
#include "file_lock.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstdint>
```

### Types and Memory

- Fixed-width types (`uint64_t`, `uint32_t`, etc.)
- `std::atomic` with `memory_order_acquire` / `memory_order_release` only
- `alignas(kCacheLineSize)` for cache-line-aligned structures
- `#pragma pack(push, 1)` for packed structs (if needed)

### Error Handling

- Use C++ exceptions, inherit from `std::runtime_error`
- NO `std::format` (use `std::to_string` + concatenation)
- Custom exceptions: `OverrunException`, `InvalidFrameException`

### Concurrency

- Only Acquire/Release semantics in hot paths
- Avoid `fetch_add`, `compare_exchange` in Publish/TryRead
- Use `std::memory_order_release` when publishing, `std::memory_order_acquire` when subscribing

### Class Design

Delete copy/move constructors. Use `explicit` for single-argument constructors. Use `[[nodiscard]]` on functions where return value matters.
```cpp
class Publisher {
 public:
  explicit Publisher(const std::string& shm_name, uint64_t capacity = 0);
  ~Publisher() { lock_.Unlock(); }
  Publisher(const Publisher&) = delete;
  Publisher(Publisher&&) = delete;
  Publisher& operator=(const Publisher&) = delete;
  Publisher& operator=(Publisher&&) = delete;

  // Batch API for efficient multi-message publishing
  [[nodiscard]] Batch CreateBatch();

 private:
  SharedMemory shm_;
  FileLock lock_;
};
```

### Batch API Usage

The `Batch` class provides efficient bulk publishing with a single atomic commit:
```cpp
// Option 1: Quick publish (auto-creates batch internally)
publisher.Publish(payload);

// Option 2: Manual batch for multiple messages
auto batch = publisher.CreateBatch();
batch.Add(payload1);
batch.Add(payload2);
batch.Commit();  // Single atomic publish_offset update
```

---

## 3. Project Structure

```
.
├── spms_ring_buffer.h      # Core library (Publisher, Subscriber, Batch classes)
├── shared_memory.h         # Shared memory wrapper
├── file_lock.h            # File lock wrapper
├── publisher.cc           # Demo publisher
├── subscriber.cc          # Demo subscriber
├── profiler.cc            # Latency profiler with percentiles
├── CMakeLists.txt         # Build config (Release with -O3)
├── .clang-format          # Code formatting config
├── readme.md              # Technical documentation
└── build/                 # Build output
```

---

## 4. Key Constraints

- **Linux x86-64 only**: POSIX shared memory (`/dev/shm`) and fcntl locks
- **No external deps**: Only C++20 stdlib + pthread
- **Power-of-two capacity**: Must be non-zero power of two
- **No std::format**: Use string concatenation

---

## 5. Common Tasks

### Adding a Feature

1. Modify `spms_ring_buffer.h` (core library)
2. Update `readme.md` if API changes
3. Test with publisher/subscriber or profiler
4. Run `clang-format` before committing
5. Commit changes immediately after any file modification

### Performance Optimization

- Cache `SpmsRingBufferControlBlock*` and `data_start` pointers as private members in Publisher/Subscriber to avoid repeated `static_cast` in hot paths (Publish/TryRead)
- Use Batch API for publishing multiple messages in succession
- Use Release build with -O3 for profiling

### Debugging

- Check `/dev/shm/` for shared memory files
- Look for `.lock` files for file locks
- Publisher outputs "Published: <message>"
- Subscriber outputs "Received: <message>"
- Use profiler with --rate 0 (unlimited) to test maximum throughput

### Profiling Latency

```bash
# Quick latency test
./build/profiler

# Multi-subscriber test with rate limiting (ensures all subscribers receive messages)
./build/profiler --subscriber-count 4 --message-count 50000 --rate 10000

# Large message test
./build/profiler --message-size 1024 --message-count 10000
```

---

## 6. Known Issues

- **LSP false positives**: The LSP may report errors about `std::span` not existing in `std::namespace`. This is a false positive - the code compiles correctly with GCC in C++20 mode. Ignore these errors.

---

## 7. Project History

- Initial implementation of SPMS ring buffer with Publisher/Subscriber classes
- Added Batch class for efficient bulk publishing
- Added profiler executable with latency percentiles (p50, p90, p95, p99, p999)
- Release build uses -O3 optimization
