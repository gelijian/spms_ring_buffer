# AGENTS.md - Development Guidelines for spms_broadcast_ring_buffer

## Overview

C++20 single-publisher multi-subscriber (SPMS) broadcast ring buffer library using shared memory for Linux x86-64. Core implementation in header files with demo publisher/subscriber applications.

---

## 1. Build, Lint, and Test Commands

### Building

```bash
# Debug build
cmake -B build && cmake --build build

# Release build
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release && cmake --build cmake-build-release
```

### Running Demo

```bash
# Terminal 1: Start publisher
./build/publisher [shm_name] [capacity]
# Example: ./build/publisher my_shm 1048576

# Terminal 2: Start subscriber
./build/subscriber [shm_name]
```

### Testing

No formal test framework. To test:
1. Build the project
2. Run publisher in one terminal
3. Run subscriber in another terminal
4. Verify messages are received

Clean rebuild:
```bash
rm -rf build cmake-build-release && cmake -B build && cmake --build build
```

### Code Formatting

```bash
# Format with clang-format (Google style)
clang-format -i --style=file spms_ring_buffer.h shared_memory.h file_lock.h publisher.cc subscriber.cc

# Check without modifying
clang-format --style=file --dry-run spms_ring_buffer.h shared_memory.h file_lock.h publisher.cc subscriber.cc
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
| Classes | PascalCase | `Publisher`, `Subscriber` |
| Structs | PascalCase | `FrameHeader`, `SpmsRingBufferControlBlock` |
| Enums | PascalCase | `enum class Type` |
| Constants | kCamelCase | `kFrameHeaderMagic` |
| Member variables | snake_case_ | `shm_name_`, `subscribe_offset_` |
| Functions | PascalCase | `Publish()`, `TryRead()` |
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
- `#pragma pack(push, 1)` for packed structs

### Error Handling

- Use C++ exceptions, inherit from `std::runtime_error`
- NO `std::format` (use `std::to_string` + concatenation)
- Custom exceptions: `OverrunException`, `BufferTooSmallException`

### Concurrency

- Only Acquire/Release semantics in hot paths
- Avoid `fetch_add`, `compare_exchange` in Publish/TryRead

### Class Design

Delete copy/move constructors. Use `explicit` for single-argument constructors. Use `[[nodiscard]]` on functions where return value matters.
```cpp
class Publisher {
 public:
  explicit Publisher(const std::string& shm_name, uint64_t capacity = 0);
  ~Publisher() { lock_.Unlock(); }
  Publisher(const Publisher&) = delete;
  Publisher& operator=(const Publisher&) = delete;
 private:
  SharedMemory shm_;
  FileLock lock_;
};
```

### Best Practices

1. Single data-ring: header + data in one contiguous SHM segment
2. Wrap handling: write padding frame first, then data at buffer start
3. 8-byte alignment: round frame lengths (except padding)
4. No loops in TryRead: process exactly one frame per call
5. Fault recovery: Publisher resumes from publish_offset; Subscriber from latest available

---

## 3. Project Structure

```
.
├── spms_ring_buffer.h           # Core library
├── shared_memory.h               # Shared memory wrapper
├── file_lock.h                   # File lock wrapper
├── publisher.cc                  # Demo publisher
├── subscriber.cc                 # Demo subscriber
├── CMakeLists.txt                # Build config
├── .clang-format                 # Code formatting
├── spec                          # Technical spec
└── build/                        # Build output
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

1. Modify `spms_ring_buffer.h`
2. Update `spec` if API changes
3. Test with publisher/subscriber demo
4. Run `clang-format` before committing
5. Commit changes immediately after any file modification

### Performance Optimization

- Cache `SpmsRingBufferControlBlock*` and `data_start` pointers as private members in Publisher/Subscriber to avoid repeated `static_cast` in hot paths (Publish/TryRead)

### Debugging

- Check `/dev/shm/` for shared memory files
- Look for `.lock` files for file locks
- Publisher outputs "Published: <message>"
- Subscriber outputs "Received: <message>"
