#ifndef SPMS_RING_BUFFER_H_
#define SPMS_RING_BUFFER_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "file_lock.h"
#include "shared_memory.h"

namespace spms_ring_buffer {

constexpr uint32_t kFrameHeaderMagic = 0x42524246;
constexpr size_t kCacheLineSize = 64;
constexpr uint64_t kHugePageSize = 2 * 1024 * 1024;
constexpr uint64_t kShmMagic = 0x53504D5342425253ULL;

struct FrameHeader {
  enum class Type : uint8_t {
    kMessage = 1,
    kPadding = 2,
  };

  uint64_t logical_offset = 0;
  uint64_t sequence = 0;
  uint32_t frame_len = 0;
  uint32_t payload_len = 0;
  uint32_t magic = kFrameHeaderMagic;
  Type frame_type = Type::kMessage;
  std::array<uint8_t, 3> reserved{};

  [[nodiscard]] uint32_t TotalFrameLen() const { return sizeof(FrameHeader) + frame_len; }

  [[nodiscard]] uint64_t OffsetEnd() const { return logical_offset + TotalFrameLen(); }

  friend std::ostream& operator<<(std::ostream& os, const FrameHeader& header) {
    os << "FrameHeader{logical_offset=" << header.logical_offset << ", sequence=" << header.sequence
       << ", frame_len=" << header.frame_len << ", payload_len=" << header.payload_len << ", magic=0x" << std::hex
       << header.magic << std::dec << ", frame_type=" << (header.frame_type == Type::kMessage ? "kMessage" : "kPadding")
       << ", total_len=" << header.TotalFrameLen() << ", offset_end=" << header.OffsetEnd() << "}";
    return os;
  }
};
static_assert(sizeof(FrameHeader) == 32);

struct alignas(kCacheLineSize) SpmsRingBufferControlBlock {
  uint64_t magic = 0;
  uint64_t data_capacity = 0;
  std::atomic<uint64_t> publish_offset{0};

  [[nodiscard]] uint64_t PhysicalOffset(uint64_t logical_offset) const { return logical_offset & (data_capacity - 1); }

  [[nodiscard]] static uint64_t ComputeRequiredSize(uint64_t data_capacity) {
    return (sizeof(SpmsRingBufferControlBlock) + data_capacity + kHugePageSize - 1) & ~(kHugePageSize - 1);
  }
};

struct PublisherStats {
  uint64_t messages_published = 0;
  uint64_t publish_offset = 0;
};

struct SubscriberStats {
  uint64_t messages_read = 0;
  uint64_t subscribe_offset = 0;
};

class OverrunException : public std::runtime_error {
 public:
  OverrunException() : std::runtime_error("Subscriber read position overrun by publisher") {}
};

class InvalidFrameException : public std::runtime_error {
 public:
  InvalidFrameException() : std::runtime_error("Invalid frame header: magic mismatch (data corruption detected)") {}
};

class Publisher {
 public:
  static bool IsPowerOfTwo(uint64_t n) { return n > 0 && (n & (n - 1)) == 0; }

  explicit Publisher(const std::string& shm_name, uint64_t capacity = 0)
      : shm_(shm_name, SharedMemory::Mode::ReadWrite, SpmsRingBufferControlBlock::ComputeRequiredSize(capacity)),
        control_block_(static_cast<SpmsRingBufferControlBlock*>(shm_.GetBaseAddr())),
        data_start_(static_cast<char*>(shm_.GetBaseAddr()) + sizeof(SpmsRingBufferControlBlock)),
        lock_(shm_name + ".lock") {
    if (capacity > 0 && !IsPowerOfTwo(capacity)) {
      throw std::invalid_argument("capacity must be power of two, got " + std::to_string(capacity));
    }

    if (shm_.IsCreated()) {
      control_block_->magic = kShmMagic;
      control_block_->data_capacity = capacity;
      control_block_->publish_offset.store(0, std::memory_order_release);
    } else {
      if (control_block_->magic != kShmMagic) {
        throw std::runtime_error("Invalid shm magic: expected 0x" + 
          std::to_string(kShmMagic) + ", got 0x" + 
          std::to_string(control_block_->magic));
      }
      if (control_block_->data_capacity != capacity) {
        throw std::runtime_error("Capacity mismatch: expected " + std::to_string(capacity) + 
          ", got " + std::to_string(control_block_->data_capacity));
      }
    }
  }

  ~Publisher() = default;

  Publisher(const Publisher&) = delete;
  Publisher(Publisher&&) = delete;
  Publisher& operator=(const Publisher&) = delete;
  Publisher& operator=(Publisher&&) = delete;

  [[nodiscard]] FrameHeader Publish(std::span<const char> payload) {
    Batch batch(*this);
    auto header = batch.Add(payload);
    batch.Commit();
    return header;
  }

  [[nodiscard]] FrameHeader Publish(std::string_view sv) {
    return Publish(std::span<const char>{sv.data(), sv.size()});
  }

  class Batch {
   public:
    [[nodiscard]] FrameHeader Add(std::span<const char> payload) {
      auto payload_len = static_cast<uint32_t>(payload.size());
      auto rounded_payload_len = RoundUp8(payload_len);

      if (auto remaining_space = control_block_->data_capacity - control_block_->PhysicalOffset(current_offset_);
          rounded_payload_len + 2 * sizeof(FrameHeader) > remaining_space) {
        FrameHeader padding_header{};
        padding_header.logical_offset = current_offset_;
        padding_header.frame_len = remaining_space - sizeof(FrameHeader);
        padding_header.payload_len = 0;
        padding_header.magic = kFrameHeaderMagic;
        padding_header.frame_type = FrameHeader::Type::kPadding;
        WriteFrameInternal(padding_header, {});
        current_offset_ = padding_header.OffsetEnd();
      }

      FrameHeader header{};
      header.logical_offset = current_offset_;
      header.frame_len = rounded_payload_len;
      header.payload_len = payload_len;
      header.magic = kFrameHeaderMagic;
      header.frame_type = FrameHeader::Type::kMessage;
      WriteFrameInternal(header, payload);
      current_offset_ = header.OffsetEnd();
      messages_count_++;
      return header;
    }

    void Commit() {
      if (!committed_) {
        *messages_published_ += messages_count_;
        control_block_->publish_offset.store(current_offset_, std::memory_order_release);
        committed_ = true;
      }
    }

    void CommitFence() {
      if (!committed_) {
        *messages_published_ += messages_count_;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        control_block_->publish_offset.store(current_offset_, std::memory_order_release);
        committed_ = true;
      }
    }

    [[nodiscard]] bool IsCommitted() const { return committed_; }

    Batch(Batch&&) noexcept = default;
    Batch& operator=(Batch&&) noexcept = default;
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

   private:
    explicit Batch(Publisher& publisher)
        : control_block_(publisher.control_block_), data_start_(publisher.data_start_),
          messages_published_(&publisher.messages_published_) {
      start_offset_ = control_block_->publish_offset.load(std::memory_order_acquire);
      current_offset_ = start_offset_;
    }

    [[nodiscard]] static uint32_t RoundUp8(uint32_t value) { return (value + 7) & ~7; }

    [[nodiscard]] char* GetDataPtr(uint64_t offset) const {
      return data_start_ + control_block_->PhysicalOffset(offset);
    }

    void WriteFrameInternal(const FrameHeader& header, std::span<const char> body) {
      auto data_ptr = data_start_ + control_block_->PhysicalOffset(header.logical_offset);
      memcpy(data_ptr, &header, sizeof(FrameHeader));
      if (!body.empty()) {
        memcpy(data_ptr + sizeof(FrameHeader), body.data(), body.size());
      }
    }

    friend class Publisher;

    SpmsRingBufferControlBlock* control_block_;
    char* data_start_;
    uint64_t start_offset_ = 0;
    uint64_t current_offset_ = 0;
    uint64_t* messages_published_ = nullptr;
    uint64_t messages_count_ = 0;
    bool committed_ = false;
  };

  [[nodiscard]] Batch CreateBatch() { return Batch(*this); }

  [[nodiscard]] PublisherStats GetStats() const {
    return {messages_published_, control_block_->publish_offset.load(std::memory_order_acquire)};
  }

 private:
  SharedMemory shm_;
  SpmsRingBufferControlBlock* control_block_;
  char* data_start_;
  FileLock lock_;
  uint64_t messages_published_ = 0;
};

class Subscriber {
 public:
  explicit Subscriber(const std::string& shm_name)
      : shm_(shm_name, SharedMemory::Mode::ReadOnly, 0),
        control_block_(static_cast<SpmsRingBufferControlBlock*>(shm_.GetBaseAddr())),
        data_start_(static_cast<char*>(shm_.GetBaseAddr()) + sizeof(SpmsRingBufferControlBlock)) {
    if (control_block_->magic != kShmMagic) {
      throw std::runtime_error("Invalid shm magic");
    }
    cache_publish_offset_ = control_block_->publish_offset.load(std::memory_order_acquire);
    subscribe_offset_ = cache_publish_offset_;
  }

  ~Subscriber() = default;

  Subscriber(const Subscriber&) = delete;
  Subscriber(Subscriber&&) = delete;
  Subscriber& operator=(const Subscriber&) = delete;
  Subscriber& operator=(Subscriber&&) = delete;

  struct ReadResult {
    FrameHeader header;
    std::span<const char> payload;
  };

  [[nodiscard]] ReadResult TryRead() {
    if (subscribe_offset_ == cache_publish_offset_) {
      cache_publish_offset_ = control_block_->publish_offset.load(std::memory_order_acquire);
    }
    if (subscribe_offset_ == cache_publish_offset_) {
      return {};
    }
    if (cache_publish_offset_ - subscribe_offset_ > control_block_->data_capacity) {
      throw OverrunException();
    }

    char* data_ptr = data_start_ + control_block_->PhysicalOffset(subscribe_offset_);
    auto& frame_header = *static_cast<const FrameHeader*>(static_cast<void*>(data_ptr));
    if (frame_header.magic != kFrameHeaderMagic) {
      throw InvalidFrameException();
    }

    subscribe_offset_ += frame_header.TotalFrameLen();
    if (frame_header.frame_type == FrameHeader::Type::kPadding) {
      return {frame_header, {}};
    }
    messages_read_++;
    char* payload_ptr = data_ptr + sizeof(FrameHeader);
    return {frame_header, {payload_ptr, static_cast<size_t>(frame_header.payload_len)}};
  }

  [[nodiscard]] const char* GetDataPtr(uint64_t offset) const {
    return data_start_ + control_block_->PhysicalOffset(offset);
  }

  [[nodiscard]] SubscriberStats GetStats() const {
    return {messages_read_, subscribe_offset_};
  }

 private:
  SharedMemory shm_;
  SpmsRingBufferControlBlock* control_block_;
  char* data_start_;
  uint64_t subscribe_offset_ = 0;
  uint64_t cache_publish_offset_ = 0;
  uint64_t messages_read_ = 0;
};

}  // namespace spms_ring_buffer

#endif  // SPMS_RING_BUFFER_H_
