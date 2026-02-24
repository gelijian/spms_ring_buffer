#ifndef SPMS_RING_BUFFER_H_
#define SPMS_RING_BUFFER_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

#include "file_lock.h"
#include "shared_memory.h"

namespace spms_ring_buffer {

constexpr uint32_t kFrameHeaderMagic = 0x42524246;
constexpr size_t kCacheLineSize = 64;

constexpr uint64_t kShmMagic = 0x53504D5342425253ULL;

#pragma pack(push, 1)
struct FrameHeader {
  enum class Type : uint8_t {
    kMessage = 1,
    kPadding = 2,
  };

  uint64_t data_offset = 0;
  uint32_t frame_len = 0;
  uint32_t payload_len = 0;
  uint32_t magic = kFrameHeaderMagic;
  Type frame_type = Type::kMessage;
  std::array<uint8_t, 11> reserved{};
};
static_assert(sizeof(FrameHeader) == 32);
#pragma pack(pop)

struct alignas(kCacheLineSize) SpmsRingBufferControlBlock {
  uint64_t magic = 0;
  uint64_t data_capacity = 0;
  std::atomic<uint64_t> publish_offset{0};

  [[nodiscard]] uint64_t MaskOffset(uint64_t logical_offset) const { return logical_offset & (data_capacity - 1); }
};

struct ReadResult {
  FrameHeader header;
  std::span<const char> payload;
};

class OverrunException : public std::runtime_error {
 public:
  OverrunException() : std::runtime_error("Subscriber read position overrun by publisher") {}
};

class Publisher {
 public:
  explicit Publisher(const std::string& shm_name, uint64_t capacity = 0)
      : shm_(shm_name, Mode::ReadWrite, sizeof(SpmsRingBufferControlBlock), capacity), lock_(shm_name + ".lock") {
    auto* cb = static_cast<SpmsRingBufferControlBlock*>(shm_.GetBaseAddr());
    cb->magic = kShmMagic;
    cb->data_capacity = capacity;
    cb->publish_offset.store(0, std::memory_order_release);
    lock_.Lock();
  }

  ~Publisher() { lock_.Unlock(); }

  Publisher(const Publisher&) = delete;
  Publisher& operator=(const Publisher&) = delete;

  [[nodiscard]] FrameHeader Publish(std::span<const char> payload) {
    SpmsRingBufferControlBlock* cb = static_cast<SpmsRingBufferControlBlock*>(shm_.GetBaseAddr());
    uint64_t data_capacity = cb->data_capacity;
    void* data_start = shm_.GetDataStart();

    uint32_t length = static_cast<uint32_t>(payload.size());
    uint32_t rounded_payload_len = RoundUp8(length);

    uint64_t current_offset = cb->publish_offset.load(std::memory_order_acquire);
    uint64_t remaining_space = data_capacity - cb->MaskOffset(current_offset);

    if (rounded_payload_len + sizeof(FrameHeader) + sizeof(FrameHeader) > remaining_space) {
      uint32_t padding_len = RoundUp8(remaining_space);
      std::span<const char> empty_payload;
      WriteFrame(current_offset, empty_payload, FrameHeader::Type::kPadding, data_start, cb);
      current_offset = cb->publish_offset.load(std::memory_order_acquire);
    }

    return WriteFrame(current_offset, payload, FrameHeader::Type::kMessage, data_start, cb);
  }

 private:
  [[nodiscard]] static uint32_t RoundUp8(uint32_t value) { return (value + 7) & ~7; }

  FrameHeader WriteFrame(uint64_t offset, std::span<const char> payload, FrameHeader::Type frame_type,
                        void* data_start, SpmsRingBufferControlBlock* cb) {
    uint64_t data_capacity = cb->data_capacity;
    uint64_t masked_offset = cb->MaskOffset(offset);
    char* data_ptr = static_cast<char*>(data_start) + masked_offset;

    uint32_t frame_len = RoundUp8(static_cast<uint32_t>(payload.size()));
    uint32_t payload_len = (frame_type == FrameHeader::Type::kPadding) ? 0 : static_cast<uint32_t>(payload.size());

    auto* frame_header = static_cast<FrameHeader*>(static_cast<void*>(data_ptr));
    frame_header->data_offset = offset;
    frame_header->frame_len = frame_len;
    frame_header->payload_len = payload_len;
    frame_header->magic = kFrameHeaderMagic;
    frame_header->frame_type = frame_type;
    memset(frame_header->reserved.data(), 0, frame_header->reserved.size());

    if (frame_type == FrameHeader::Type::kMessage && !payload.empty()) {
      char* payload_ptr = data_ptr + sizeof(FrameHeader);
      memcpy(payload_ptr, payload.data(), payload.size());
      if (frame_len > payload.size()) {
        memset(payload_ptr + payload.size(), 0, frame_len - payload.size());
      }
    }

    uint64_t total_written = sizeof(FrameHeader) + frame_len;
    uint64_t new_offset = offset + total_written;
    cb->publish_offset.store(new_offset, std::memory_order_release);

    return *frame_header;
  }

  SharedMemory shm_;
  FileLock lock_;
};

class Subscriber {
 public:
  explicit Subscriber(const std::string& shm_name) : shm_(shm_name, Mode::ReadOnly, sizeof(SpmsRingBufferControlBlock), 0) {
    auto* cb = static_cast<SpmsRingBufferControlBlock*>(shm_.GetBaseAddr());
    if (cb->magic != kShmMagic) {
      throw std::runtime_error("Invalid shm magic");
    }
    cache_publish_offset_ = cb->publish_offset.load(std::memory_order_acquire);
    subscribe_offset_ = cache_publish_offset_;
  }

  ~Subscriber() = default;

  Subscriber(const Subscriber&) = delete;
  Subscriber& operator=(const Subscriber&) = delete;

  [[nodiscard]] ReadResult TryRead() {
    SpmsRingBufferControlBlock* cb = static_cast<SpmsRingBufferControlBlock*>(shm_.GetBaseAddr());
    uint64_t data_capacity = cb->data_capacity;
    void* data_start = shm_.GetDataStart();

    if (subscribe_offset_ == cache_publish_offset_) {
      cache_publish_offset_ = cb->publish_offset.load(std::memory_order_acquire);
    }

    if (subscribe_offset_ == cache_publish_offset_) {
      return {};
    }

    if (cache_publish_offset_ - subscribe_offset_ > data_capacity) {
      subscribe_offset_ = cache_publish_offset_;
      return {};
    }

    uint64_t masked_offset = cb->MaskOffset(subscribe_offset_);
    char* data_ptr = static_cast<char*>(data_start) + masked_offset;

    auto* frame_header = static_cast<FrameHeader*>(static_cast<void*>(data_ptr));

    if (frame_header->magic != kFrameHeaderMagic) {
      subscribe_offset_ = cache_publish_offset_;
      return {};
    }

    uint32_t total_len = sizeof(FrameHeader) + frame_header->frame_len;
    subscribe_offset_ += total_len;

    if (frame_header->frame_type == FrameHeader::Type::kPadding) {
      return {*frame_header, {}};
    }

    char* payload_ptr = data_ptr + sizeof(FrameHeader);
    return {*frame_header, {payload_ptr, static_cast<size_t>(frame_header->payload_len)}};
  }

 private:
  SharedMemory shm_;
  uint64_t subscribe_offset_ = 0;
  uint64_t cache_publish_offset_ = 0;
};

}  // namespace spms_ring_buffer

#endif  // SPMS_RING_BUFFER_H_
