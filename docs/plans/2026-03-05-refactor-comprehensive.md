# Comprehensive Refactor Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Transform SPMS Ring Buffer from a working prototype into a production-grade library with better safety, performance, usability, and comprehensive test coverage.

**Architecture:** Incremental improvements across 4 focus areas (Code Safety, Performance, API/Usability, Testing) while maintaining backward compatibility. All changes remain Linux-only.

**Tech Stack:** C++20, doctest, POSIX shared memory, fcntl locks

---

## Focus Areas (Priority Order)

| Priority | Area | Description |
|----------|------|-------------|
| 1 | Testing | Multi-process tests, fault recovery, edge cases |
| 2 | Code Safety | Complete remaining safety items |
| 3 | Performance | Zero-copy API, memory fences |
| 4 | API/Usability | Convenience methods, metrics |

---

## Task 1: Complete Code Safety Improvements

### Files
- Modify: `spms_ring_buffer.h` (lines 1-264)
- Modify: `file_lock.h` (lines 1-69)

### Step 1: Add Lock/Unlock methods to FileLock

**Test first:**

```cpp
// Add to tests/spms_ring_buffer_test.cc
TEST_CASE("test_file_lock_manual_lock_unlock") {
  std::string lock_name = "test_lock_manual_" + std::to_string(test_counter++);
  std::string lock_path = "/dev/shm/" + lock_name;
  
  FileLock lock(lock_name);
  
  // Test manual unlock/lock
  lock.Unlock();
  lock.Lock();  // Should not throw
  
  // Cleanup
  unlink(lock_path.c_str());
}
```

**Step 2: Implement Lock/Unlock in file_lock.h**

Add after constructor:
```cpp
void Lock() {
  struct flock fl{};
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;
  
  if (fcntl(fd_, F_SETLKW, &fl) < 0) {
    throw std::runtime_error("Failed to acquire lock");
  }
}

void Unlock() {
  struct flock fl{};
  fl.l_type = F_UNLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;
  fcntl(fd_, F_SETLK, &fl);
}
```

**Step 3: Run tests**

```bash
./build/tests/spms_ring_buffer_test --test-case="test_file_lock_manual_lock_unlock"
```

Expected: PASS

**Step 4: Commit**

```bash
git add file_lock.h tests/spms_ring_buffer_test.cc
git commit -m "feat: add Lock/Unlock methods to FileLock"
```

---

## Task 2: Add Zero-Copy Publishing API

### Files
- Modify: `spms_ring_buffer.h` (Publisher class)

### Step 1: Add PublishStringView test

```cpp
// tests/spms_ring_buffer_test.cc
TEST_CASE("test_publish_string_view_zero_copy") {
  std::string shm_name = "test_shm_zc_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  Publisher publisher(shm_name, 1024);
  
  // Test zero-copy with std::string_view
  std::string payload = "zero_copy_test";
  std::string_view sv(payload);
  FrameHeader header = publisher.Publish(sv);
  
  CHECK(header.payload_len == payload.size());
  CHECK(header.frame_type == FrameHeader::Type::kMessage);
}
```

**Step 2: Run test to verify it fails**

```bash
./build/tests/spms_ring_buffer_test --test-case="test_publish_string_view_zero_copy"
```

Expected: FAIL - method not defined

**Step 3: Implement Publish with std::string_view**

Add to Publisher class in spms_ring_buffer.h:

```cpp
// After Publish(std::span<const char>)
[[nodiscard]] FrameHeader Publish(std::string_view sv) {
  std::span<const char> payload{sv.data(), sv.size()};
  return Publish(payload);
}
```

**Step 4: Run test to verify it passes**

```bash
./build/tests/spms_ring_buffer_test --test-case="test_publish_string_view_zero_copy"
```

Expected: PASS

**Step 5: Commit**

```bash
git add spms_ring_buffer.h tests/spms_ring_buffer_test.cc
git commit -m "feat: add zero-copy Publish(std::string_view) API"
```

---

## Task 3: Add Memory Fence Support

### Files
- Modify: `spms_ring_buffer.h` (Publisher::Batch class)

### Step 1: Add CommitFence test

```cpp
// tests/spms_ring_buffer_test.cc
TEST_CASE("test_batch_commit_fence") {
  std::string shm_name = "test_shm_fence_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  Publisher publisher(shm_name, 1024);
  Subscriber subscriber(shm_name);
  
  // Create batch and commit with fence
  auto batch = publisher.CreateBatch();
  batch.Add(std::span<const char>("msg1", 4));
  batch.Add(std::span<const char>("msg2", 4));
  batch.CommitFence();  // Full memory fence
  
  CHECK(batch.IsCommitted() == true);
  
  // Reader should immediately see data
  std::this_thread::sleep_for(std::chrono::microseconds(50));
  auto result1 = subscriber.TryRead();
  CHECK(result1.payload.size() == 4);
}
```

**Step 2: Run test to verify it fails**

```bash
./build/tests/spms_ring_buffer_test --test-case="test_batch_commit_fence"
```

Expected: FAIL - method not defined

**Step 3: Implement CommitFence in Batch class**

Add after Commit() method:

```cpp
void CommitFence() {
  if (!committed_) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    control_block_->publish_offset.store(current_offset_, std::memory_order_release);
    committed_ = true;
  }
}
```

**Step 4: Run test to verify it passes**

```bash
./build/tests/spms_ring_buffer_test --test-case="test_batch_commit_fence"
```

Expected: PASS

**Step 5: Commit**

```bash
git add spms_ring_buffer.h tests/spms_ring_buffer_test.cc
git commit -m "feat: add CommitFence with full memory barrier"
```

---

## Task 4: Add Metrics API

### Files
- Modify: `spms_ring_buffer.h` (Publisher and Subscriber classes)

### Step 1: Add metrics test

```cpp
// tests/spms_ring_buffer_test.cc
TEST_CASE("test_metrics_api") {
  std::string shm_name = "test_shm_metrics_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  Publisher publisher(shm_name, 1024);
  (void)publisher.Publish(std::span<const char>("test", 4));
  
  // Test Publisher metrics
  auto pub_stats = publisher.GetStats();
  CHECK(pub_stats.messages_published > 0);
  CHECK(pub_stats.publish_offset > 0);
  
  Subscriber subscriber(shm_name);
  (void)subscriber.TryRead();
  
  // Test Subscriber metrics
  auto sub_stats = subscriber.GetStats();
  CHECK(sub_stats.messages_read > 0);
}
```

**Step 2: Run test to verify it fails**

```bash
./build/tests/spms_ring_buffer_test --test-case="test_metrics_api"
```

Expected: FAIL - method not defined

**Step 3: Add Stats structs and GetStats methods**

Add to spms_ring_buffer.h:

```cpp
struct PublisherStats {
  uint64_t messages_published = 0;
  uint64_t publish_offset = 0;
};

struct SubscriberStats {
  uint64_t messages_read = 0;
  uint64_t subscribe_offset = 0;
};
```

Add to Publisher class:
```cpp
[[nodiscard]] PublisherStats GetStats() const {
  return {messages_published_, control_block_->publish_offset.load(std::memory_order_acquire)};
}
```

Add to Subscriber class:
```cpp
[[nodiscard]] SubscriberStats GetStats() const {
  return {messages_read_, subscribe_offset_};
}
```

Add member variables:
- Publisher: `uint64_t messages_published_ = 0;` (increment in Batch::Commit)
- Subscriber: `uint64_t messages_read_ = 0;` (increment in TryRead)

**Step 4: Run test to verify it passes**

```bash
./build/tests/spms_ring_buffer_test --test-case="test_metrics_api"
```

Expected: PASS

**Step 5: Commit**

```bash
git add spms_ring_buffer.h tests/spms_ring_buffer_test.cc
git commit -m "feat: add GetStats API for Publisher and Subscriber"
```

---

## Task 5: Multi-Process Tests

### Files
- Modify: `tests/spms_ring_buffer_test.cc`

### Step 1: Add fork-based multi-process test

```cpp
// tests/spms_ring_buffer_test.cc
TEST_CASE("test_multi_process_publisher_subscriber") {
  std::string shm_name = "test_shm_mp_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  pid_t pid = fork();
  
  if (pid == 0) {
    // Child: Publisher
    Publisher publisher(shm_name, 1024);
    for (int i = 0; i < 10; ++i) {
      std::string msg = "msg_" + std::to_string(i);
      (void)publisher.Publish(std::span<const char>(msg));
    }
    _exit(0);
  }
  
  // Parent: Wait for child
  int status;
  waitpid(pid, &status, 0);
  CHECK(WIFEXITED(status));
  
  // Parent: Read all messages
  Subscriber subscriber(shm_name);
  int read_count = 0;
  for (int i = 0; i < 20; ++i) {
    auto result = subscriber.TryRead();
    if (result.header.frame_type == FrameHeader::Type::kMessage && !result.payload.empty()) {
      read_count++;
    }
  }
  CHECK(read_count == 10);
}
```

**Step 2: Run test to verify it passes**

```bash
./build/tests/spms_ring_buffer_test --test-case="test_multi_process_publisher_subscriber"
```

Expected: PASS

**Step 3: Commit**

```bash
git add tests/spms_ring_buffer_test.cc
git commit -m "test: add multi-process publisher/subscriber test"
```

---

## Task 6: Fault Recovery Tests

### Files
- Modify: `tests/spms_ring_buffer_test.cc`

### Step 1: Add publisher crash recovery test

```cpp
TEST_CASE("test_publisher_crash_recovery") {
  std::string shm_name = "test_shm_recovery_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  // First publisher publishes messages
  {
    Publisher publisher1(shm_name, 1024);
    (void)publisher1.Publish(std::span<const char>("msg1", 4));
    (void)publisher1.Publish(std::span<const char>("msg2", 4));
    // publisher1 destroyed here (simulates crash)
  }
  
  // Second publisher should resume from where first left off
  Publisher publisher2(shm_name, 1024);
  auto stats = publisher2.GetStats();
  CHECK(stats.publish_offset > 0);  // Should see existing data
  
  (void)publisher2.Publish(std::span<const char>("msg3", 4));
  
  // Subscriber should see all 3 messages
  Subscriber subscriber(shm_name);
  int count = 0;
  for (int i = 0; i < 10; ++i) {
    auto result = subscriber.TryRead();
    if (result.header.frame_type == FrameHeader::Type::kMessage && !result.payload.empty()) {
      count++;
    }
  }
  CHECK(count == 3);
}
```

**Step 2: Add subscriber crash recovery test

```cpp
TEST_CASE("test_subscriber_crash_recovery") {
  std::string shm_name = "test_shm_sub_recovery_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  Publisher publisher(shm_name, 1024);
  
  // First subscriber reads some messages
  {
    Subscriber subscriber1(shm_name);
    (void)publisher.Publish(std::span<const char>("msg1", 4));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    (void)subscriber1.TryRead();
    // subscriber1 destroyed
  }
  
  // Second subscriber should resume from latest publish offset
  (void)publisher.Publish(std::span<const char>("msg2", 4));
  (void)publisher.Publish(std::span<const char>("msg3", 4));
  
  Subscriber subscriber2(shm_name);
  int count = 0;
  for (int i = 0; i < 10; ++i) {
    auto result = subscriber2.TryRead();
    if (result.header.frame_type == FrameHeader::Type::kMessage && !result.payload.empty()) {
      count++;
    }
  }
  CHECK(count == 2);  // Only msg2 and msg3
}
```

**Step 3: Run tests**

```bash
./build/tests/spms_ring_buffer_test --test-case="test_publisher_crash_recovery"
./build/tests/spms_ring_buffer_test --test-case="test_subscriber_crash_recovery"
```

Expected: Both PASS

**Step 4: Commit**

```bash
git add tests/spms_ring_buffer_test.cc
git commit -m "test: add fault recovery tests for publisher and subscriber"
```

---

## Task 7: Edge Case Tests

### Files
- Modify: `tests/spms_ring_buffer_test.cc`

### Step 1: Add max payload test

```cpp
TEST_CASE("test_max_payload_size") {
  std::string shm_name = "test_shm_max_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  Publisher publisher(shm_name, 1024);
  Subscriber subscriber(shm_name);
  
  // Try publishing near-capacity payload
  std::string large_payload(500, 'x');
  (void)publisher.Publish(std::span<const char>(large_payload));
  
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  auto result = subscriber.TryRead();
  
  CHECK(result.header.frame_type == FrameHeader::Type::kMessage);
  CHECK(result.payload.size() == large_payload.size());
}
```

### Step 2: Add exact capacity boundary test

```cpp
TEST_CASE("test_exact_capacity_boundary") {
  std::string shm_name = "test_shm_boundary_" + std::to_string(test_counter++);
  ShmCleanupFixture cleanup(shm_name);
  
  // 256 byte capacity
  Publisher publisher(shm_name, 256);
  
  // Fill exactly to boundary
  std::string msg1(100, 'a');
  (void)publisher.Publish(std::span<const char>(msg1));
  
  std::string msg2(100, 'b');
  (void)publisher.Publish(std::span<const char>(msg2));
  
  Subscriber subscriber(shm_name);
  int count = 0;
  for (int i = 0; i < 5; ++i) {
    auto result = subscriber.TryRead();
    if (result.header.frame_type == FrameHeader::Type::kMessage && !result.payload.empty()) {
      count++;
    }
  }
  CHECK(count == 2);
}
```

### Step 3: Run tests

```bash
./build/tests/spms_ring_buffer_test --test-case="test_max_payload_size"
./build/tests/spms_ring_buffer_test --test-case="test_exact_capacity_boundary"
```

Expected: Both PASS

**Step 4: Commit**

```bash
git add tests/spms_ring_buffer_test.cc
git commit -m "test: add edge case tests for payload size and capacity boundary"
```

---

## Task 8: Update Documentation

### Files
- Modify: `AGENTS.md`
- Modify: `readme.md`

### Step 1: Update AGENTS.md

Add new sections:
- New commands for testing
- Updated naming conventions if new methods added
- Any new build requirements

### Step 2: Update readme.md

Document new APIs:
- `Publish(std::string_view)`
- `CommitFence()`
- `GetStats()`
- New test cases

### Step 3: Commit

```bash
git add AGENTS.md readme.md
git commit -m "docs: update documentation for new features"
```

---

## Final Verification

### Run all tests

```bash
cmake -B build && cmake --build build
./build/tests/spms_ring_buffer_test
```

Expected: All tests pass

### Final commit

```bash
git add -A
git commit -m "refactor: comprehensive improvements - safety, performance, API, tests"
```

---

## Success Criteria

- [ ] All 8 tasks completed
- [ ] Code compiles without errors
- [ ] All existing tests still pass
- [ ] New tests pass (multi-process, fault recovery, edge cases)
- [ ] No changes to public API (backward compatible)
- [ ] Documentation updated

---

## Summary of Changes

| Task | Description | Files Modified |
|------|-------------|----------------|
| 1 | FileLock Lock/Unlock methods | file_lock.h |
| 2 | Zero-copy Publish(std::string_view) | spms_ring_buffer.h |
| 3 | CommitFence with full memory barrier | spms_ring_buffer.h |
| 4 | GetStats API for metrics | spms_ring_buffer.h |
| 5 | Multi-process tests | tests/spms_ring_buffer_test.cc |
| 6 | Fault recovery tests | tests/spms_ring_buffer_test.cc |
| 7 | Edge case tests | tests/spms_ring_buffer_test.cc |
| 8 | Documentation updates | AGENTS.md, readme.md |

---

**Plan complete and saved to `docs/plans/2026-03-05-refactor-comprehensive.md`**

Two execution options:

1. **Subagent-Driven (this session)** - I dispatch fresh subagent per task, review between tasks, fast iteration

2. **Parallel Session (separate)** - Open new session with executing_plans, batch execution with checkpoints

Which approach?
