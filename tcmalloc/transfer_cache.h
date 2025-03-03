// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_TRANSFER_CACHE_H_
#define TCMALLOC_TRANSFER_CACHE_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/base/thread_annotations.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/transfer_cache_stats.h"

#ifndef TCMALLOC_SMALL_BUT_SLOW
#include "tcmalloc/transfer_cache_internals.h"
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

#ifndef TCMALLOC_SMALL_BUT_SLOW

class StaticForwarder {
 public:
  static size_t class_to_size(int size_class);
  static size_t num_objects_to_move(int size_class);
  static void *Alloc(size_t size) ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
};

class TransferCacheManager : public StaticForwarder {
  template <typename CentralFreeList, typename Manager>
  friend class internal_transfer_cache::TransferCache;
  using TransferCache =
      internal_transfer_cache::TransferCache<tcmalloc_internal::CentralFreeList,
                                             TransferCacheManager>;

  template <typename CentralFreeList, typename Manager>
  friend class internal_transfer_cache::RingBufferTransferCache;
  using RingBufferTransferCache =
      internal_transfer_cache::RingBufferTransferCache<
          tcmalloc_internal::CentralFreeList, TransferCacheManager>;

 public:
  constexpr TransferCacheManager() : next_to_evict_(1) {}

  TransferCacheManager(const TransferCacheManager &) = delete;
  TransferCacheManager &operator=(const TransferCacheManager &) = delete;

  void Init() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    use_ringbuffer_ = IsExperimentActive(
        Experiment::TEST_ONLY_TCMALLOC_RING_BUFFER_TRANSFER_CACHE);
    for (int i = 0; i < kNumClasses; ++i) {
      if (use_ringbuffer_) {
        new (&cache_[i].rbtc) RingBufferTransferCache(this, i);
      } else {
        new (&cache_[i].tc) TransferCache(this, i);
      }
    }
  }

  void InsertRange(int size_class, absl::Span<void *> batch) {
    if (use_ringbuffer_) {
      cache_[size_class].rbtc.InsertRange(size_class, batch);
    } else {
      cache_[size_class].tc.InsertRange(size_class, batch);
    }
  }

  ABSL_MUST_USE_RESULT int RemoveRange(int size_class, void **batch, int n) {
    if (use_ringbuffer_) {
      return cache_[size_class].rbtc.RemoveRange(size_class, batch, n);
    } else {
      return cache_[size_class].tc.RemoveRange(size_class, batch, n);
    }
  }

  // This is not const because the underlying ring-buffer transfer cache
  // function requires acquiring a lock.
  size_t tc_length(int size_class) {
    if (use_ringbuffer_) {
      return cache_[size_class].rbtc.tc_length();
    } else {
      return cache_[size_class].tc.tc_length();
    }
  }

  TransferCacheStats GetHitRateStats(int size_class) const {
    if (use_ringbuffer_) {
      return cache_[size_class].rbtc.GetHitRateStats();
    } else {
      return cache_[size_class].tc.GetHitRateStats();
    }
  }

  const CentralFreeList &central_freelist(int size_class) const {
    if (use_ringbuffer_) {
      return cache_[size_class].rbtc.freelist();
    } else {
      return cache_[size_class].tc.freelist();
    }
  }

 private:
  int DetermineSizeClassToEvict();
  bool ShrinkCache(int size_class) {
    if (use_ringbuffer_) {
      return cache_[size_class].rbtc.ShrinkCache(size_class);
    } else {
      return cache_[size_class].tc.ShrinkCache(size_class);
    }
  }
  bool GrowCache(int size_class) {
    if (use_ringbuffer_) {
      return cache_[size_class].rbtc.GrowCache(size_class);
    } else {
      return cache_[size_class].tc.GrowCache(size_class);
    }
  }

  bool use_ringbuffer_ = false;
  std::atomic<int32_t> next_to_evict_;
  union Cache {
    constexpr Cache() : dummy(false) {}
    ~Cache() {}

    TransferCache tc;
    RingBufferTransferCache rbtc;
    bool dummy;
  };
  Cache cache_[kNumClasses];
} ABSL_CACHELINE_ALIGNED;

#else

// For the small memory model, the transfer cache is not used.
class TransferCacheManager {
 public:
  constexpr TransferCacheManager() : freelist_() {}
  TransferCacheManager(const TransferCacheManager &) = delete;
  TransferCacheManager &operator=(const TransferCacheManager &) = delete;

  void Init() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    for (int i = 0; i < kNumClasses; ++i) {
      freelist_[i].Init(i);
    }
  }

  void InsertRange(int size_class, absl::Span<void *> batch) {
    freelist_[size_class].InsertRange(batch);
  }

  ABSL_MUST_USE_RESULT int RemoveRange(int size_class, void **batch, int n) {
    return freelist_[size_class].RemoveRange(batch, n);
  }

  static constexpr size_t tc_length(int size_class) { return 0; }

  static constexpr TransferCacheStats GetHitRateStats(int size_class) {
    return {0, 0, 0, 0};
  }

  const CentralFreeList &central_freelist(int size_class) const {
    return freelist_[size_class];
  }

 private:
  CentralFreeList freelist_[kNumClasses];
} ABSL_CACHELINE_ALIGNED;

#endif

// A trivial no-op implementation.
struct ShardedTransferCacheManager {
  static constexpr void Init() {}
  static constexpr bool should_use(int cl) { return false; }
  static constexpr void *Pop(int cl) { return nullptr; }
  static constexpr void Push(int cl, void *ptr) {}
  static constexpr size_t TotalBytes() { return 0; }
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_TRANSFER_CACHE_H_
