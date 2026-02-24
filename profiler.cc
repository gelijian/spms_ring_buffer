#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <getopt.h>
#include <unordered_map>
#include <iomanip>

#include "spms_ring_buffer.h"

using namespace spms_ring_buffer;

struct Config {
  std::string shm_name = "profiler_shm";
  int subscriber_count = 1;
  size_t message_size = 64;
  uint64_t message_count = 100000;
  uint64_t warmup = 1000;
  uint64_t rate = 0;
};

class LatencyStats {
 public:
  explicit LatencyStats(size_t capacity = 0) {
    if (capacity > 0) {
      samples_.reserve(capacity);
    }
  }

  void Add(uint64_t latency_ns) {
    samples_.push_back(latency_ns);
  }

  [[nodiscard]] size_t Count() const { return samples_.size(); }

  struct Result {
    uint64_t min = 0;
    uint64_t max = 0;
    uint64_t avg = 0;
    uint64_t p50 = 0;
    uint64_t p99 = 0;
    uint64_t p999 = 0;

    friend std::ostream& operator<<(std::ostream& os, const Result& r) {
      os << std::setw(10) << r.min << " | "
         << std::setw(13) << r.max << " | "
         << std::setw(13) << r.avg << " | "
         << std::setw(13) << r.p50 << " | "
         << std::setw(13) << r.p99 << " | "
         << std::setw(13) << r.p999;
      return os;
    }
  };

  [[nodiscard]] Result Calculate() const {
    Result r{};
    if (samples_.empty()) {
      return r;
    }
    auto sorted = samples_;
    std::sort(sorted.begin(), sorted.end());
    r.min = sorted.front();
    r.max = sorted.back();
    r.avg = std::accumulate(sorted.begin(), sorted.end(), 0ULL) / sorted.size();
    r.p50 = sorted[sorted.size() * 50 / 100];
    r.p99 = sorted[sorted.size() * 99 / 100];
    r.p999 = sorted[sorted.size() * 999 / 1000];
    return r;
  }

 private:
  std::vector<uint64_t> samples_;
};

struct GlobalStats {
  std::atomic<uint64_t> messages_published{0};
  std::atomic<bool> publisher_done{false};
  std::unordered_map<int, LatencyStats> subscriber_stats;
};

void PrintUsage(const char* program_name) {
  std::cout << "Usage: " << program_name << " [options]\n"
            << "Options:\n"
            << "  --shm-name <name>       Shared memory name (default: profiler_shm)\n"
            << "  --subscriber-count <N>   Number of subscribers (default: 1)\n"
            << "  --message-size <bytes>  Message payload size (default: 64)\n"
            << "  --message-count <N>     Total messages to publish (default: 100000)\n"
            << "  --warmup <N>            Warmup messages (default: 1000)\n"
            << "  --rate <N>              Messages per second (0=unlimited, default: 0)\n"
            << "  -h, --help              Show this help\n";
}

bool ParseArgs(int argc, char* argv[], Config& cfg) {
  static struct option long_options[] = {
      {"shm-name", required_argument, 0, 's'},
      {"subscriber-count", required_argument, 0, 'c'},
      {"message-size", required_argument, 0, 'm'},
      {"message-count", required_argument, 0, 'n'},
      {"warmup", required_argument, 0, 'w'},
      {"rate", required_argument, 0, 'r'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}};

  int opt;
  int option_index = 0;
  while ((opt = getopt_long(argc, argv, "hs:c:m:n:w:r:", long_options, &option_index)) != -1) {
    switch (opt) {
      case 's':
        cfg.shm_name = optarg;
        break;
      case 'c':
        cfg.subscriber_count = std::stoi(optarg);
        break;
      case 'm':
        cfg.message_size = std::stoul(optarg);
        break;
      case 'n':
        cfg.message_count = std::stoull(optarg);
        break;
      case 'w':
        cfg.warmup = std::stoull(optarg);
        break;
      case 'r':
        cfg.rate = std::stoull(optarg);
        break;
      case 'h':
        PrintUsage(argv[0]);
        return false;
      default:
        PrintUsage(argv[0]);
        return false;
    }
  }
  return true;
}

void PublisherThread(Config& cfg, GlobalStats& stats, Publisher& publisher) {
  std::vector<char> payload(cfg.message_size);
  uint64_t published = 0;

  std::chrono::steady_clock::time_point last_publish;
  if (cfg.rate > 0) {
    last_publish = std::chrono::steady_clock::now();
  }

  while (published < cfg.message_count + cfg.warmup) {
    if (cfg.rate > 0) {
      auto interval = std::chrono::microseconds(1'000'000 / cfg.rate);
      auto now = std::chrono::steady_clock::now();
      while (now - last_publish < interval) {
        now = std::chrono::steady_clock::now();
      }
      last_publish = now;
    }

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::memcpy(payload.data(), &now, sizeof(now));

    (void)publisher.Publish(std::span<const char>(payload.data(), payload.size()));
    published++;
    stats.messages_published.store(published, std::memory_order_release);
  }
  stats.publisher_done.store(true, std::memory_order_release);
}

void SubscriberThread(Config& cfg, GlobalStats& stats, int subscriber_id) {
  Subscriber subscriber(cfg.shm_name);

  stats.subscriber_stats.emplace(subscriber_id, LatencyStats(cfg.message_count));

  uint64_t received = 0;
  bool warmup_done = false;

  while (true) {
    auto result = subscriber.TryRead();
    if (!result.payload.empty()) {
      if (!warmup_done) {
        received++;
        if (received >= cfg.warmup) {
          warmup_done = true;
          received = 0;
        }
        continue;
      }

      auto now = std::chrono::steady_clock::now().time_since_epoch().count();
      uint64_t then = 0;
      std::memcpy(&then, result.payload.data(), sizeof(then));
      uint64_t latency_ns = now - then;

      stats.subscriber_stats[subscriber_id].Add(latency_ns);

      received++;

      if (received >= cfg.message_count) {
        break;
      }
    } else if (stats.publisher_done.load(std::memory_order_acquire)) {
      if (received >= cfg.message_count) {
        break;
      }
    }
  }
}

void PrintResults(Config& cfg, GlobalStats& stats) {
  std::cout << "\n=== Latency Results ===\n";
  std::cout << "Messages: " << cfg.message_count << " (warmup: " << cfg.warmup << ")\n";
  std::cout << "Subscribers: " << cfg.subscriber_count << "\n";
  std::cout << "Message size: " << cfg.message_size << " bytes\n";
  std::cout << "Rate: " << (cfg.rate > 0 ? std::to_string(cfg.rate) : "unlimited") << " msg/sec\n\n";

  std::cout << "Per-Subscriber Latency (ns):\n";
  std::cout << "          |      Min      |      Max      |      Avg      |      P50      |      P99      |     P999\n";
  std::cout << "----------|---------------|---------------|---------------|---------------|---------------|-------------\n";

  size_t total_received = 0;
  for (int i = 0; i < cfg.subscriber_count; i++) {
    const auto& latency = stats.subscriber_stats[i];
    auto result = latency.Calculate();
    total_received += latency.Count();

    std::cout << "Sub #" << i << " |" << result << "\n";
  }

  std::cout << "\nTotal published: " << stats.messages_published.load() << "\n";
  std::cout << "Total received: " << total_received << "\n";
}

int main(int argc, char* argv[]) {
  Config cfg;

  if (!ParseArgs(argc, argv, cfg)) {
    return 1;
  }

  std::cout << "=== Profiler Configuration ===\n";
  std::cout << "Shared memory: " << cfg.shm_name << "\n";
  std::cout << "Subscribers: " << cfg.subscriber_count << "\n";
  std::cout << "Message size: " << cfg.message_size << " bytes\n";
  std::cout << "Message count: " << cfg.message_count << "\n";
  std::cout << "Warmup: " << cfg.warmup << "\n";
  std::cout << "Rate: " << (cfg.rate > 0 ? std::to_string(cfg.rate) : "unlimited") << " msg/sec\n\n";

  Publisher publisher(cfg.shm_name, 16 * 1024 * 1024);

  GlobalStats stats;

  std::vector<std::thread> threads;

  threads.emplace_back(PublisherThread, std::ref(cfg), std::ref(stats), std::ref(publisher));

  for (int i = 0; i < cfg.subscriber_count; i++) {
    threads.emplace_back(SubscriberThread, std::ref(cfg), std::ref(stats), i);
  }

  for (auto& t : threads) {
    t.join();
  }

  PrintResults(cfg, stats);

  return 0;
}
