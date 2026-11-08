#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/memory.h>
#include <rex/thread/mutex.h>

namespace rex::graphics {

// Manages memory for unconverted textures, resolve targets, vertex and index
// buffers that can be accessed from shaders with Xenon physical addresses, with
// system page size granularity.
class SharedMemory {
 public:
  static constexpr uint32_t kBufferSizeLog2 = 29;
  static constexpr uint32_t kBufferSize = 1 << kBufferSizeLog2;

  virtual ~SharedMemory();
  // Call in the implementation-specific ClearCache.
  virtual void ClearCache();
  void SetSystemPageBlocksValidWithGpuDataWritten();
  // Called when the backend ends a submission: the pages used by commands of
  // the next one are tracked from scratch (see UploadRangesAtSubmissionStart).
  void ResetSubmissionUsage();
  void InvalidateAllPages();

  typedef void (*GlobalWatchCallback)(const std::unique_lock<std::recursive_mutex>& global_lock,
                                      void* context, uint32_t address_first, uint32_t address_last,
                                      bool invalidated_by_gpu);
  typedef void* GlobalWatchHandle;
  // Registers a callback invoked when something is invalidated in the GPU
  // memory copy by the CPU or (if triggered explicitly - such as by a resolve)
  // by the GPU. It will be fired for writes to pages previously requested, but
  // may also be fired regardless of whether it was used by GPU emulation - for
  // example, if the game changes protection level of a memory range containing
  // the watched range.
  //
  // The callback is called within the global critical region.
  GlobalWatchHandle RegisterGlobalWatch(GlobalWatchCallback callback, void* callback_context);
  void UnregisterGlobalWatch(GlobalWatchHandle handle);
  typedef void (*WatchCallback)(const std::unique_lock<std::recursive_mutex>& global_lock,
                                void* context, void* data, uint64_t argument,
                                bool invalidated_by_gpu);
  typedef void* WatchHandle;
  // Registers a callback invoked when the specified memory range is invalidated
  // in the GPU memory copy by the CPU or (if triggered explicitly - such as by
  // a resolve) by the GPU. It will be fired for writes to pages previously
  // requested, but may also be fired regardless of whether it was used by GPU
  // emulation - for example, if the game changes protection level of a memory
  // range containing the watched range.
  //
  // Generally the context is the subsystem pointer (for example, the texture
  // cache), the data is the object (such as a texture), and the argument is
  // additional subsystem/object-specific data (such as whether the range
  // belongs to the base mip level or to the rest of the mips).
  //
  // Called with the global critical region locked. Do NOT watch or unwatch
  // ranges from within it! The watch for the callback is cancelled after the
  // callback - the handle becomes invalid.
  WatchHandle WatchMemoryRange(uint32_t start, uint32_t length, WatchCallback callback,
                               void* callback_context, void* callback_data,
                               uint64_t callback_argument);
  // Unregisters previously registered watched memory range.
  void UnwatchMemoryRange(WatchHandle handle);

  // Checks if the range has been updated, uploads new data if needed and
  // ensures the host GPU memory backing the range are resident. Returns true if
  // the range has been fully updated and is usable.
  bool RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count);
  bool RequestRange(uint32_t start, uint32_t length);

  // Marks the range and, if not exact_range, potentially its surroundings
  // (to up to the first GPU-written page, as an access violation exception
  // count optimization) as modified by the CPU, also invalidating GPU-written
  // pages directly in the range.
  std::pair<uint32_t, uint32_t> MemoryInvalidationCallback(uint32_t physical_address_start,
                                                           uint32_t length, bool exact_range);

  // Marks the range as containing GPU-generated data (such as resolves),
  // triggering modification callbacks, making it valid (so pages are not
  // copied from the main memory until they're modified by the CPU) and
  // protecting it. Before writing anything from the GPU side, RequestRange must
  // be called, to make sure, if the GPU writes don't overwrite *everything* in
  // the pages they touch, the CPU data is properly loaded to the unmodified
  // regions in those pages.
  void RangeWrittenByGpu(uint32_t start, uint32_t length);

 protected:
  SharedMemory(memory::Memory& memory);
  // Call in implementation-specific initialization.
  void InitializeCommon();
  void InitializeSparseHostGpuMemory(uint32_t granularity_log2);
  // Call last in implementation-specific shutdown, also callable from the
  // destructor.
  void ShutdownCommon();

  // Sparse allocations are 4 MB, so not too many of them are allocated, but
  // also not to waste too much memory for padding (with 16 MB there's too
  // much).
  static constexpr uint32_t kHostGpuMemoryOptimalSparseAllocationLog2 = 22;
  static_assert(kHostGpuMemoryOptimalSparseAllocationLog2 <= kBufferSizeLog2);

  memory::Memory& memory() const { return memory_; }

  uint32_t page_size_log2() const { return page_size_log2_; }

  uint32_t host_gpu_memory_sparse_granularity_log2() const {
    return host_gpu_memory_sparse_granularity_log2_;
  }

  // Allocations in the host buffer are aligned the same way as in the guest
  // physical memory (for instance, if an allocation is 64 KB, it can represent
  // 0-64 KB, 64-128 KB, 128-192 KB in the guest memory, and so on, but not
  // something like 16-80 KB. This is assumed by the rules for texture data
  // access in the texture cache.
  virtual bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                uint32_t length_allocations);

  // Mark the memory range as updated and protect it.
  void MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu);

  // Uploads a range of host pages - only called if host GPU sparse memory
  // allocation succeeded if needed. While uploading, MakeRangeValid must be
  // called for each successfully uploaded range as early as possible, before
  // the memcpy, to make sure invalidation that happened during the CPU -> GPU
  // memcpy isn't missed (upload_page_ranges is in pages because of this -
  // MakeRangeValid has page granularity). upload_page_ranges are sorted in
  // ascending address order, so front and back can be used to determine the
  // overall bounds of pages to be uploaded.
  virtual bool UploadRanges(
      const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) = 0;
  // Like UploadRanges, for pages no command in the current submission has used
  // yet, so the copies may be done before all of them, for instance not to
  // interrupt render passes.
  virtual bool UploadRangesAtSubmissionStart(
      const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
    return UploadRanges(upload_page_ranges);
  }

 private:
  memory::Memory& memory_;

  // Log2 of invalidation granularity (the system page size, but the dependency
  // on it is not hard - the access callback takes a range as an argument, and
  // touched pages of the buffer of this size will be invalidated).
  uint32_t page_size_log2_;

  bool EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length);
  uint32_t host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;
  std::vector<uint64_t> host_gpu_memory_sparse_allocated_;
  uint32_t host_gpu_memory_sparse_allocations_ = 0;
  uint32_t host_gpu_memory_sparse_used_bytes_ = 0;

  void* memory_invalidation_callback_handle_ = nullptr;
  void* memory_data_provider_handle_ = nullptr;

  // Ranges that need to be uploaded, generated by GetRangesToUpload (a
  // persistently allocated vector).
  std::vector<std::pair<uint32_t, uint32_t>> upload_ranges_;

  // Mutex between the guest memory subsystem and the command processor, to be
  // locked when checking or updating validity of pages/ranges and when firing
  // watches.
  rex::thread::global_critical_region global_critical_region_;

  // ***************************************************************************
  // Things below should be fully protected by global_critical_region.
  // ***************************************************************************

  // Pages whose contents in the buffer are in sync with guest memory.
  std::vector<uint64_t> system_page_flags_valid_;
  // Subset of valid pages containing data written by the GPU.
  std::vector<uint64_t> system_page_flags_valid_and_gpu_written_;
  // Pages the GPU has requested at least once. Uploads may be widened to
  // invalid pages in this set (shared_memory_upload_window_kb), so that one
  // write fault does not cost one re-protection per later draw.
  std::vector<uint64_t> system_page_flags_requested_;
  std::vector<std::pair<uint32_t, uint32_t>> upload_windows_;
  std::vector<std::pair<uint32_t, uint32_t>> merged_ranges_;
  uint32_t num_system_page_flags_ = 0;

  // Pages the CPU keeps rewriting between GPU requests ("hot",
  // shared_memory_hot_page_uploads) are not write-protected after every upload,
  // which would cost a VirtualProtect and an access violation each time.
  // Instead, their contents are compared with a copy taken before the upload
  // whenever the GPU requests them.
  struct HotPage {
    std::unique_ptr<uint8_t[]> shadow;
    uint32_t unchanged_requests = 0;
  };
  // Hot pages that stayed unchanged this many requests are protected again.
  static constexpr uint32_t kHotPageUnchangedRequestsToProtect = 256;
  std::vector<uint64_t> system_page_flags_hot_;
  std::vector<uint8_t> page_upload_counts_;
  // Indexed by page, a hot page has a shadow.
  std::vector<HotPage> hot_pages_;
  uint32_t hot_page_count_ = 0;
  std::vector<uint32_t> hot_pages_to_protect_;
  // Changed hot pages found when a watch was added, to fire watches for later.
  std::vector<uint32_t> hot_pages_changed_under_watch_;
  std::vector<std::pair<uint32_t, uint32_t>> protect_page_runs_;
  // Pages of the range made valid that are already protected (scratch for MakeRangeValid).
  std::vector<uint64_t> protected_page_blocks_;
  // Pages requested or written by the GPU in the current submission.
  std::vector<uint64_t> system_page_flags_used_in_submission_;
  // For pages used in the submission, the bytes used: first in the low half, end in the high.
  std::vector<uint32_t> page_used_bytes_;
  // Pages uploaded in order (not at the submission start) in the current submission.
  std::vector<uint64_t> system_page_flags_uploaded_in_order_;
  // Marks bytes [start, start + length) as used by the GPU in this submission.
  void MarkUsedInSubmission(uint32_t start, uint32_t length);
  std::vector<std::pair<uint32_t, uint32_t>> upload_ranges_in_submission_;
  std::vector<std::pair<uint32_t, uint32_t>> upload_ranges_at_submission_start_;
  bool IsPageWatched(uint32_t page) const;

 public:
  struct Stats {
    uint64_t upload_bytes_in_order = 0;
    uint64_t upload_bytes_at_start = 0;
    uint64_t hot_compare_bytes = 0;
    uint64_t hot_pages_changed = 0;
    uint64_t requests = 0;
    uint64_t repeat_compare_bytes = 0;
    // 0: total, 1: before the hot pages, 2: waiting for the lock, 3: hot page compares,
    // 4: protecting hot pages and watches, 5: marking and splitting uploads,
    // 6: uploads at the submission start, 7: uploads in order.
    uint64_t ticks[8] = {};
    // By request_tag_: 0 other, 1 vertex buffers, 2 index buffers, 3 textures.
    uint64_t tag_compare_bytes[4] = {};
    uint64_t tag_pages_changed[4] = {};
    uint64_t tag_requests[4] = {};
  };
  uint32_t request_tag_ = 0;
  Stats TakeStats() {
    Stats stats = stats_;
    stats_ = {};
    return stats;
  }

 protected:
  Stats stats_;
  // Protects hot pages again, and invalidates them if they have changed since
  // the last upload. Returns whether any of them has changed.
  bool ProtectHotPages(const std::vector<uint32_t>& pages);

  static std::pair<uint32_t, uint32_t> MemoryInvalidationCallbackThunk(
      void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range);

  struct GlobalWatch {
    GlobalWatchCallback callback;
    void* callback_context;
  };
  std::vector<GlobalWatch*> global_watches_;
  struct WatchNode;
  // Watched range placed by other GPU subsystems.
  struct WatchRange {
    union {
      struct {
        WatchCallback callback;
        void* callback_context;
        void* callback_data;
        uint64_t callback_argument;
        WatchNode* node_first;
        uint32_t page_first;
        uint32_t page_last;
      };
      WatchRange* next_free;
    };
  };
  // Node for faster checking of watches when pages have been written to - all
  // 512 MB are split into smaller equally sized buckets, and then ranges are
  // linearly checked.
  struct WatchNode {
    union {
      struct {
        WatchRange* range;
        // Link to another node of this watched range in the next bucket.
        WatchNode* range_node_next;
        // Links to nodes belonging to other watched ranges in the bucket.
        WatchNode* bucket_node_previous;
        WatchNode* bucket_node_next;
      };
      WatchNode* next_free;
    };
  };
  static constexpr uint32_t kWatchBucketSizeLog2 = 22;
  static constexpr uint32_t kWatchBucketCount = 1 << (kBufferSizeLog2 - kWatchBucketSizeLog2);
  WatchNode* watch_buckets_[kWatchBucketCount] = {};
  // Allocation from pools - taking new WatchRanges and WatchNodes from the free
  // list, and if there are none, creating a pool if the current one is fully
  // used, and linearly allocating from the current pool.
  static constexpr uint32_t kWatchRangePoolSize = 8192;
  static constexpr uint32_t kWatchNodePoolSize = 8192;
  std::vector<WatchRange*> watch_range_pools_;
  std::vector<WatchNode*> watch_node_pools_;
  uint32_t watch_range_current_pool_allocated_ = 0;
  uint32_t watch_node_current_pool_allocated_ = 0;
  WatchRange* watch_range_first_free_ = nullptr;
  WatchNode* watch_node_first_free_ = nullptr;
  // Triggers the watches (global and per-range), removing triggered range
  // watches.
  void FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu);
  // Unlinks and frees the range and its nodes. Call this in the global critical
  // region.
  void UnlinkWatchRange(WatchRange* range);
};

}  // namespace rex::graphics
