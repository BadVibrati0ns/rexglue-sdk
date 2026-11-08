/**
 * @file        core/perf/counter.cpp
 * @brief       Performance counter registry implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/perf/counter.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <vector>
#include <atomic>
#include <cstdio>
#include <cstring>

REXCVAR_DEFINE_STRING(perf_ab_cvar, "", "Perf",
                      "Boolean setting to flip every perf_ab_frames frames, with its state "
                      "logged in the ab_state column of perf_log_csv");
REXCVAR_DEFINE_INT32(perf_ab_frames, 60, "Perf", "Frames between flips of perf_ab_cvar");
REXCVAR_DEFINE_STRING(perf_ab_values, "", "Perf",
                      "Two values separated by a colon for a non-boolean perf_ab_cvar: state 0 "
                      "is the first, state 1 the second");
REXCVAR_DEFINE_STRING(perf_log_csv, "", "Perf",
                      "Path to write per-frame CSV log (empty = disabled)");

namespace rex::perf {

namespace {

constexpr size_t kNumCounters = static_cast<size_t>(CounterId::kCount);

// One cache line per counter: they are updated from many threads (guest
// threads, the GPU command thread), and sharing lines made updates contend.
struct alignas(64) PaddedCounter : std::atomic<int64_t> {};
std::array<PaddedCounter, kNumCounters> g_counters{};
std::array<std::atomic<int64_t>, kNumCounters> g_snapshot{};

constexpr const char* kCounterNames[] = {
    "frame_time_us",
    "fps",
    "draw_calls",
    "command_buffer_stalls",
    "vertices_processed",
    "xma_frames_decoded",
    "audio_frame_latency_us",
    "buffer_queue_depth",
    "functions_dispatched",
    "interrupt_dispatches",
    "active_threads",
    "apc_queue_depth",
    "critical_region_contentions",
    "texture_cache_hits",
    "texture_cache_misses",
    "pipeline_cache_hits",
    "pipeline_cache_misses",
    "marker",
    "gpu_busy_ns",
    "gpu_wait_ns",
    "page_protect_calls",
    "page_faults",
    "ab_state",
};
static_assert(std::size(kCounterNames) == kNumCounters, "kCounterNames must match CounterId enum");

// Gauge counters are snapshotted but NOT zeroed each frame.
// Accumulators (everything else) are zeroed after snapshot.
constexpr bool kIsGauge[] = {
    false,  // kFrameTimeUs       (set each frame)
    false,  // kFps               (set each frame)
    false,  // kDrawCalls
    false,  // kCommandBufferStalls
    false,  // kVerticesProcessed
    false,  // kXmaFramesDecoded
    false,  // kAudioFrameLatencyUs
    false,  // kBufferQueueDepth  (set each frame)
    false,  // kFunctionsDispatched
    false,  // kInterruptDispatches
    true,   // kActiveThreads     (inc/dec over lifetime)
    false,  // kApcQueueDepth
    true,   // kCriticalRegionContentions (running total)
    false,  // kTextureCacheHits
    false,  // kTextureCacheMisses
    false,  // kPipelineCacheHits
    false,  // kPipelineCacheMisses
    false,  // kMarker            (set on the frame it happens)
    false,  // kGpuBusyNs
    false,  // kGpuWaitNs
    false,  // kPageProtectCalls
    false,  // kPageFaults
    true,   // kAbState
};
static_assert(std::size(kIsGauge) == kNumCounters, "kIsGauge must match CounterId enum");

// CSV state
std::FILE* g_csv_file = nullptr;
std::string g_csv_path;
int g_csv_frame_count = 0;

}  // anonymous namespace

const char* CounterName(CounterId id) {
  auto idx = static_cast<size_t>(id);
  if (idx < kNumCounters)
    return kCounterNames[idx];
  return "unknown";
}

namespace {

struct TimelineEntry {
  int64_t ns;
  uint32_t thread;
  uint8_t event;
  int8_t phase;
};
constexpr size_t kTimelineCapacity = size_t(1) << 22;
std::string g_timeline_path;
std::vector<TimelineEntry> g_timeline;
std::atomic<size_t> g_timeline_count{0};
std::atomic<bool> g_timeline_active{false};
std::atomic<uint32_t> g_timeline_threads{0};
thread_local uint32_t t_timeline_thread = UINT32_MAX;

}  // namespace

void TimelineSetPath(const std::string& path) {
  g_timeline_path = path;
  g_timeline.resize(kTimelineCapacity);
}

void TimelineSetActive(bool active) {
  if (!g_timeline_path.empty()) {
    g_timeline_active.store(active, std::memory_order_release);
  }
}

bool TimelineActive() { return g_timeline_active.load(std::memory_order_relaxed); }

void TimelineRecord(TimelineEvent event, int phase) {
  size_t index = g_timeline_count.fetch_add(1, std::memory_order_relaxed);
  if (index >= kTimelineCapacity) {
    return;
  }
  if (t_timeline_thread == UINT32_MAX) {
    t_timeline_thread = g_timeline_threads.fetch_add(1, std::memory_order_relaxed);
  }
  g_timeline[index] = {std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count(),
                       t_timeline_thread, uint8_t(event), int8_t(phase)};
}

void TimelineWrite() {
  if (g_timeline_path.empty()) {
    return;
  }
  g_timeline_active.store(false, std::memory_order_release);
  static const char* const kNames[] = {"gpu_idle", "gpu_wait_reg_mem", "gpu_fence_wait",
                                       "gpu_swap", "guest_kick",       "guest0",
                                       "guest1",   "guest2",           "guest3",
                                       "guest4",   "guest5",           "vblank_interrupt",
                                       "cp_interrupt"};
  static_assert(std::size(kNames) == size_t(TimelineEvent::kCount));
  FILE* file = std::fopen(g_timeline_path.c_str(), "w");
  if (!file) {
    return;
  }
  std::fprintf(file, "ns,thread,event,phase\n");
  size_t count = std::min(g_timeline_count.load(), kTimelineCapacity);
  for (size_t i = 0; i < count; ++i) {
    const TimelineEntry& entry = g_timeline[i];
    if (!entry.ns) {
      continue;
    }
    std::fprintf(file, "%lld,%u,%s,%d\n", static_cast<long long>(entry.ns), entry.thread,
                 entry.event < std::size(kNames) ? kNames[entry.event] : "?", entry.phase);
  }
  std::fclose(file);
  g_timeline_path.clear();
}

void SetCounter(CounterId id, int64_t value) {
  g_counters[static_cast<size_t>(id)].store(value, std::memory_order_relaxed);
}

void IncrementCounter(CounterId id, int64_t delta) {
  g_counters[static_cast<size_t>(id)].fetch_add(delta, std::memory_order_relaxed);
}

int64_t GetCounter(CounterId id) {
  return g_counters[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

void ResetFrameCounters() {
  for (size_t i = 0; i < kNumCounters; ++i) {
    if (kIsGauge[i]) {
      // Gauges: snapshot the current value, don't zero
      g_snapshot[i].store(g_counters[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    } else {
      // Accumulators: snapshot and zero for next frame
      g_snapshot[i].store(g_counters[i].exchange(0, std::memory_order_relaxed),
                          std::memory_order_relaxed);
    }
  }
  // Flip the A/B setting for the next frames.
  const std::string& ab_cvar = REXCVAR_GET(perf_ab_cvar);
  if (!ab_cvar.empty() && rex::cvar::GetFlagInfo(ab_cvar)) {
    static int64_t ab_frames = 0;
    const std::string& ab_values = REXCVAR_GET(perf_ab_values);
    size_t ab_comma = ab_values.find(':');
    static bool ab_state =
        ab_comma == std::string::npos ? rex::cvar::Query<bool>(ab_cvar) : false;
    if (ab_frames == 0) {
      if (ab_comma != std::string::npos) {
        rex::cvar::SetFlagByName(ab_cvar, ab_values.substr(0, ab_comma));
      }
      g_counters[size_t(CounterId::kAbState)].store(ab_state, std::memory_order_relaxed);
    }
    if (++ab_frames % std::max(REXCVAR_GET(perf_ab_frames), 1) == 0) {
      ab_state = !ab_state;
      if (ab_comma != std::string::npos) {
        rex::cvar::SetFlagByName(
            ab_cvar, ab_state ? ab_values.substr(ab_comma + 1) : ab_values.substr(0, ab_comma));
      } else {
        rex::cvar::SetFlagByName(ab_cvar, ab_state ? "true" : "false");
      }
      g_counters[size_t(CounterId::kAbState)].store(ab_state, std::memory_order_relaxed);
    }
  }
#ifdef REXGLUE_ENABLE_PROFILING
  // Show each frame's values as Tracy plots next to the frame timeline.
  if (tracy::IsProfilerStarted()) {
    for (size_t i = 0; i < kNumCounters; ++i) {
      TracyPlot(kCounterNames[i], g_snapshot[i].load(std::memory_order_relaxed));
    }
  }
#endif
}

int64_t GetSnapshotCounter(CounterId id) {
  return g_snapshot[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

void Init() {
  for (auto& c : g_counters)
    c.store(0, std::memory_order_relaxed);
  for (auto& s : g_snapshot)
    s.store(0, std::memory_order_relaxed);
}

void SetCsvLogPath(const std::string& path) {
  if (g_csv_file) {
    std::fflush(g_csv_file);
    std::fclose(g_csv_file);
    g_csv_file = nullptr;
  }
  g_csv_path = path;
  g_csv_frame_count = 0;

  if (path.empty())
    return;

  g_csv_file = rex::filesystem::OpenFile(rex::to_path(path), "w");
  if (!g_csv_file) {
    REXLOG_WARN("perf: failed to open CSV log: {}", path);
    g_csv_path.clear();
    return;
  }

  // Write header
  for (size_t i = 0; i < kNumCounters; ++i) {
    if (i > 0)
      std::fputc(',', g_csv_file);
    std::fputs(kCounterNames[i], g_csv_file);
  }
  std::fputc('\n', g_csv_file);
}

void WriteCsvFrame() {
  if (!g_csv_file)
    return;

  for (size_t i = 0; i < kNumCounters; ++i) {
    if (i > 0)
      std::fputc(',', g_csv_file);
    std::fprintf(g_csv_file, "%lld",
                 static_cast<long long>(g_snapshot[i].load(std::memory_order_relaxed)));
  }
  std::fputc('\n', g_csv_file);

  if (++g_csv_frame_count % 60 == 0) {
    std::fflush(g_csv_file);
  }
}

void FlushCsv() {
  if (g_csv_file) {
    std::fflush(g_csv_file);
    std::fclose(g_csv_file);
    g_csv_file = nullptr;
  }
  g_csv_path.clear();
}

}  // namespace rex::perf
