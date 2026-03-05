# AGENTS.md - spms_ring_buffer

C++20 single-publisher multi-subscriber (SPMS) broadcast ring buffer library using shared memory for Linux x86-64.

---

## Structure
```
.
├── spms_ring_buffer.h      # Core library (header-only)
├── shared_memory.h         # Shared memory wrapper
├── file_lock.h            # File lock wrapper
├── publisher.cc           # Demo publisher
├── subscriber.cc          # Demo subscriber
├── profiler.cc            # Latency profiler
├── tests/                 # Unit tests (doctest)
├── CMakeLists.txt         # Release -O3
└── .clang-format          # Google style
```

---

## Commands

```bash
# Build (Release -O3)
cmake -B build && cmake --build build

# Debug build
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build

# Run all tests
./build/tests/spms_ring_buffer_test

# Run specific test
./build/tests/spms_ring_buffer_test --test-case="test_name"

# Demo: Terminal 1
./build/publisher [shm_name] [capacity]

# Demo: Terminal 2
./build/subscriber [shm_name]

# Profiler
./build/profiler --subscriber-count 4 --message-count 50000 --rate 10000

# Clean up
rm -f /dev/shm/* *.lock
```

---

## Implementation Plan

See `docs/plans/2026-03-05-refactor-comprehensive.md` for the comprehensive refactor plan covering:
- Code Safety improvements
- Performance enhancements (zero-copy API, memory fences)
- API/Usability (metrics, convenience methods)
- Testing (multi-process, fault recovery, edge cases)

---

## Naming

| Element | Convention | Example |
|---------|------------|---------|
| Classes/Structs | PascalCase | `Publisher`, `FrameHeader` |
| Constants | kCamelCase | `kCacheLineSize`, `kFrameHeaderMagic` |
| Member vars | snake_case_ | `shm_name_`, `publish_offset_` |
| Functions | PascalCase | `Publish()`, `TryRead()`, `Commit()` |
| Namespaces | snake_case | `spms_ring_buffer` |

---

## Constraints

- **Linux x86-64 only**: POSIX `/dev/shm` + fcntl locks
- **No external deps**: C++20 stdlib + pthread only
- **Power-of-two capacity**: Must be non-zero power of two
- **No std::format**: Use `std::to_string` + concatenation
- **Atomic**: `memory_order_acquire` / `memory_order_release` only
- **Class design**: Delete copy/move, use `explicit`, `[[nodiscard]]`

---

## Key Classes

- `Publisher`: Create shared memory, publish messages with `Publish()` or `Batch`
- `Subscriber`: Attach to existing shared memory, read with `TryRead()`
- `Batch`: Efficient bulk publishing - `CreateBatch()` → `Add()` → `Commit()`

---

## API Features

### Zero-Copy Publishing
```cpp
// Option 1: std::span (original)
publisher.Publish(std::span<const char>(data, size));

// Option 2: std::string_view (zero-copy)
std::string_view sv(data);
publisher.Publish(sv);
```

### Batch API
```cpp
auto batch = publisher.CreateBatch();
batch.Add(payload1);
batch.Add(payload2);
batch.Commit();           // Release semantics
```

### Metrics
```cpp
auto pub_stats = publisher.GetStats();
pub_stats.messages_published;
pub_stats.publish_offset;

auto sub_stats = subscriber.GetStats();
sub_stats.messages_read;
sub_stats.subscribe_offset;
```

---

## Performance

- Cache `SpmsRingBufferControlBlock*` and `data_start` pointers as private members
- Use Batch API for multiple messages
- Release build with -O3 for profiling

---

## Known Issues

- **LSP false positives**: Ignore `std::span` errors - compiles correctly with GCC C++20
