/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <utility>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/shared_memory.h>
#include <rex/math.h>
#include <rex/memory.h>

REXCVAR_DEFINE_UINT32(shared_memory_upload_window_kb, 64, "GPU/Optimizaciones",
                      "Widen shared memory uploads to invalid, previously requested pages in "
                      "aligned windows of this size, so fewer pages are re-protected one by one "
                      "(0 = upload only the requested pages)");
REXCVAR_DEFINE_UINT32(shared_memory_hot_page_uploads, 4, "GPU/Optimizaciones",
                      "Uploads after which a page the CPU keeps rewriting is no longer "
                      "write-protected, and is checked by comparing its contents instead "
                      "(0 = always protect)");
REXCVAR_DEFINE_BOOL(shared_memory_skip_valid_page_protection, true, "GPU/Optimizaciones",
                    "Don't protect again GPU-written pages that are still valid and not hot, as "
                    "any CPU write to them would have invalidated them");
REXCVAR_DEFINE_BOOL(shared_memory_hot_compare_requested_bytes, true, "GPU/Optimizaciones",
                    "Compare only the requested bytes of a hot page with its last upload, "
                    "rather than the whole page, on every request")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(shared_memory_relaxed_submission_uploads, true, "GPU/Optimizaciones",
                    "Upload a hot page already used in the submission at the submission start "
                    "too, if the bytes the GPU used from it have not changed since its last "
                    "upload, instead of ending the render pass for an upload in order")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(shared_memory_upload_at_submission_start, true, "GPU/Optimizaciones",
                    "Upload pages no command of the current submission has used yet at the "
                    "start of the submission, so the uploads don't interrupt render passes");
REXCVAR_DEFINE_BOOL(shared_memory_invalidation_stop_at_watches, true, "GPU/Optimizaciones",
                    "When widening the range invalidated by a CPU write, stop at pages watched "
                    "by ranges (textures) that the written pages don't overlap, instead of "
                    "invalidating and reloading those textures");

#include <atomic>
#include <array>
#include <chrono>
#include <map>
#include <rex/perf/counter.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

REXCVAR_DEFINE_BOOL(shared_memory_log_writers, false, "GPU/Debug",
                    "Log the guest functions whose writes invalidate GPU-used memory (set "
                    "shared_memory_hot_page_uploads=0 so every write is seen)");
REXCVAR_DEFINE_INT32(shared_memory_log_writers_min_uploads, 4, "GPU/Debug",
                     "Uploads of a page before shared_memory_log_writers logs writes to it");

namespace {

#ifdef _WIN32
struct WriterStats {
  uint64_t count = 0;
  uint64_t bytes = 0;
};
std::map<std::array<uint64_t, 3>, WriterStats> g_writers;
std::chrono::steady_clock::time_point g_writers_last_log;

void RecordWriter(uint32_t length) {
  static uint64_t module_begin = 0, module_end = 0;
  if (!module_begin) {
    auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    module_begin = uint64_t(base);
    module_end = module_begin + nt->OptionalHeader.SizeOfImage;
  }
  void* frames[32];
  USHORT frame_count = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
  std::array<uint64_t, 3> key = {};
  size_t key_count = 0;
  for (USHORT i = 0; i < frame_count && key_count < key.size(); ++i) {
    uint64_t address = uint64_t(frames[i]);
    if (address >= module_begin && address < module_end) {
      key[key_count++] = address;
    }
  }
  WriterStats& stats = g_writers[key];
  ++stats.count;
  stats.bytes += length;
  auto now = std::chrono::steady_clock::now();
  if (now - g_writers_last_log < std::chrono::seconds(10)) {
    return;
  }
  g_writers_last_log = now;
  static bool symbols_initialized = false;
  HANDLE process = GetCurrentProcess();
  if (!symbols_initialized) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    symbols_initialized = true;
  }
  auto name_of = [process](uint64_t address) -> std::string {
    if (!address) {
      return "-";
    }
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, address, &displacement, symbol)) {
      return symbol->Name;
    }
    return fmt::format("{:X}", address - module_begin);
  };
  std::vector<std::pair<WriterStats, std::array<uint64_t, 3>>> sorted;
  for (const auto& [frames_key, writer_stats] : g_writers) {
    sorted.emplace_back(writer_stats, frames_key);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.first.count > b.first.count; });
  REXGPU_INFO("Writers of GPU-used memory since the start:");
  for (size_t i = 0; i < sorted.size() && i < 25; ++i) {
    REXGPU_INFO("  {:8} faults {:10} bytes: {} <- {} <- {}", sorted[i].first.count,
                sorted[i].first.bytes, name_of(sorted[i].second[0]),
                name_of(sorted[i].second[1]), name_of(sorted[i].second[2]));
  }
}
#endif

}  // namespace
namespace rex::graphics {

SharedMemory::SharedMemory(memory::Memory& memory) : memory_(memory) {
  page_size_log2_ = rex::log2_ceil(uint32_t(rex::memory::page_size()));
}

SharedMemory::~SharedMemory() {
  ShutdownCommon();
}

void SharedMemory::InitializeCommon() {
  num_system_page_flags_ = ((kBufferSize >> page_size_log2_) + 63) / 64;
  system_page_flags_valid_.assign(num_system_page_flags_, 0);
  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);
  system_page_flags_requested_.assign(num_system_page_flags_, 0);
  system_page_flags_hot_.assign(num_system_page_flags_, 0);
  system_page_flags_used_in_submission_.assign(num_system_page_flags_, 0);
  page_used_bytes_.assign(kBufferSize >> page_size_log2_, 0);
  system_page_flags_uploaded_in_order_.assign(num_system_page_flags_, 0);
  page_upload_counts_.assign(kBufferSize >> page_size_log2_, 0);
  hot_pages_.clear();
  hot_pages_.resize(kBufferSize >> page_size_log2_);
  hot_page_count_ = 0;

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidationCallbackThunk, this);
}

void SharedMemory::InitializeSparseHostGpuMemory(uint32_t granularity_log2) {
  assert_true(granularity_log2 <= kBufferSizeLog2);
  assert_true(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      size_t(1) << (std::max(kBufferSizeLog2 - granularity_log2, uint32_t(6)) - 6));
}

void SharedMemory::ShutdownCommon() {
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
  assert_true(global_watches_.empty());
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();

  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }

  if (host_gpu_memory_sparse_used_bytes_) {
    host_gpu_memory_sparse_used_bytes_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb", 0);
  }
  if (host_gpu_memory_sparse_allocations_) {
    host_gpu_memory_sparse_allocations_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations", 0);
  }
  host_gpu_memory_sparse_allocated_.clear();
  host_gpu_memory_sparse_allocated_.shrink_to_fit();
  host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;

  system_page_flags_valid_.clear();
  system_page_flags_valid_.shrink_to_fit();
  system_page_flags_valid_and_gpu_written_.clear();
  system_page_flags_valid_and_gpu_written_.shrink_to_fit();
  system_page_flags_requested_.clear();
  system_page_flags_requested_.shrink_to_fit();
  system_page_flags_hot_.clear();
  system_page_flags_hot_.shrink_to_fit();
  system_page_flags_used_in_submission_.clear();
  system_page_flags_used_in_submission_.shrink_to_fit();
  page_upload_counts_.clear();
  page_upload_counts_.shrink_to_fit();
  hot_pages_.clear();
  hot_pages_.shrink_to_fit();
  hot_page_count_ = 0;
  num_system_page_flags_ = 0;
}

void SharedMemory::InvalidateAllPages() {
  auto global_lock = global_critical_region_.Acquire();

  std::fill(system_page_flags_valid_.begin(), system_page_flags_valid_.end(), uint64_t(0));
  std::fill(system_page_flags_valid_and_gpu_written_.begin(),
            system_page_flags_valid_and_gpu_written_.end(), uint64_t(0));
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  auto global_lock = global_critical_region_.Acquire();

  // Pages that are valid only because the CPU uploaded them lose their valid
  // bit here, so the next frame re-reads them from guest memory. Hot pages are
  // compared with guest memory on every request anyway, so they stay valid.
  for (uint32_t i = 0; i < num_system_page_flags_; ++i) {
    system_page_flags_valid_[i] = system_page_flags_valid_and_gpu_written_[i] |
                                  (system_page_flags_valid_[i] & system_page_flags_hot_[i]);
  }
}

void SharedMemory::ResetSubmissionUsage() {
  auto global_lock = global_critical_region_.Acquire();
  std::fill(system_page_flags_used_in_submission_.begin(),
            system_page_flags_used_in_submission_.end(), uint64_t(0));
  std::fill(system_page_flags_uploaded_in_order_.begin(),
            system_page_flags_uploaded_in_order_.end(), uint64_t(0));
}

void SharedMemory::MarkUsedInSubmission(uint32_t start, uint32_t length) {
  const uint32_t page_mask = (uint32_t(1) << page_size_log2_) - 1;
  uint32_t end = start + length;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (end - 1) >> page_size_log2_;
  for (uint32_t page = page_first; page <= page_last; ++page) {
    uint32_t first = page == page_first ? start & page_mask : 0;
    uint32_t last_end = page == page_last ? ((end - 1) & page_mask) + 1 : page_mask + 1;
    uint64_t& used_bits = system_page_flags_used_in_submission_[page >> 6];
    uint64_t page_bit = uint64_t(1) << (page & 63);
    uint32_t& used_bytes = page_used_bytes_[page];
    if (used_bits & page_bit) {
      first = std::min(first, used_bytes & 0xFFFF);
      last_end = std::max(last_end, used_bytes >> 16);
    }
    used_bytes = first | (last_end << 16);
    used_bits |= page_bit;
  }
}

void SharedMemory::ClearCache() {
  // Keeping GPU-written data, so "invalidated by GPU".
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, true);
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();
  SetSystemPageBlocksValidWithGpuDataWritten();
}

SharedMemory::GlobalWatchHandle SharedMemory::RegisterGlobalWatch(GlobalWatchCallback callback,
                                                                  void* callback_context) {
  GlobalWatch* watch = new GlobalWatch;
  watch->callback = callback;
  watch->callback_context = callback_context;

  auto global_lock = global_critical_region_.Acquire();
  global_watches_.push_back(watch);

  return reinterpret_cast<GlobalWatchHandle>(watch);
}

void SharedMemory::UnregisterGlobalWatch(GlobalWatchHandle handle) {
  auto watch = reinterpret_cast<GlobalWatch*>(handle);

  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = std::find(global_watches_.begin(), global_watches_.end(), watch);
    assert_false(it == global_watches_.end());
    if (it != global_watches_.end()) {
      global_watches_.erase(it);
    }
  }

  delete watch;
}

SharedMemory::WatchHandle SharedMemory::WatchMemoryRange(uint32_t start, uint32_t length,
                                                         WatchCallback callback,
                                                         void* callback_context,
                                                         void* callback_data,
                                                         uint64_t callback_argument) {
  if (length == 0 || start >= kBufferSize) {
    return nullptr;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t watch_page_first = start >> page_size_log2_;
  uint32_t watch_page_last = (start + length - 1) >> page_size_log2_;
  uint32_t bucket_first = watch_page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  uint32_t bucket_last = watch_page_last << page_size_log2_ >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Watches rely on write protection, so hot pages under them must be protected
  // again. If one has changed since it was uploaded, the watcher may have used
  // stale data, so its watch is fired on the next request.
  if (hot_page_count_) {
    hot_pages_to_protect_.clear();
    for (uint32_t i = watch_page_first >> 6; i <= watch_page_last >> 6; ++i) {
      uint64_t bits = system_page_flags_hot_[i];
      if (i == watch_page_first >> 6) {
        bits &= ~((uint64_t(1) << (watch_page_first & 63)) - 1);
      }
      if (i == watch_page_last >> 6 && (watch_page_last & 63) != 63) {
        bits &= (uint64_t(1) << ((watch_page_last & 63) + 1)) - 1;
      }
      uint32_t bit;
      while (rex::bit_scan_forward(bits, &bit)) {
        bits &= ~(uint64_t(1) << bit);
        hot_pages_to_protect_.push_back((i << 6) + bit);
      }
    }
    if (!hot_pages_to_protect_.empty()) {
      std::vector<uint32_t> pages;
      pages.swap(hot_pages_to_protect_);
      if (ProtectHotPages(pages)) {
        for (uint32_t page : pages) {
          if (!(system_page_flags_valid_[page >> 6] & (uint64_t(1) << (page & 63)))) {
            hot_pages_changed_under_watch_.push_back(page);
          }
        }
      }
      pages.clear();
      pages.swap(hot_pages_to_protect_);
    }
  }

  // Allocate the range.
  WatchRange* range = watch_range_first_free_;
  if (range != nullptr) {
    watch_range_first_free_ = range->next_free;
  } else {
    if (watch_range_pools_.empty() || watch_range_current_pool_allocated_ >= kWatchRangePoolSize) {
      watch_range_pools_.push_back(new WatchRange[kWatchRangePoolSize]);
      watch_range_current_pool_allocated_ = 0;
    }
    range = &(watch_range_pools_.back()[watch_range_current_pool_allocated_++]);
  }
  range->callback = callback;
  range->callback_context = callback_context;
  range->callback_data = callback_data;
  range->callback_argument = callback_argument;
  range->page_first = watch_page_first;
  range->page_last = watch_page_last;

  // Allocate and link the nodes.
  WatchNode* node_previous = nullptr;
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_node_first_free_;
    if (node != nullptr) {
      watch_node_first_free_ = node->next_free;
    } else {
      if (watch_node_pools_.empty() || watch_node_current_pool_allocated_ >= kWatchNodePoolSize) {
        watch_node_pools_.push_back(new WatchNode[kWatchNodePoolSize]);
        watch_node_current_pool_allocated_ = 0;
      }
      node = &(watch_node_pools_.back()[watch_node_current_pool_allocated_++]);
    }
    node->range = range;
    node->range_node_next = nullptr;
    if (node_previous != nullptr) {
      node_previous->range_node_next = node;
    } else {
      range->node_first = node;
    }
    node_previous = node;
    node->bucket_node_previous = nullptr;
    node->bucket_node_next = watch_buckets_[i];
    if (watch_buckets_[i] != nullptr) {
      watch_buckets_[i]->bucket_node_previous = node;
    }
    watch_buckets_[i] = node;
  }

  return reinterpret_cast<WatchHandle>(range);
}

void SharedMemory::UnwatchMemoryRange(WatchHandle handle) {
  auto global_lock = global_critical_region_.Acquire();
  UnlinkWatchRange(reinterpret_cast<WatchRange*>(handle));
}

void SharedMemory::FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu) {
  uint32_t address_first = page_first << page_size_log2_;
  uint32_t address_last = (page_last << page_size_log2_) + ((1 << page_size_log2_) - 1);
  uint32_t bucket_first = address_first >> kWatchBucketSizeLog2;
  uint32_t bucket_last = address_last >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Fire global watches.
  for (const auto global_watch : global_watches_) {
    global_watch->callback(global_lock, global_watch->callback_context, address_first, address_last,
                           invalidated_by_gpu);
  }

  // Fire per-range watches.
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_buckets_[i];
    while (node != nullptr) {
      WatchRange* range = node->range;
      // Store the next node now since when the callback is triggered, the links
      // will be broken.
      node = node->bucket_node_next;
      if (page_first <= range->page_last && page_last >= range->page_first) {
        range->callback(global_lock, range->callback_context, range->callback_data,
                        range->callback_argument, invalidated_by_gpu);
        UnlinkWatchRange(range);
      }
    }
  }
}

void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t end = start + length - 1;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = end >> page_size_log2_;

  {
    auto global_lock = global_critical_region_.Acquire();
    for (uint32_t i = page_first >> 6; i <= page_last >> 6; ++i) {
      uint64_t bits = ~uint64_t(0);
      if (i == page_first >> 6) {
        bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
      }
      if (i == page_last >> 6 && (page_last & 63) != 63) {
        bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
      }
      system_page_flags_used_in_submission_[i] |= bits;
      // Written by the GPU: a later upload must stay after it.
      system_page_flags_uploaded_in_order_[i] |= bits;
    }
  }

  // Trigger modification callbacks so, for instance, resolved data is loaded to
  // the texture.
  FireWatches(page_first, page_last, true);

  // Mark the range as valid (so pages are not reuploaded until modified by the
  // CPU) and watch it so the CPU can reuse it and this will be caught.
  MakeRangeValid(start, length, true);
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                    uint32_t length_allocations) {
  assert_always(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

void SharedMemory::MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t last = start + length - 1;
  uint32_t valid_page_first = start >> page_size_log2_;
  uint32_t valid_page_last = last >> page_size_log2_;
  uint32_t valid_block_first = valid_page_first >> 6;
  uint32_t valid_block_last = valid_page_last >> 6;

  {
    auto global_lock = global_critical_region_.Acquire();

    // Valid pages that are not hot are still protected: a CPU write would have invalidated them.
    bool skip_protected_pages = written_by_gpu && REXCVAR_GET(shared_memory_skip_valid_page_protection);
    if (skip_protected_pages) {
      protected_page_blocks_.assign(system_page_flags_valid_.begin() + valid_block_first,
                                    system_page_flags_valid_.begin() + valid_block_last + 1);
      for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
        protected_page_blocks_[i - valid_block_first] &= ~system_page_flags_hot_[i];
      }
    }

    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t valid_bits = UINT64_MAX;
      if (i == valid_block_first) {
        valid_bits &= ~((uint64_t(1) << (valid_page_first & 63)) - 1);
      }
      if (i == valid_block_last && (valid_page_last & 63) != 63) {
        valid_bits &= (uint64_t(1) << ((valid_page_last & 63) + 1)) - 1;
      }
      system_page_flags_valid_[i] |= valid_bits;
      uint64_t& gpu_written = system_page_flags_valid_and_gpu_written_[i];
      gpu_written = written_by_gpu ? (gpu_written | valid_bits) : (gpu_written & ~valid_bits);
    }

    uint32_t hot_page_uploads = REXCVAR_GET(shared_memory_hot_page_uploads);
    protect_page_runs_.clear();
    if (written_by_gpu || !hot_page_uploads) {
      // GPU-written data must be protected to notice the CPU overwriting it.
      if (hot_page_count_) {
        for (uint32_t page = valid_page_first; page <= valid_page_last; ++page) {
          uint64_t page_bit = uint64_t(1) << (page & 63);
          if (system_page_flags_hot_[page >> 6] & page_bit) {
            system_page_flags_hot_[page >> 6] &= ~page_bit;
            hot_pages_[page].shadow.reset();
            --hot_page_count_;
          }
          page_upload_counts_[page] = 0;
        }
      }
      if (skip_protected_pages) {
        for (uint32_t page = valid_page_first; page <= valid_page_last; ++page) {
          if ((protected_page_blocks_[(page >> 6) - valid_block_first] >> (page & 63)) & 1) {
            continue;
          }
          if (!protect_page_runs_.empty() && protect_page_runs_.back().second + 1 == page) {
            protect_page_runs_.back().second = page;
          } else {
            protect_page_runs_.emplace_back(page, page);
          }
        }
      } else {
        protect_page_runs_.emplace_back(valid_page_first, valid_page_last);
      }
    } else {
      // Called before the data is read for uploading, so a write after taking
      // the copy is seen as a change on the next request.
      size_t page_size = size_t(1) << page_size_log2_;
      for (uint32_t page = valid_page_first; page <= valid_page_last; ++page) {
        uint64_t page_bit = uint64_t(1) << (page & 63);
        uint64_t& hot_bits = system_page_flags_hot_[page >> 6];
        if (!(hot_bits & page_bit)) {
          uint8_t& upload_count = page_upload_counts_[page];
          if (upload_count < UINT8_MAX) {
            ++upload_count;
          }
          if (upload_count < hot_page_uploads || IsPageWatched(page)) {
            if (!protect_page_runs_.empty() && protect_page_runs_.back().second + 1 == page) {
              protect_page_runs_.back().second = page;
            } else {
              protect_page_runs_.emplace_back(page, page);
            }
            continue;
          }
          hot_bits |= page_bit;
          hot_pages_[page].shadow = std::make_unique<uint8_t[]>(page_size);
          ++hot_page_count_;
        }
        HotPage& hot_page = hot_pages_[page];
        std::memcpy(hot_page.shadow.get(), memory().TranslatePhysical(page << page_size_log2_),
                    page_size);
        hot_page.unchanged_requests = 0;
      }
    }
  }

  if (memory_invalidation_callback_handle_) {
    uint64_t tmp_before = rex::perf::GetCounter(rex::perf::CounterId::kPageProtectCalls);
    for (const std::pair<uint32_t, uint32_t>& run : protect_page_runs_) {
      memory().EnablePhysicalMemoryAccessCallbacks(
          run.first << page_size_log2_, (run.second - run.first + 1) << page_size_log2_, true,
          false);
    }
  }
}

bool SharedMemory::IsPageWatched(uint32_t page) const {
  uint32_t bucket = page << page_size_log2_ >> kWatchBucketSizeLog2;
  for (const WatchNode* node = watch_buckets_[bucket]; node != nullptr;
       node = node->bucket_node_next) {
    if (node->range->page_first <= page && node->range->page_last >= page) {
      return true;
    }
  }
  return false;
}

bool SharedMemory::ProtectHotPages(const std::vector<uint32_t>& pages) {
  // Protect first, then compare, so a write in between is not missed.
  for (uint32_t page : pages) {
    memory().EnablePhysicalMemoryAccessCallbacks(page << page_size_log2_,
                                                 uint32_t(1) << page_size_log2_, true, false);
  }
  bool any_changed = false;
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t page : pages) {
    HotPage& hot_page = hot_pages_[page];
    if (!hot_page.shadow) {
      continue;
    }
    uint64_t page_bit = uint64_t(1) << (page & 63);
    if ((system_page_flags_valid_[page >> 6] & page_bit) &&
        std::memcmp(memory().TranslatePhysical(page << page_size_log2_), hot_page.shadow.get(),
                    size_t(1) << page_size_log2_)) {
      system_page_flags_valid_[page >> 6] &= ~page_bit;
      system_page_flags_valid_and_gpu_written_[page >> 6] &= ~page_bit;
      any_changed = true;
    }
    hot_page.shadow.reset();
    --hot_page_count_;
    system_page_flags_hot_[page >> 6] &= ~page_bit;
    page_upload_counts_[page] = 0;
  }
  return any_changed;
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  uint32_t bucket = range->page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  WatchNode* node = range->node_first;
  while (node != nullptr) {
    WatchNode* node_next = node->range_node_next;
    if (node->bucket_node_previous != nullptr) {
      node->bucket_node_previous->bucket_node_next = node->bucket_node_next;
    } else {
      watch_buckets_[bucket] = node->bucket_node_next;
    }
    if (node->bucket_node_next != nullptr) {
      node->bucket_node_next->bucket_node_previous = node->bucket_node_previous;
    }
    node->next_free = watch_node_first_free_;
    watch_node_first_free_ = node;
    node = node_next;
    ++bucket;
  }
  range->next_free = watch_range_first_free_;
  watch_range_first_free_ = range;
}

bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count) {
  if (ranges == nullptr || !count) {
    return true;
  }

  // Some texture or buffer is empty, for example - safe to draw in this case.
  // Reused between calls: this runs about once per draw.
  std::vector<std::pair<uint32_t, uint32_t>>& merged_ranges = merged_ranges_;
  merged_ranges.clear();
  for (size_t i = 0; i < count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length) {
      continue;
    }
    if (start > kBufferSize || (kBufferSize - start) < length) {
      return false;
    }
    merged_ranges.emplace_back(start, length);
  }
  if (merged_ranges.empty()) {
    return true;
  }

  SCOPE_profile_cpu_f("gpu");
  const uint64_t ticks_start = rex::perf::ReadTicks();
  uint64_t ticks_last = ticks_start;
  auto tick = [this, &ticks_last](uint32_t section) {
    uint64_t now = rex::perf::ReadTicks();
    stats_.ticks[section] += now - ticks_last;
    ticks_last = now;
  };
  struct TotalTicks {
    SharedMemory* self;
    uint64_t start;
    ~TotalTicks() { self->stats_.ticks[0] += rex::perf::ReadTicks() - start; }
  } total_ticks{this, ticks_start};

  if (merged_ranges.size() > 1) {
    std::sort(merged_ranges.begin(), merged_ranges.end(),
              [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
                return a.first < b.first;
              });
  }
  size_t merged_write = 0;
  for (size_t i = 1; i < merged_ranges.size(); ++i) {
    std::pair<uint32_t, uint32_t>& range_previous = merged_ranges[merged_write];
    const std::pair<uint32_t, uint32_t>& range_current = merged_ranges[i];
    uint64_t previous_end = uint64_t(range_previous.first) + uint64_t(range_previous.second);
    uint64_t current_start = uint64_t(range_current.first);
    if (current_start <= previous_end) {
      uint64_t current_end = current_start + uint64_t(range_current.second);
      if (current_end > previous_end) {
        range_previous.second = uint32_t(current_end - uint64_t(range_previous.first));
      }
    } else {
      merged_ranges[++merged_write] = range_current;
    }
  }
  merged_ranges.resize(merged_write + 1);

  for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
    if (!EnsureHostGpuMemoryAllocated(range.first, range.second)) {
      return false;
    }
  }

  upload_ranges_.clear();
  auto append_upload_range = [this](uint32_t page_start, uint32_t page_count) {
    if (!page_count) {
      return;
    }
    if (!upload_ranges_.empty()) {
      std::pair<uint32_t, uint32_t>& last_upload_range = upload_ranges_.back();
      if (last_upload_range.first + last_upload_range.second == page_start) {
        last_upload_range.second += page_count;
        return;
      }
    }
    upload_ranges_.emplace_back(page_start, page_count);
  };
  // Window, in pages, to which each request is widened (a power of two).
  uint32_t window_pages =
      std::max(REXCVAR_GET(shared_memory_upload_window_kb) << 10 >> page_size_log2_, uint32_t(1));
  window_pages = uint32_t(1) << rex::log2_floor(window_pages);
  const uint32_t page_count_total = kBufferSize >> page_size_log2_;
  if (hot_page_count_ || !hot_pages_changed_under_watch_.empty()) {
    std::vector<uint32_t> changed_under_watch;
    tick(1);
    {
      auto global_lock = global_critical_region_.Acquire();
      tick(2);
      changed_under_watch.swap(hot_pages_changed_under_watch_);
      hot_pages_to_protect_.clear();
      size_t page_size = size_t(1) << page_size_log2_;
      const bool compare_requested_bytes =
          REXCVAR_GET(shared_memory_hot_compare_requested_bytes);
      for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
        uint32_t page_first = range.first >> page_size_log2_;
        uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
        for (uint32_t i = page_first >> 6; i <= page_last >> 6; ++i) {
          uint64_t bits = system_page_flags_hot_[i] & system_page_flags_valid_[i];
          if (i == page_first >> 6) {
            bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
          }
          if (i == page_last >> 6 && (page_last & 63) != 63) {
            bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          }
          uint32_t bit;
          while (rex::bit_scan_forward(bits, &bit)) {
            bits &= ~(uint64_t(1) << bit);
            uint32_t page = (i << 6) + bit;
            HotPage& hot_page = hot_pages_[page];
            // Only the requested bytes have to match what the GPU has; other bytes are
            // compared when they are requested.
            uint32_t compare_first = 0;
            uint32_t compare_end = uint32_t(page_size);
            if (compare_requested_bytes) {
              uint32_t page_start = page << page_size_log2_;
              compare_first = std::max(range.first, page_start) - page_start;
              compare_end =
                  std::min(range.first + range.second, page_start + uint32_t(page_size)) -
                  page_start;
            }
            stats_.hot_compare_bytes += compare_end - compare_first;
            stats_.tag_compare_bytes[request_tag_ & 3] += compare_end - compare_first;
            if ((system_page_flags_used_in_submission_[i] >> bit) & 1) {
              uint32_t used_bytes = page_used_bytes_[page];
              uint32_t repeat_first = std::max(compare_first, used_bytes & 0xFFFF);
              uint32_t repeat_end = std::min(compare_end, used_bytes >> 16);
              if (repeat_end > repeat_first) {
                stats_.repeat_compare_bytes += repeat_end - repeat_first;
              }
            }
            if (std::memcmp(memory().TranslatePhysical((page << page_size_log2_) + compare_first),
                            hot_page.shadow.get() + compare_first,
                            compare_end - compare_first)) {
              ++stats_.hot_pages_changed;
              ++stats_.tag_pages_changed[request_tag_ & 3];
              system_page_flags_valid_[i] &= ~(uint64_t(1) << bit);
              system_page_flags_valid_and_gpu_written_[i] &= ~(uint64_t(1) << bit);
              hot_page.unchanged_requests = 0;
            } else if (++hot_page.unchanged_requests >= kHotPageUnchangedRequestsToProtect) {
              hot_pages_to_protect_.push_back(page);
            }
          }
        }
      }
    }
    tick(3);
    if (!hot_pages_to_protect_.empty()) {
      std::vector<uint32_t> pages;
      pages.swap(hot_pages_to_protect_);
      ProtectHotPages(pages);
      pages.clear();
      pages.swap(hot_pages_to_protect_);
    }
    for (uint32_t page : changed_under_watch) {
      FireWatches(page, page, false);
    }
    tick(4);
  } else {
    tick(1);
  }
  {
    auto global_lock = global_critical_region_.Acquire();
    tick(2);
    // Mark the requested pages and build the (merged) widened windows.
    upload_windows_.clear();
    bool any_requested_page_invalid = false;
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      for (uint32_t i = page_first >> 6; i <= page_last >> 6; ++i) {
        uint64_t bits = ~uint64_t(0);
        if (i == page_first >> 6) {
          bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
        }
        if (i == page_last >> 6 && (page_last & 63) != 63) {
          bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
        }
        system_page_flags_requested_[i] |= bits;
        any_requested_page_invalid |= (bits & ~system_page_flags_valid_[i]) != 0;
      }
      uint32_t window_first = page_first & ~(window_pages - 1);
      uint32_t window_last = std::min(page_last | (window_pages - 1), page_count_total - 1);
      if (!upload_windows_.empty() && window_first <= upload_windows_.back().second + 1) {
        upload_windows_.back().second = std::max(upload_windows_.back().second, window_last);
      } else {
        upload_windows_.emplace_back(window_first, window_last);
      }
    }
    // Upload every invalid page in the windows that the GPU has requested;
    // without widening that is exactly the invalid part of the requested ranges.
    // Only widen when something requested now has to be uploaded anyway.
    if (!any_requested_page_invalid) {
      upload_windows_.clear();
    }
    for (const std::pair<uint32_t, uint32_t>& window : upload_windows_) {
      uint32_t page_first = window.first;
      uint32_t page_last = window.second;
      for (uint32_t i = page_first >> 6; i <= page_last >> 6; ++i) {
        uint64_t bits = ~system_page_flags_valid_[i] & system_page_flags_requested_[i];
        if (i == page_first >> 6) {
          bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
        }
        if (i == page_last >> 6 && (page_last & 63) != 63) {
          bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
        }
        while (bits) {
          uint32_t run_start;
          rex::bit_scan_forward(bits, &run_start);
          uint64_t from_start = bits >> run_start;
          uint32_t run_length = 64 - run_start;
          uint32_t run_end;
          if (rex::bit_scan_forward(~from_start, &run_end)) {
            run_length = std::min(run_length, run_end);
          }
          append_upload_range((i << 6) + run_start, run_length);
          bits &= run_length + run_start >= 64 ? 0 : ~uint64_t(0) << (run_start + run_length);
        }
      }
    }
    // Pages not used by any earlier command of this submission can be uploaded
    // at its start.
    upload_ranges_at_submission_start_.clear();
    if (!upload_ranges_.empty() && REXCVAR_GET(shared_memory_upload_at_submission_start)) {
      upload_ranges_in_submission_.clear();
      const bool relaxed = REXCVAR_GET(shared_memory_relaxed_submission_uploads);
      for (const std::pair<uint32_t, uint32_t>& upload_range : upload_ranges_) {
        uint32_t page = upload_range.first;
        uint32_t page_end = upload_range.first + upload_range.second;
        while (page < page_end) {
          // Whether the upload must stay in order: an earlier command of the submission used
          // the page, and may need its previous contents.
          auto page_used = [this, relaxed](uint32_t page) -> uint64_t {
            uint64_t page_bit = uint64_t(1) << (page & 63);
            if (!(system_page_flags_used_in_submission_[page >> 6] & page_bit)) {
              return 0;
            }
            // A hot page's shadow holds the contents of its last upload. If the bytes used so
            // far are unchanged, uploading the page before everything else gives the earlier
            // commands the same data, unless an upload in order would then overwrite it.
            if (relaxed && (system_page_flags_hot_[page >> 6] & page_bit) &&
                !(system_page_flags_uploaded_in_order_[page >> 6] & page_bit) &&
                hot_pages_[page].shadow) {
              uint32_t used_bytes = page_used_bytes_[page];
              uint32_t first = used_bytes & 0xFFFF;
              uint32_t end = used_bytes >> 16;
              if (end <= first ||
                  !std::memcmp(memory().TranslatePhysical((page << page_size_log2_) + first),
                               hot_pages_[page].shadow.get() + first, end - first)) {
                return 0;
              }
            }
            system_page_flags_uploaded_in_order_[page >> 6] |= page_bit;
            return 1;
          };
          uint64_t used = page_used(page);
          uint32_t run_end = page + 1;
          while (run_end < page_end && page_used(run_end) == used) {
            ++run_end;
          }
          (used ? upload_ranges_in_submission_ : upload_ranges_at_submission_start_)
              .emplace_back(page, run_end - page);
          page = run_end;
        }
      }
      upload_ranges_.swap(upload_ranges_in_submission_);
    }
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      MarkUsedInSubmission(range.first, range.second);
    }
  }

  COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
  COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                    uint32_t(merged_ranges.size()));
  COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count",
                    uint32_t(upload_ranges_.size()));

  tick(5);
  ++stats_.requests;
  ++stats_.tag_requests[request_tag_ & 3];
  for (const std::pair<uint32_t, uint32_t>& range : upload_ranges_at_submission_start_) {
    stats_.upload_bytes_at_start += uint64_t(range.second) << page_size_log2_;
  }
  for (const std::pair<uint32_t, uint32_t>& range : upload_ranges_) {
    stats_.upload_bytes_in_order += uint64_t(range.second) << page_size_log2_;
  }
  if (!upload_ranges_at_submission_start_.empty() &&
      !UploadRangesAtSubmissionStart(upload_ranges_at_submission_start_)) {
    return false;
  }
  tick(6);
  if (upload_ranges_.empty()) {
    return true;
  }

  bool uploaded = UploadRanges(upload_ranges_);
  tick(7);
  return uploaded;
}

bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {
  std::pair<uint32_t, uint32_t> range(start, length);
  return RequestRanges(&range, 1);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallbackThunk(
    void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range) {
  return reinterpret_cast<SharedMemory*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (length == 0 || physical_address_start >= kBufferSize) {
    return std::make_pair(uint32_t(0), UINT32_MAX);
  }
  length = std::min(length, kBufferSize - physical_address_start);
  uint32_t physical_address_last = physical_address_start + (length - 1);

  uint32_t page_first = physical_address_start >> page_size_log2_;
  uint32_t page_last = physical_address_last >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire();

#ifdef _WIN32
  // Only pages uploaded often enough to be hot normally (run with hot pages off).
  if (REXCVAR_GET(shared_memory_log_writers) &&
      page_upload_counts_[page_first] >= REXCVAR_GET(shared_memory_log_writers_min_uploads)) {
    RecordWriter(length);
  }
#endif

  if (!exact_range) {
    uint32_t written_page_first = page_first;
    uint32_t written_page_last = page_last;
    // Check if a somewhat wider range (up to 256 KB with 4 KB pages) can be
    // invalidated - if no GPU-written data nearby that was not intended to be
    // invalidated since it's not in sync with CPU memory and can't be
    // reuploaded. It's a lot cheaper to upload some excess data than to catch
    // access violations - with 4 KB callbacks, 58410824 (being a
    // software-rendered game) runs at 4 FPS on Intel Core i7-3770, with 64 KB,
    // the CPU game code takes 3 ms to run per frame, but with 256 KB, it's
    // 0.7 ms.
    if (page_first & 63) {
      uint64_t gpu_written_start = system_page_flags_valid_and_gpu_written_[block_first];
      gpu_written_start &= (uint64_t(1) << (page_first & 63)) - 1;
      page_first = (page_first & ~uint32_t(63)) + (64 - rex::lzcnt(gpu_written_start));
    }
    if ((page_last & 63) != 63) {
      uint64_t gpu_written_end = system_page_flags_valid_and_gpu_written_[block_last];
      gpu_written_end &= ~((uint64_t(1) << ((page_last & 63) + 1)) - 1);
      page_last =
          (page_last & ~uint32_t(63)) + (std::max(rex::tzcnt(gpu_written_end), uint8_t(1)) - 1);
    }
    if (REXCVAR_GET(shared_memory_invalidation_stop_at_watches)) {
      // Textures are often placed next to data the CPU rewrites every frame,
      // and invalidating them would make them reload unchanged data. Keep the
      // pages of watches not overlapping the written pages protected.
      uint32_t bucket_first = page_first << page_size_log2_ >> kWatchBucketSizeLog2;
      uint32_t bucket_last = page_last << page_size_log2_ >> kWatchBucketSizeLog2;
      for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
        for (const WatchNode* node = watch_buckets_[i]; node != nullptr;
             node = node->bucket_node_next) {
          const WatchRange* range = node->range;
          if (range->page_last < written_page_first) {
            if (range->page_last >= page_first) {
              page_first = range->page_last + 1;
            }
          } else if (range->page_first > written_page_last) {
            if (range->page_first <= page_last) {
              page_last = range->page_first - 1;
            }
          }
        }
      }
    }
  }

  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    system_page_flags_valid_[i] &= ~invalidate_bits;
    system_page_flags_valid_and_gpu_written_[i] &= ~invalidate_bits;
  }

  FireWatches(page_first, page_last, false);

  return std::make_pair(page_first << page_size_log2_, (page_last - page_first + 1)
                                                           << page_size_log2_);
}

bool SharedMemory::EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length) {
  if (host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX) {
    return true;
  }
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t allocation_first =
      page_first << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  uint32_t allocation_last =
      page_last << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  while (true) {
    std::pair<size_t, size_t> allocation_range =
        rex::bit::GetNextRangeUnset(host_gpu_memory_sparse_allocated_.data(), allocation_first,
                                    allocation_last - allocation_first + 1);
    if (!allocation_range.second) {
      break;
    }
    if (!AllocateSparseHostGpuMemoryRange(uint32_t(allocation_range.first),
                                          uint32_t(allocation_range.second))) {
      return false;
    }
    rex::bit::SetRange(host_gpu_memory_sparse_allocated_.data(), allocation_range.first,
                       allocation_range.second);
    ++host_gpu_memory_sparse_allocations_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations",
                      host_gpu_memory_sparse_allocations_);
    host_gpu_memory_sparse_used_bytes_ += uint32_t(allocation_range.second)
                                          << host_gpu_memory_sparse_granularity_log2_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb",
                      (host_gpu_memory_sparse_used_bytes_ + ((1 << 20) - 1)) >> 20);
    allocation_first = uint32_t(allocation_range.first + allocation_range.second);
  }
  return true;
}

}  // namespace rex::graphics
