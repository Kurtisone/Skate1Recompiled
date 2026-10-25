// skate1 - periodic CPU usage log, for finding what limits the frame rate on
// a device without a profiler (Linux and Android).
//
// With perf_log_seconds > 0, every that many seconds the log gets the busiest
// threads (CPU % of one core, and the core each one last ran on) and every
// core's current clock. On big.LITTLE SoCs a game thread stuck on a little
// core shows up here.

#include <rex/platform.h>
#if REX_PLATFORM_LINUX

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(perf_ab_plan, "", "Skate1",
                      "A/B test of hot-reloadable settings: variants separated by ';', each a "
                      "list of name=value separated by ','; an empty variant is the baseline. "
                      "Each runs perf_ab_seconds, the whole plan twice, starting "
                      "perf_ab_start_seconds after launch; switches are logged");
REXCVAR_DEFINE_INT32(perf_ab_seconds, 30, "Skate1", "Seconds per A/B variant").range(5, 3600);
REXCVAR_DEFINE_INT32(perf_ab_start_seconds, 120, "Skate1",
                     "Seconds after launch before the A/B test starts")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(perf_log_seconds, 0, "Skate1",
                     "Every this many seconds, log the busiest threads (CPU %, core) and the "
                     "core clocks (0 = off)")
    .range(0, 3600);

namespace {

struct ThreadSample {
  std::string name;
  uint64_t ticks = 0;
  int core = -1;
};

// /proc/self/task/<tid>/stat: "<tid> (<comm>) <state> ..." with utime and
// stime the 12th/13th and the last CPU the 37th field after the comm.
std::map<int, ThreadSample> ReadThreads() {
  std::map<int, ThreadSample> threads;
  DIR* tasks = opendir("/proc/self/task");
  if (!tasks) {
    return threads;
  }
  while (dirent* entry = readdir(tasks)) {
    int tid = atoi(entry->d_name);
    if (tid <= 0) {
      continue;
    }
    std::ifstream stat_file("/proc/self/task/" + std::string(entry->d_name) + "/stat");
    std::string line;
    if (!std::getline(stat_file, line)) {
      continue;
    }
    size_t open = line.find('(');
    size_t close = line.rfind(')');
    if (open == std::string::npos || close == std::string::npos) {
      continue;
    }
    ThreadSample sample;
    sample.name = line.substr(open + 1, close - open - 1);
    std::vector<std::string> fields;
    size_t pos = close + 2;
    while (pos < line.size()) {
      size_t space = line.find(' ', pos);
      if (space == std::string::npos) {
        space = line.size();
      }
      fields.push_back(line.substr(pos, space - pos));
      pos = space + 1;
    }
    if (fields.size() > 36) {
      sample.ticks = std::stoull(fields[11]) + std::stoull(fields[12]);
      sample.core = std::stoi(fields[36]);
    }
    threads[tid] = sample;
  }
  closedir(tasks);
  return threads;
}

std::string ReadCoreClocks() {
  std::string clocks;
  for (int core = 0; core < 16; ++core) {
    std::ifstream freq_file("/sys/devices/system/cpu/cpu" + std::to_string(core) +
                            "/cpufreq/scaling_cur_freq");
    uint64_t khz = 0;
    if (!(freq_file >> khz)) {
      if (core >= 8) {
        break;
      }
      continue;
    }
    clocks += " " + std::to_string(core) + ":" + std::to_string(khz / 1000);
  }
  return clocks;
}

void PerfLogLoop() {
  std::map<int, ThreadSample> previous;
  auto previous_time = std::chrono::steady_clock::now();
  int waited = 0;
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    int interval = REXCVAR_GET(perf_log_seconds);
    if (interval <= 0 || ++waited < interval) {
      if (interval <= 0) {
        previous.clear();
        waited = 0;
      }
      continue;
    }
    waited = 0;
    auto now = std::chrono::steady_clock::now();
    auto current = ReadThreads();
    if (!previous.empty()) {
      double seconds = std::chrono::duration<double>(now - previous_time).count();
      double ticks_per_second = double(sysconf(_SC_CLK_TCK));
      struct Usage {
        double percent;
        const ThreadSample* sample;
      };
      std::vector<Usage> usage;
      double total = 0;
      for (const auto& [tid, sample] : current) {
        auto it = previous.find(tid);
        if (it == previous.end() || sample.ticks < it->second.ticks) {
          continue;
        }
        double percent =
            100.0 * double(sample.ticks - it->second.ticks) / ticks_per_second / seconds;
        total += percent;
        usage.push_back({percent, &sample});
      }
      std::sort(usage.begin(), usage.end(),
                [](const Usage& a, const Usage& b) { return a.percent > b.percent; });
      std::string top;
      for (size_t i = 0; i < usage.size() && i < 10; ++i) {
        char buffer[96];
        snprintf(buffer, sizeof(buffer), " %s=%.0f%%@%d", usage[i].sample->name.c_str(),
                 usage[i].percent, usage[i].sample->core);
        top += buffer;
      }
      REXLOG_INFO("perf: process {:.0f}% |{}", total, top);
      REXLOG_INFO("perf: core MHz{}", ReadCoreClocks());
    }
    previous = std::move(current);
    previous_time = now;
  }
}

std::vector<std::string> Split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    size_t end = text.find(separator, start);
    parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) {
      return parts;
    }
    start = end + 1;
  }
}

void PerfAbLoop() {
  // Wait for the config to be loaded and the player to reach the scene.
  std::this_thread::sleep_for(std::chrono::seconds(5));
  std::string plan = REXCVAR_GET(perf_ab_plan);
  if (plan.empty()) {
    return;
  }
  std::vector<std::vector<std::pair<std::string, std::string>>> variants;
  std::map<std::string, std::string> baseline;
  for (const std::string& variant_text : Split(plan, ';')) {
    std::vector<std::pair<std::string, std::string>> variant;
    for (const std::string& assignment : Split(variant_text, ',')) {
      size_t equals = assignment.find('=');
      if (equals == std::string::npos) {
        continue;
      }
      std::string name = assignment.substr(0, equals);
      variant.emplace_back(name, assignment.substr(equals + 1));
      if (!baseline.count(name)) {
        baseline[name] = rex::cvar::GetFlagByName(name);
      }
    }
    variants.push_back(std::move(variant));
  }
  std::this_thread::sleep_for(std::chrono::seconds(std::max(0, REXCVAR_GET(perf_ab_start_seconds) - 5)));
  for (int round = 0; round < 2; ++round) {
    for (size_t i = 0; i < variants.size(); ++i) {
      for (const auto& [name, value] : baseline) {
        rex::cvar::SetFlagByName(name, value);
      }
      std::string description;
      for (const auto& [name, value] : variants[i]) {
        rex::cvar::SetFlagByName(name, value);
        description += " " + name + "=" + value;
      }
      REXLOG_INFO("perf-ab: round {} variant {}:{}", round, i,
                  description.empty() ? " baseline" : description);
      std::this_thread::sleep_for(std::chrono::seconds(REXCVAR_GET(perf_ab_seconds)));
    }
  }
  for (const auto& [name, value] : baseline) {
    rex::cvar::SetFlagByName(name, value);
  }
  REXLOG_INFO("perf-ab: done, settings restored");
}

// Started at load; idles until perf_log_seconds is set.
struct PerfLogStarter {
  PerfLogStarter() {
    std::thread(PerfLogLoop).detach();
    std::thread(PerfAbLoop).detach();
  }
} perf_log_starter;

}  // namespace

#endif  // REX_PLATFORM_LINUX
