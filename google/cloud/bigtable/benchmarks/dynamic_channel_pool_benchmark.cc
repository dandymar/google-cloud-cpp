// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/bigtable/benchmarks/benchmark.h"
#include "google/cloud/bigtable/benchmarks/constants.h"
#include "google/cloud/bigtable/benchmarks/random_mutation.h"
#include "google/cloud/bigtable/instance_resource.h"
#include "google/cloud/bigtable/options.h"
#include "google/cloud/bigtable/table.h"
#include "google/cloud/bigtable/testing/random_names.h"
#include "google/cloud/completion_queue.h"
#include "google/cloud/future.h"
#include "google/cloud/grpc_options.h"
#include "google/cloud/internal/build_info.h"
#include "google/cloud/internal/make_status.h"
#include "google/cloud/internal/random.h"
#include "google/cloud/log.h"
#include "google/cloud/project.h"
#include "google/cloud/status.h"
#include "google/cloud/status_or.h"
#include "google/cloud/testing_util/command_line_parsing.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

char const kDescription[] =
    R"""(Benchmark for Cloud Bigtable C++ Dynamically Sized Channel Pool.

This benchmark implements the 3 test scenarios described in the design doc:
  1. Low-to-High Surge: Starts at low QPS (e.g., 1 QPS), surges to high concurrent
     load, and follows with a cooldown phase to verify scale-up, latency recovery,
     and scale-down.
  2. High-to-Low Drop: Starts at high QPS and suddenly drops to low QPS to verify
     graceful channel draining and idle connection reduction.
  3. Sustained High QPS: Sustained high load to compare throughput, latency, and
     channel efficiency against fixed channel pool baselines.

Detection & Reporting (Method #4):
  Reports sliding-window metrics (throughput, P50, P90, P99, P100 latency, errors,
  and active TCP port 443 socket count) every window interval (e.g. 5s, 10s or 30s).
)""";

namespace {

namespace bigtable = ::google::cloud::bigtable;
using ::google::cloud::CompletionQueue;
using ::google::cloud::future;
using ::google::cloud::GrpcNumChannelsOption;
using ::google::cloud::Options;
using ::google::cloud::Project;
using ::google::cloud::Status;
using ::google::cloud::StatusOr;
using ::google::cloud::bigtable::benchmarks::Benchmark;
using ::google::cloud::bigtable::benchmarks::BenchmarkOptions;
using ::google::cloud::bigtable::benchmarks::kColumnFamily;
using ::google::cloud::bigtable::Table;
using ::google::cloud::testing_util::BuildUsage;
using ::google::cloud::testing_util::OptionDescriptor;
using ::google::cloud::testing_util::OptionsParse;
using ::google::cloud::testing_util::ParseBoolean;
using ::google::cloud::testing_util::ParseDuration;

struct BenchmarkConfig {
  std::string project_id;
  std::string instance_id;
  std::string table_id = "";
  std::string app_profile_id = "default";

  std::string scenario = "surge";  // "surge", "drop", "sustained"

  std::chrono::seconds phase1_duration = std::chrono::seconds(30);
  std::chrono::seconds phase2_duration = std::chrono::seconds(60);
  std::chrono::seconds cooldown_duration = std::chrono::seconds(60);

  int low_qps = 1;
  int high_concurrency = 150;
  int cq_threads = 8;

  std::size_t min_channels = 4;
  std::size_t max_channels = 20;
  int min_rpcs = 1;
  int max_rpcs = 25;
  std::chrono::seconds cooldown_interval = std::chrono::seconds(60);
  std::chrono::seconds polling_interval = std::chrono::seconds(15);
  bool use_fixed_pool = false;

  std::chrono::seconds window_size = std::chrono::seconds(5);
  std::string csv_output = "";

  std::int64_t table_size = 100;
  bool exit_after_parse = false;
};

// Count active established TCP port 443 sockets belonging to this process.
int CountActiveTcp443Sockets() {
  std::set<unsigned long> socket_inodes;
  DIR* dir = opendir("/proc/self/fd");
  if (dir != nullptr) {
    struct dirent* entry;
    char link_buf[256];
    while ((entry = readdir(dir)) != nullptr) {
      if (entry->d_name[0] == '.') continue;
      std::string fd_path = std::string("/proc/self/fd/") + entry->d_name;
      ssize_t len = readlink(fd_path.c_str(), link_buf, sizeof(link_buf) - 1);
      if (len > 0) {
        link_buf[len] = '\0';
        std::string target(link_buf);
        if (target.rfind("socket:[", 0) == 0 && target.back() == ']') {
          auto inode_str = target.substr(8, target.size() - 9);
          char* end = nullptr;
          unsigned long inode = std::strtoul(inode_str.c_str(), &end, 10);
          if (end != inode_str.c_str()) {
            socket_inodes.insert(inode);
          }
        }
      }
    }
    closedir(dir);
  }

  if (socket_inodes.empty()) return 0;

  int count = 0;
  for (char const* net_path : {"/proc/net/tcp", "/proc/net/tcp6"}) {
    std::ifstream is(net_path);
    if (!is.is_open()) continue;
    std::string line;
    std::getline(is, line);  // Skip header
    while (std::getline(is, line)) {
      std::istringstream iss(line);
      std::string sl, local_addr, rem_addr, st, tx_rx, tr_tm, retrnsmt, uid, timeout;
      unsigned long inode = 0;
      if (iss >> sl >> local_addr >> rem_addr >> st >> tx_rx >> tr_tm >> retrnsmt >> uid >> timeout >> inode) {
        if (st == "01" && socket_inodes.find(inode) != socket_inodes.end()) {
          auto colon_pos = rem_addr.find(':');
          if (colon_pos != std::string::npos) {
            std::string port_hex = rem_addr.substr(colon_pos + 1);
            if (port_hex == "01BB" || port_hex == "01bb") {
              ++count;
            }
          }
        }
      }
    }
  }
  return count;
}

struct WindowRecord {
  std::string phase;
  int window_idx = 0;
  absl::Time start_time;
  absl::Time end_time;
  int completed_ops = 0;
  int errors = 0;
  double p50_ms = 0.0;
  double p90_ms = 0.0;
  double p99_ms = 0.0;
  double p100_ms = 0.0;
  double qps = 0.0;
  int active_sockets = 0;
};

class SlidingWindowTracker {
 public:
  SlidingWindowTracker(std::chrono::seconds window_size, std::string csv_file)
      : window_size_(window_size), csv_file_(std::move(csv_file)) {
    if (!csv_file_.empty()) {
      csv_stream_.open(csv_file_, std::ios::out | std::ios::trunc);
      if (csv_stream_.is_open()) {
        csv_stream_ << "timestamp_iso,elapsed_sec,phase,window_idx,requests,"
                       "qps,p50_ms,p90_ms,p99_ms,p100_ms,errors,active_sockets\n";
        csv_stream_.flush();
      }
    }
  }

  ~SlidingWindowTracker() {
    Stop();
    if (csv_stream_.is_open()) {
      csv_stream_.close();
    }
  }

  void SetPhase(std::string phase_name) {
    std::lock_guard<std::mutex> lk(mu_);
    current_phase_ = std::move(phase_name);
  }

  void RecordOperation(Status const& status, std::chrono::microseconds latency) {
    std::lock_guard<std::mutex> lk(mu_);
    if (status.ok()) {
      current_latencies_us_.push_back(latency.count());
    } else {
      if (logged_errors_ < 3) {
        std::cerr << "RPC FAILURE [" << current_phase_ << "]: " << status << "\n";
        ++logged_errors_;
      }
      ++current_errors_;
    }
    ++current_ops_;
  }

  void Start() {
    bench_start_time_ = absl::Now();
    window_start_time_ = bench_start_time_;
    running_ = true;
    worker_ = std::thread([this] { RunLoop(); });
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!running_) return;
      running_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    FlushCurrentWindow();
  }

  std::vector<WindowRecord> GetHistory() {
    std::lock_guard<std::mutex> lk(mu_);
    return history_;
  }

 private:
  void RunLoop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (running_) {
      cv_.wait_for(lk, window_size_, [this] { return !running_; });
      FlushCurrentWindowLocked();
    }
  }

  void FlushCurrentWindow() {
    std::lock_guard<std::mutex> lk(mu_);
    FlushCurrentWindowLocked();
  }

  void FlushCurrentWindowLocked() {
    auto now = absl::Now();
    auto elapsed = absl::ToDoubleSeconds(now - window_start_time_);
    if (elapsed <= 0.0) return;

    WindowRecord rec;
    rec.phase = current_phase_;
    rec.window_idx = ++window_index_;
    rec.start_time = window_start_time_;
    rec.end_time = now;
    rec.completed_ops = current_ops_;
    rec.errors = current_errors_;
    rec.qps = current_ops_ / elapsed;
    rec.active_sockets = CountActiveTcp443Sockets();

    if (!current_latencies_us_.empty()) {
      std::sort(current_latencies_us_.begin(), current_latencies_us_.end());
      auto get_p = [this](double p) {
        auto idx = static_cast<std::size_t>(p * (current_latencies_us_.size() - 1));
        return current_latencies_us_[idx] / 1000.0;
      };
      rec.p50_ms = get_p(0.50);
      rec.p90_ms = get_p(0.90);
      rec.p99_ms = get_p(0.99);
      rec.p100_ms = get_p(1.00);
    }

    history_.push_back(rec);

    // Formatted stdout row
    auto total_elapsed = absl::ToInt64Seconds(now - bench_start_time_);
    int mins = static_cast<int>(total_elapsed / 60);
    int secs = static_cast<int>(total_elapsed % 60);
    std::string time_str = absl::FormatTime("%H:%M:%S", now, absl::LocalTimeZone());

    std::cout << absl::StreamFormat(
        "[%s] [%-15s] Win #%03d (%02d:%02d) | Req: %6d | QPS: %7.1f | "
        "P50: %6.2fms | P90: %6.2fms | P99: %6.2fms | Max: %6.2fms | Err: %2d | Sockets: %2d\n",
        time_str, rec.phase, rec.window_idx, mins, secs, rec.completed_ops,
        rec.qps, rec.p50_ms, rec.p90_ms, rec.p99_ms, rec.p100_ms, rec.errors,
        rec.active_sockets);
    std::cout.flush();

    // CSV output
    if (csv_stream_.is_open()) {
      std::string iso_str = absl::FormatTime(absl::RFC3339_sec, now, absl::UTCTimeZone());
      csv_stream_ << absl::StreamFormat(
          "%s,%d,%s,%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d\n",
          iso_str, total_elapsed, rec.phase, rec.window_idx, rec.completed_ops,
          rec.qps, rec.p50_ms, rec.p90_ms, rec.p99_ms, rec.p100_ms, rec.errors,
          rec.active_sockets);
      csv_stream_.flush();
    }

    // Reset window counters
    current_ops_ = 0;
    current_errors_ = 0;
    current_latencies_us_.clear();
    window_start_time_ = now;
  }

  std::chrono::seconds window_size_;
  std::string csv_file_;
  std::ofstream csv_stream_;

  std::mutex mu_;
  std::condition_variable cv_;
  bool running_ = false;
  std::thread worker_;

  absl::Time bench_start_time_;
  absl::Time window_start_time_;
  std::string current_phase_ = "INIT";
  int window_index_ = 0;

  int current_ops_ = 0;
  int current_errors_ = 0;
  int logged_errors_ = 0;
  std::vector<std::int64_t> current_latencies_us_;
  std::vector<WindowRecord> history_;
};

StatusOr<BenchmarkConfig> ParseArgs(int argc, char* argv[]) {
  BenchmarkConfig cfg;
  bool wants_help = false;
  bool wants_desc = false;

  std::vector<OptionDescriptor> desc{
      {"--help", "print usage information",
       [&wants_help](std::string const&) { wants_help = true; }},
      {"--description", "print benchmark description",
       [&wants_desc](std::string const&) { wants_desc = true; }},
      {"--project-id", "the GCP Project ID",
       [&cfg](std::string const& val) { cfg.project_id = val; }},
      {"--instance-id", "the Cloud Bigtable Instance ID",
       [&cfg](std::string const& val) { cfg.instance_id = val; }},
      {"--table-id", "the Cloud Bigtable Table ID (auto-generated if not specified)",
       [&cfg](std::string const& val) { cfg.table_id = val; }},
      {"--app-profile-id", "the Application Profile ID (default: default)",
       [&cfg](std::string const& val) { cfg.app_profile_id = val; }},
      {"--scenario", "workload scenario: surge, drop, or sustained (default: surge)",
       [&cfg](std::string const& val) { cfg.scenario = val; }},
      {"--phase1-duration", "duration of phase 1 (e.g. 30s, 5m)",
       [&cfg](std::string const& val) { cfg.phase1_duration = ParseDuration(val); }},
      {"--phase2-duration", "duration of phase 2 (e.g. 60s, 10m)",
       [&cfg](std::string const& val) { cfg.phase2_duration = ParseDuration(val); }},
      {"--cooldown-duration", "duration of cooldown phase (e.g. 60s, 5m)",
       [&cfg](std::string const& val) { cfg.cooldown_duration = ParseDuration(val); }},
      {"--low-qps", "target QPS during low phase (default: 1)",
       [&cfg](std::string const& val) { cfg.low_qps = std::max(1, std::stoi(val)); }},
      {"--high-concurrency", "in-flight concurrent requests during high phase (default: 150)",
       [&cfg](std::string const& val) { cfg.high_concurrency = std::max(1, std::stoi(val)); }},
      {"--thread-count", "completion queue worker threads (default: 8)",
       [&cfg](std::string const& val) { cfg.cq_threads = std::max(1, std::stoi(val)); }},
      {"--min-channels", "minimum channel pool size (default: 4)",
       [&cfg](std::string const& val) { cfg.min_channels = std::stoul(val); }},
      {"--max-channels", "maximum channel pool size (default: 20)",
       [&cfg](std::string const& val) { cfg.max_channels = std::stoul(val); }},
      {"--min-rpcs", "minimum average RPCs per channel for downscale (default: 1)",
       [&cfg](std::string const& val) { cfg.min_rpcs = std::stoi(val); }},
      {"--max-rpcs", "maximum average RPCs per channel for upscale (default: 25)",
       [&cfg](std::string const& val) { cfg.max_rpcs = std::stoi(val); }},
      {"--cooldown", "pool size decrease cooldown interval (e.g. 60s, 120s)",
       [&cfg](std::string const& val) { cfg.cooldown_interval = ParseDuration(val); }},
      {"--polling-interval", "remove channel polling interval (e.g. 15s, 30s)",
       [&cfg](std::string const& val) { cfg.polling_interval = ParseDuration(val); }},
      {"--use-fixed-pool", "run with fixed channel pool size equal to max-channels",
       [&cfg](std::string const& val) {
         cfg.use_fixed_pool = ParseBoolean(val).value_or(true);
       }},
      {"--window-size", "reporting sliding window duration (e.g. 5s, 10s)",
       [&cfg](std::string const& val) { cfg.window_size = ParseDuration(val); }},
      {"--csv-output", "path to output windowed CSV file",
       [&cfg](std::string const& val) { cfg.csv_output = val; }},
      {"--table-size", "number of rows to populate in table (default: 100)",
       [&cfg](std::string const& val) { cfg.table_size = std::stol(val); }},
  };

  std::vector<std::string> argv_vec(argv, argv + argc);
  auto usage = BuildUsage(desc, argv[0]);
  auto unparsed = OptionsParse(desc, argv_vec);

  if (wants_help) {
    std::cout << usage << "\n";
    cfg.exit_after_parse = true;
    return cfg;
  }
  if (wants_desc) {
    std::cout << kDescription << "\n";
    cfg.exit_after_parse = true;
    return cfg;
  }

  auto make_status = [](std::string msg) {
    return google::cloud::internal::InvalidArgumentError(std::move(msg),
                                                         GCP_ERROR_INFO());
  };

  if (unparsed.size() != 1) {
    return make_status("Unknown arguments: " +
                       absl::StrJoin(std::next(unparsed.begin()), unparsed.end(), " "));
  }
  if (cfg.project_id.empty()) return make_status("Missing --project-id");
  if (cfg.instance_id.empty()) return make_status("Missing --instance-id");

  return cfg;
}

// Workload Controller
class DynamicChannelPoolWorkload {
 public:
  DynamicChannelPoolWorkload(BenchmarkConfig const& cfg, Table table,
                             Benchmark const& benchmark, SlidingWindowTracker& tracker)
      : cfg_(cfg), table_(std::move(table)), benchmark_(benchmark), tracker_(tracker) {}

  void RunLowQps(std::chrono::seconds duration, std::string const& phase_name) {
    tracker_.SetPhase(phase_name);
    std::cout << "=== Starting Phase: " << phase_name << " (Duration: "
              << duration.count() << "s, Rate: " << cfg_.low_qps << " QPS) ===" << std::endl;

    auto end_time = std::chrono::steady_clock::now() + duration;
    auto interval = std::chrono::microseconds(1000000 / std::max(1, cfg_.low_qps));
    google::cloud::internal::DefaultPRNG generator(std::random_device{}());

    while (std::chrono::steady_clock::now() < end_time) {
      auto start = std::chrono::steady_clock::now();
      auto key = benchmark_.MakeRandomKey(generator);

      auto row = table_.ReadRow(key, bigtable::Filter::ColumnRangeClosed(
                                         kColumnFamily, "field0", "field9"));
      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start);

      tracker_.RecordOperation(row.status(), elapsed);

      auto next_tick = start + interval;
      auto now = std::chrono::steady_clock::now();
      if (now < next_tick) {
        std::this_thread::sleep_for(next_tick - now);
      }
    }
  }

  void RunHighQps(std::chrono::seconds duration, std::string const& phase_name) {
    tracker_.SetPhase(phase_name);
    std::cout << "=== Starting Phase: " << phase_name << " (Duration: "
              << duration.count() << "s, Concurrency: " << cfg_.high_concurrency
              << " in-flight) ===" << std::endl;

    auto end_time = std::chrono::steady_clock::now() + duration;
    std::atomic<int> in_flight{0};
    std::mutex mu;
    std::condition_variable cv;
    bool draining = false;

    // Launch initial batch of concurrent async reads
    for (int i = 0; i < cfg_.high_concurrency; ++i) {
      ++in_flight;
      LaunchAsyncRead(end_time, in_flight, draining, mu, cv);
    }

    // Wait until end_time and all in-flight requests finish
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&] { return in_flight == 0; });
  }

  void RunCooldown(std::chrono::seconds duration, std::string const& phase_name) {
    tracker_.SetPhase(phase_name);
    std::cout << "=== Starting Phase: " << phase_name << " (Duration: "
              << duration.count() << "s, Cooldown Draining) ===" << std::endl;

    // During cooldown, keep an occasional trickle (1 op every 2s) so the client's
    // CheckPoolChannelHealth() is regularly invoked to evaluate downscale conditions
    // and drain idle channels.
    auto end_time = std::chrono::steady_clock::now() + duration;
    auto interval = std::chrono::seconds(2);
    google::cloud::internal::DefaultPRNG generator(std::random_device{}());

    while (std::chrono::steady_clock::now() < end_time) {
      auto start = std::chrono::steady_clock::now();
      auto key = benchmark_.MakeRandomKey(generator);

      auto row = table_.ReadRow(key, bigtable::Filter::ColumnRangeClosed(
                                         kColumnFamily, "field0", "field9"));
      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start);

      tracker_.RecordOperation(row.status(), elapsed);

      auto next_tick = start + interval;
      auto now = std::chrono::steady_clock::now();
      if (now < next_tick) {
        std::this_thread::sleep_for(next_tick - now);
      }
    }
  }

 private:
  void LaunchAsyncRead(
      std::chrono::steady_clock::time_point end_time,
      std::atomic<int>& in_flight, bool& draining,
      std::mutex& mu, std::condition_variable& cv) {
    thread_local google::cloud::internal::DefaultPRNG generator(std::random_device{}());
    auto key = benchmark_.MakeRandomKey(generator);

    auto req_start = std::chrono::steady_clock::now();
    table_.AsyncReadRow(std::move(key), bigtable::Filter::ColumnRangeClosed(
                                            kColumnFamily, "field0", "field9"))
        .then([this, end_time, req_start, &in_flight, &draining, &mu, &cv](
                  future<StatusOr<std::pair<bool, bigtable::Row>>> f) {
          auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - req_start);
          auto status = f.get().status();
          tracker_.RecordOperation(status, elapsed);

          bool should_continue = (std::chrono::steady_clock::now() < end_time);
          if (should_continue) {
            LaunchAsyncRead(end_time, in_flight, draining, mu, cv);
          } else {
            if (--in_flight == 0) {
              std::lock_guard<std::mutex> lk(mu);
              draining = true;
              cv.notify_all();
            }
          }
        });
  }

  BenchmarkConfig const& cfg_;
  Table table_;
  Benchmark const& benchmark_;
  SlidingWindowTracker& tracker_;
};

void PrintSummary(std::vector<WindowRecord> const& history, BenchmarkConfig const& cfg) {
  std::cout << "\n=======================================================================\n";
  std::cout << "                 BENCHMARK EXECUTION SUMMARY                           \n";
  std::cout << "=======================================================================\n";
  std::cout << "Scenario: " << cfg.scenario << " | Mode: "
            << (cfg.use_fixed_pool ? "FIXED_POOL" : "DYNAMIC_CHANNEL_POOL") << "\n";
  std::cout << "Pool Configuration: min=" << cfg.min_channels
            << ", max=" << cfg.max_channels
            << ", min_rpcs=" << cfg.min_rpcs
            << ", max_rpcs=" << cfg.max_rpcs
            << ", cooldown=" << cfg.cooldown_interval.count() << "s"
            << ", poll=" << cfg.polling_interval.count() << "s\n\n";

  std::cout << absl::StreamFormat("%-16s | %5s | %8s | %7s | %8s | %8s | %8s | %7s\n",
                                  "Phase", "Wins", "TotalReq", "MeanQPS", "P50(ms)", "P99(ms)", "Max(ms)", "Sockets");
  std::cout << "-----------------------------------------------------------------------\n";

  std::map<std::string, std::vector<WindowRecord>> phase_groups;
  for (auto const& r : history) {
    phase_groups[r.phase].push_back(r);
  }

  int min_sockets_seen = 999999;
  int max_sockets_seen = 0;

  for (auto const& [phase, records] : phase_groups) {
    int total_ops = 0;
    int total_err = 0;
    double sum_p50 = 0.0;
    double sum_p99 = 0.0;
    double max_lat = 0.0;
    int phase_min_sock = 999999;
    int phase_max_sock = 0;

    for (auto const& r : records) {
      total_ops += r.completed_ops;
      total_err += r.errors;
      sum_p50 += r.p50_ms;
      sum_p99 += r.p99_ms;
      max_lat = std::max(max_lat, r.p100_ms);
      phase_min_sock = std::min(phase_min_sock, r.active_sockets);
      phase_max_sock = std::max(phase_max_sock, r.active_sockets);
      min_sockets_seen = std::min(min_sockets_seen, r.active_sockets);
      max_sockets_seen = std::max(max_sockets_seen, r.active_sockets);
    }
    double avg_p50 = records.empty() ? 0.0 : (sum_p50 / records.size());
    double avg_p99 = records.empty() ? 0.0 : (sum_p99 / records.size());
    double mean_qps = records.empty() ? 0.0 : (static_cast<double>(total_ops) / (records.size() * cfg.window_size.count()));

    std::string sock_range = absl::StrFormat("%d..%d", phase_min_sock, phase_max_sock);
    std::cout << absl::StreamFormat("%-16s | %5d | %8d | %7.1f | %8.2f | %8.2f | %8.2f | %7s\n",
                                    phase, records.size(), total_ops, mean_qps, avg_p50, avg_p99, max_lat, sock_range);
  }
  std::cout << "=======================================================================\n";

  if (!cfg.use_fixed_pool) {
    std::cout << "\n--- Dynamic Channel Pool Scaling Verification ---\n";
    std::cout << "Min Sockets Observed: " << min_sockets_seen
              << " (Expected near: " << cfg.min_channels << ")\n";
    std::cout << "Max Sockets Observed: " << max_sockets_seen
              << " (Target range: up to " << cfg.max_channels << ")\n";
    if (max_sockets_seen > min_sockets_seen) {
      std::cout << "[PASS] Socket count dynamically expanded under load ("
                << min_sockets_seen << " -> " << max_sockets_seen << ").\n";
    } else {
      std::cout << "[NOTE] Socket count did not scale or load was handled within initial channels.\n";
    }
  }
}

}  // anonymous namespace

int main(int argc, char* argv[]) {
  google::cloud::LogSink::EnableStdClog(google::cloud::Severity::GCP_LS_INFO);

  auto cfg = ParseArgs(argc, argv);
  if (!cfg) {
    std::cerr << "Configuration error: " << cfg.status() << "\n";
    return 1;
  }
  if (cfg->exit_after_parse) return 0;

  BenchmarkOptions b_options;
  b_options.project_id = cfg->project_id;
  b_options.instance_id = cfg->instance_id;
  auto generator = google::cloud::internal::MakeDefaultPRNG();
  b_options.table_id = cfg->table_id.empty()
      ? bigtable::testing::RandomTableId(generator)
      : cfg->table_id;
  b_options.table_size = std::max<std::int64_t>(100, cfg->table_size);
  b_options.app_profile_id = cfg->app_profile_id;
  b_options.thread_count = cfg->cq_threads;

  Benchmark benchmark(b_options);

  bool created_table = false;
  if (cfg->table_id.empty()) {
    std::cout << "--> Creating ephemeral test table: " << b_options.table_id << "...\n";
    benchmark.CreateTable();
    created_table = true;
    std::cout << "--> Populating test table (" << b_options.table_size << " rows)...\n";
    auto pop_res = benchmark.PopulateTable();
    if (!pop_res) {
      std::cerr << "Failed to populate table: " << pop_res.status() << "\n";
      benchmark.DeleteTable();
      return 1;
    }
    std::cout << "--> Table created and populated successfully.\n";
  }

  std::cout << "Starting Cloud Bigtable Dynamic Channel Pool Benchmark\n";
  std::cout << "  Project: " << cfg->project_id << "\n";
  std::cout << "  Instance: " << cfg->instance_id << "\n";
  std::cout << "  Table: " << b_options.table_id << "\n";
  std::cout << "  Scenario: " << cfg->scenario << "\n";
  std::cout << "  Fixed Pool: " << (cfg->use_fixed_pool ? "true" : "false") << "\n";
  std::cout << "  Window Size: " << cfg->window_size.count() << "s\n";
  if (!cfg->csv_output.empty()) {
    std::cout << "  CSV Output: " << cfg->csv_output << "\n";
  }

  // Setup CompletionQueue worker threads
  CompletionQueue cq;
  std::vector<std::thread> cq_threads;
  for (int i = 0; i < cfg->cq_threads; ++i) {
    cq_threads.emplace_back([cq]() mutable { cq.Run(); });
  }

  Options client_opts;
  client_opts.set<bigtable::AppProfileIdOption>(cfg->app_profile_id);

  if (!cfg->use_fixed_pool) {
    bigtable::experimental::DynamicChannelPoolSizingPolicy policy;
    policy.minimum_channel_pool_size = cfg->min_channels;
    policy.maximum_channel_pool_size = cfg->max_channels;
    policy.minimum_average_outstanding_rpcs_per_channel = cfg->min_rpcs;
    policy.maximum_average_outstanding_rpcs_per_channel = cfg->max_rpcs;
    policy.pool_size_decrease_cooldown_interval = cfg->cooldown_interval;
    policy.remove_channel_polling_interval = cfg->polling_interval;
    client_opts.set<bigtable::experimental::DynamicChannelPoolSizingPolicyOption>(policy);
    client_opts.set<GrpcNumChannelsOption>(static_cast<int>(cfg->min_channels));
  } else {
    // Negative Case / Fixed Pool:
    // Explicitly lock the pool by clamping minimum and maximum channel counts
    // to min_channels and setting max_rpcs to INT_MAX to strictly disable scaling.
    auto fixed_size = cfg->min_channels;
    bigtable::experimental::DynamicChannelPoolSizingPolicy policy;
    policy.minimum_channel_pool_size = fixed_size;
    policy.maximum_channel_pool_size = fixed_size;
    policy.maximum_average_outstanding_rpcs_per_channel = std::numeric_limits<int>::max();
    client_opts.set<bigtable::experimental::DynamicChannelPoolSizingPolicyOption>(policy);
    client_opts.set<GrpcNumChannelsOption>(static_cast<int>(fixed_size));
  }

  auto table = benchmark.MakeTable(client_opts);

  SlidingWindowTracker tracker(cfg->window_size, cfg->csv_output);
  tracker.Start();

  DynamicChannelPoolWorkload workload(*cfg, std::move(table), benchmark, tracker);

  if (cfg->scenario == "surge") {
    workload.RunLowQps(cfg->phase1_duration, "1_LOW_QPS");
    workload.RunHighQps(cfg->phase2_duration, "2_HIGH_SURGE");
    workload.RunCooldown(cfg->cooldown_duration, "3_COOLDOWN");
  } else if (cfg->scenario == "drop") {
    workload.RunHighQps(cfg->phase1_duration, "1_HIGH_LOAD");
    workload.RunLowQps(cfg->phase2_duration, "2_LOW_DROP");
    workload.RunCooldown(cfg->cooldown_duration, "3_COOLDOWN");
  } else if (cfg->scenario == "sustained") {
    workload.RunHighQps(cfg->phase1_duration, "1_SUSTAINED_HIGH");
  } else {
    std::cerr << "Unknown scenario: " << cfg->scenario << "\n";
  }

  tracker.Stop();
  cq.Shutdown();
  cq.CancelAll();
  for (auto& t : cq_threads) {
    if (t.joinable()) t.join();
  }

  PrintSummary(tracker.GetHistory(), *cfg);

  if (created_table) {
    std::cout << "--> Cleaning up ephemeral table...\n";
    benchmark.DeleteTable();
  }

  return 0;
}
