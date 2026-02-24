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
#include <mutex>
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

struct LatencyStats {
  std::vector<uint64_t> samples;
  uint64_t min = 0;
  uint64_t max = 0;
  uint64_t avg = 0;
  uint64_t p50 = 0;
  uint64_t p99 = 0;
  uint64_t p999 = 0;
};

struct SubscriberStats {
  int id = 0;
  LatencyStats latency;
};

struct GlobalStats {
  std::atomic<uint64_t> messages_published{0};
  std::atomic<bool> publisher_done{false};
  std::vector<SubscriberStats> subscriber_stats;
  std::mutex latency_mutex;
  std::vector<uint64_t> received_counts;
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

void ComputeLatencyStats(LatencyStats& stats) {
  if (stats.samples.empty()) {
    return;
  }
  std::sort(stats.samples.begin(), stats.samples.end());
  stats.min = stats.samples.front();
  stats.max = stats.samples.back();
  uint64_t sum = std::accumulate(stats.samples.begin(), stats.samples.end(), 0ULL);
  stats.avg = sum / stats.samples.size();
  size_t idx50 = stats.samples.size() * 50 / 100;
  size_t idx99 = stats.samples.size() * 99 / 100;
  size_t idx999 = stats.samples.size() * 999 / 1000;
  stats.p50 = stats.samples[idx50];
  stats.p99 = stats.samples[idx99];
  stats.p999 = stats.samples[idx999];
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

  auto& sub_stats = stats.subscriber_stats[subscriber_id];
  sub_stats.id = subscriber_id;

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

      {
        std::lock_guard<std::mutex> lock(stats.latency_mutex);
        sub_stats.latency.samples.push_back(latency_ns);
      }

      received++;
      stats.received_counts[subscriber_id]++;

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
  std::cout << "----------|----------------|----------------|---------------|---------------|---------------|-------------\n";

  uint64_t total_received = 0;
  for (int i = 0; i < cfg.subscriber_count; i++) {
    ComputeLatencyStats(stats.subscriber_stats[i].latency);
    total_received += stats.received_counts[i];

    std::cout << "Sub #" << stats.subscriber_stats[i].id << "    |";
    std::cout << " " << std::setw(12) << stats.subscriber_stats[i].latency.min << " |";
    std::cout << " " << std::setw(12) << stats.subscriber_stats[i].latency.max << " |";
    std::cout << " " << std::setw(12) << stats.subscriber_stats[i].latency.avg << " |";
    std::cout << " " << std::setw(12) << stats.subscriber_stats[i].latency.p50 << " |";
    std::cout << " " << std::setw(12) << stats.subscriber_stats[i].latency.p99 << " |";
    std::cout << " " << std::setw(12) << stats.subscriber_stats[i].latency.p999 << "\n";
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

  Publisher publisher(cfg.shm_name, 1024 * 1024);

  GlobalStats stats;
  stats.subscriber_stats.resize(cfg.subscriber_count);
  stats.received_counts.resize(cfg.subscriber_count);

  for (int i = 0; i < cfg.subscriber_count; i++) {
    stats.subscriber_stats[i].id = i;
  }

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
