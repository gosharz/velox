/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/common/caching/AsyncDataCache.h"
#include "velox/common/caching/FileIds.h"

#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/executors/QueuedImmediateExecutor.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

DECLARE_bool(velox_exception_stacktrace);

using namespace facebook::velox;
using namespace facebook::velox::cache;

using facebook::velox::memory::MappedMemory;

// Represents a planned load from a file. Many of these constitute a load plan.
struct Request {
  Request(uint64_t _offset, uint64_t _size) : offset(_offset), size(_size) {}

  uint64_t offset;
  uint64_t size;
};

class AsyncDataCacheTest : public testing::Test {
 protected:
  static constexpr int32_t kNumFiles = 100;
  void initializeCache(int64_t maxBytes) {
    cache_ = std::make_shared<AsyncDataCache>(
        MappedMemory::createDefaultInstance(), maxBytes);
    for (auto i = 0; i < kNumFiles; ++i) {
      auto name = fmt::format("testing_file_{}", i);
      filenames_.push_back(StringIdLease(fileIds(), name));
    }
  }

  // Finds one entry from RAM, SSD or storage. Throws if the data
  // cannot be read, otherwise request.pin is a loaded CachePin with
  // the data for the range of 'request.size' bytes starting at
  // 'request.offset'.
  void loadOne(uint64_t fileNum, Request& request, bool injectError);

  // Brings the data for the ranges in 'requests' into cache. The individual
  // entries should be accessed with loadOne().
  void
  loadBatch(uint64_t fileNum, std::vector<Request>& requests, bool injectError);

  // Gets a pin on each of 'requests'individually. This checks the
  // contents via cache_'s verifyHook.
  void checkBatch(
      uint64_t fileNum,
      std::vector<Request>& requests,
      bool injectError) {
    for (auto& request : requests) {
      loadOne(fileNum, request, injectError);
    }
  }

  // Loads a sequence of entries from a number of files. Looks up a
  // number of entries, then loads the ones that nobody else is
  // loading. Stops after loading 'loadBytes' worth of entries. If
  // 'errorEveryNBatches' is non-0, every nth load batch will have a
  // bad read and wil be dropped. The entries of the failed batch read
  // will still be accessed one by one.
  void loadLoop(
      int64_t startOffset,
      int64_t loadBytes,
      int32_t errorEveryNBatches = 0);

  // Calls func on 'numThreads' in parallel.
  template <typename Func>
  void runThreads(int32_t numThreads, Func func) {
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (int32_t i = 0; i < numThreads; ++i) {
      threads.push_back(std::thread([this, i, func]() { func(i); }));
    }
    for (auto& thread : threads) {
      thread.join();
    }
  }

 public:
  // Deterministically fills 'allocation'  based on 'sequence'
  static void initializeContents(
      int64_t sequence,
      MappedMemory::Allocation& alloc) {
    bool first = true;
    for (int32_t i = 0; i < alloc.numRuns(); ++i) {
      MappedMemory::PageRun run = alloc.runAt(i);
      int64_t* ptr = reinterpret_cast<int64_t*>(run.data());
      int32_t numWords =
          run.numPages() * MappedMemory::kPageSize / sizeof(void*);
      for (int32_t offset = 0; offset < numWords; offset++) {
        if (first) {
          ptr[offset] = sequence;
          first = false;
        } else {
          ptr[offset] = offset + sequence;
        }
      }
    }
  }

  // Checks that the contents are consistent with what is set in
  // initializeContents.
  static void checkContents(
      const MappedMemory::Allocation& alloc,
      int32_t numBytes) {
    bool first = true;
    int64_t sequence;
    int32_t bytesChecked = sizeof(int64_t);
    for (int32_t i = 0; i < alloc.numRuns(); ++i) {
      MappedMemory::PageRun run = alloc.runAt(i);
      int64_t* ptr = reinterpret_cast<int64_t*>(run.data());
      int32_t numWords =
          run.numPages() * MappedMemory::kPageSize / sizeof(void*);
      for (int32_t offset = 0; offset < numWords; offset++) {
        if (first) {
          sequence = ptr[offset];
          first = false;
        } else {
          bytesChecked += sizeof(int64_t);
          if (bytesChecked >= numBytes) {
            return;
          }
          ASSERT_EQ(ptr[offset], offset + sequence);
        }
      }
    }
  }

  CachePin newEntry(uint64_t offset, int32_t size) {
    folly::SemiFuture<bool> wait(false);
    try {
      RawFileCacheKey key{filenames_[0].id(), offset};
      auto pin = cache_->findOrCreate(key, size, &wait);
      EXPECT_FALSE(pin.empty());
      EXPECT_TRUE(pin.entry()->isExclusive());
      pin.entry()->setPrefetch();
      return pin;
    } catch (const VeloxException& e) {
      return CachePin();
    };
  }

 protected:
  static folly::Executor* executor() {
    static auto executor = std::make_unique<folly::IOThreadPoolExecutor>(4);
    return executor.get();
  }

  std::shared_ptr<AsyncDataCache> cache_;
  std::vector<StringIdLease> filenames_;
};

class TestingCoalescedLoad : public CoalescedLoad {
 public:
  TestingCoalescedLoad(
      std::vector<RawFileCacheKey> keys,
      std::vector<int32_t> sizes,
      AsyncDataCache& cache)
      : CoalescedLoad(std::move(keys), std::move(sizes)), cache_(cache) {}

  void injectError(bool error) {
    injectError_ = error;
  }

  std::vector<CachePin> loadData(bool /*isPrefetch*/) override {
    std::vector<CachePin> pins;
    cache_.makePins(
        keys_,
        [&](int32_t index) { return sizes_[index]; },
        [&](int32_t /*index*/, CachePin pin) {
          pins.push_back(std::move(pin));
        });
    for (auto& pin : pins) {
      auto& buffer = pin.entry()->data();
      AsyncDataCacheTest::initializeContents(
          pin.entry()->key().offset + pin.entry()->key().fileNum.id(), buffer);
    }
    VELOX_CHECK(!injectError_, "Testing error");
    return pins;
  }

 protected:
  AsyncDataCache& cache_;
  std::vector<Request> requests_;
  bool injectError_{false};
};

namespace {
int64_t sizeAtOffset(int64_t offset) {
  return offset % 100000;
}
} // namespace

void AsyncDataCacheTest::loadOne(
    uint64_t fileNum,
    Request& request,
    bool injectError) {
  // Pattern for loading one pin.
  RawFileCacheKey key{fileNum, request.offset};
  for (;;) {
    folly::SemiFuture<bool> loadFuture(false);
    auto pin = cache_->findOrCreate(key, request.size, &loadFuture);
    if (pin.empty()) {
      // The pin was exclusive on another thread. Wait until it is no longer so
      // and retry.
      auto& exec = folly::QueuedImmediateExecutor::instance();
      std::move(loadFuture).via(&exec).wait();
      continue;
    }
    auto entry = pin.checkedEntry();
    if (entry->isShared()) {
      // The entry has data. In this case the entry was checked by verifyHook,
      // so no action here.
      VELOX_CHECK(!injectError, "Testing error");
      return;
    }
    // We have an uninitialized entry in exclusive mode. We fill it with data
    // and set it to shared. If we release this pin while still in exclusive
    // mode, the entry will be erased.

    // Load from storage.
    initializeContents(
        entry->key().offset + entry->key().fileNum.id(), entry->data());
    // The entry is filled and will be made visible. 'pin' is released on
    // return.
    entry->setExclusiveToShared();
    return;
  }
}

void AsyncDataCacheTest::loadBatch(
    uint64_t fileNum,
    std::vector<Request>& requests,
    bool injectError) {
  // Pattern for loading a set of buffers from a file: Divide the requested
  // ranges between already loaded and loadeable from
  // storage.
  std::vector<Request*> fromStorage;
  for (auto& request : requests) {
    RawFileCacheKey key{fileNum, request.offset};
    if (cache_->exists(key)) {
      continue;
    }
    // Schedule a CoalescedLoad with other keys that need loading from the same
    // source.
    fromStorage.push_back(&request);
  }

  // Make CoalescedLoads for pins from different sources.
  if (!fromStorage.empty()) {
    std::vector<CachePin> pins;
    std::vector<RawFileCacheKey> keys;
    std::vector<int32_t> sizes;
    for (auto request : fromStorage) {
      keys.push_back(RawFileCacheKey{fileNum, request->offset});
      sizes.push_back(request->size);
    }
    auto load = std::make_shared<TestingCoalescedLoad>(
        std::move(keys), std::move(sizes), *cache_);
    load->injectError(injectError);
    executor()->add([load]() { load->loadOrFuture(nullptr); });
  }
}

void AsyncDataCacheTest::loadLoop(
    int64_t startOffset,
    int64_t loadBytes,
    int32_t errorEveryNBatches) {
  int64_t maxOffset =
      std::max<int64_t>(100000, (startOffset + loadBytes) / filenames_.size());
  int64_t skippedBytes = 0;
  int32_t errorCounter = 0;
  std::vector<Request> batch;
  for (auto file = 0; file < filenames_.size(); ++file) {
    auto fileNum = filenames_[file].id();
    for (uint64_t offset = 100; offset < maxOffset;
         offset += sizeAtOffset(offset)) {
      auto size = sizeAtOffset(offset);
      if (skippedBytes < startOffset) {
        skippedBytes += size;
        continue;
      }

      batch.emplace_back(offset, size);
      if (batch.size() >= 8) {
        for (;;) {
          bool injectError =
              errorEveryNBatches && (++errorCounter % errorEveryNBatches == 0);
          loadBatch(fileNum, batch, injectError);
          try {
            checkBatch(fileNum, batch, injectError);
          } catch (std::exception& e) {
            continue;
          }
          batch.clear();
          break;
        }
      }
    }
  }
}

TEST_F(AsyncDataCacheTest, pin) {
  constexpr int64_t kSize = 25000;
  initializeCache(1 << 20);
  auto& exec = folly::QueuedImmediateExecutor::instance();

  StringIdLease file(fileIds(), std::string_view("testingfile"));
  uint64_t offset = 1000;
  folly::SemiFuture<bool> wait(false);
  RawFileCacheKey key{file.id(), offset};
  auto pin = cache_->findOrCreate(key, kSize, &wait);
  EXPECT_FALSE(pin.empty());
  EXPECT_TRUE(wait.isReady());
  EXPECT_TRUE(pin.entry()->isExclusive());
  pin.entry()->setPrefetch();
  EXPECT_LE(kSize, pin.entry()->data().byteSize());
  EXPECT_LT(0, cache_->incrementPrefetchPages(0));
  auto stats = cache_->refreshStats();
  EXPECT_EQ(1, stats.numExclusive);
  EXPECT_LE(kSize, stats.largeSize);

  CachePin otherPin;
  EXPECT_THROW(otherPin = pin, VeloxException);
  EXPECT_TRUE(otherPin.empty());

  // Second reference to an exclusive entry.
  otherPin = cache_->findOrCreate(key, kSize, &wait);
  EXPECT_FALSE(wait.isReady());
  EXPECT_TRUE(otherPin.empty());
  bool noLongerExclusive = false;
  std::move(wait).via(&exec).thenValue([&](bool) { noLongerExclusive = true; });
  initializeContents(key.fileNum + key.offset, pin.checkedEntry()->data());
  pin.checkedEntry()->setExclusiveToShared();
  pin.clear();
  EXPECT_TRUE(pin.empty());

  EXPECT_TRUE(noLongerExclusive);

  pin = cache_->findOrCreate(key, kSize, &wait);
  EXPECT_TRUE(pin.entry()->isShared());
  EXPECT_TRUE(pin.entry()->getAndClearFirstUseFlag());
  EXPECT_FALSE(pin.entry()->getAndClearFirstUseFlag());
  checkContents(pin.entry()->data(), pin.entry()->size());
  otherPin = pin;
  EXPECT_EQ(2, pin.entry()->numPins());
  EXPECT_FALSE(pin.entry()->isPrefetch());
  pin.clear();
  otherPin.clear();
  stats = cache_->refreshStats();
  EXPECT_LE(kSize, stats.largeSize);
  EXPECT_EQ(1, stats.numEntries);
  EXPECT_EQ(0, stats.numShared);
  EXPECT_EQ(0, stats.numExclusive);

  cache_->clear();
  stats = cache_->refreshStats();
  EXPECT_EQ(0, stats.largeSize);
  EXPECT_EQ(0, stats.numEntries);
  EXPECT_EQ(0, cache_->incrementPrefetchPages(0));
}

TEST_F(AsyncDataCacheTest, replace) {
  constexpr int64_t kMaxBytes = 16 << 20;
  FLAGS_velox_exception_stacktrace = false;
  initializeCache(kMaxBytes);
  // Load 10x the max size, inject an error every 21 batches.
  loadLoop(0, kMaxBytes * 10, 21);
  auto stats = cache_->refreshStats();
  EXPECT_LT(0, stats.numEvict);
  EXPECT_GE(
      kMaxBytes / memory::MappedMemory::kPageSize,
      cache_->incrementCachedPages(0));
}

TEST_F(AsyncDataCacheTest, outOfCapacity) {
  constexpr int64_t kMaxBytes = 16 << 20;
  constexpr int32_t kSize = 16 << 10;
  constexpr int32_t kSizeInPages = kSize / MappedMemory::kPageSize;
  std::deque<CachePin> pins;
  std::deque<MappedMemory::Allocation> allocations;
  initializeCache(kMaxBytes);
  // We pin 2 16K entries and unpin 1. Eventually the whole capacity
  // is pinned and we fail making a ew entry.

  uint64_t offset = 0;
  for (;;) {
    pins.push_back(newEntry(++offset, kSize));
    pins.push_back(newEntry(++offset, kSize));
    if (pins.back().empty()) {
      break;
    }
    pins.pop_front();
  }
  MappedMemory::Allocation allocation(cache_.get());
  EXPECT_FALSE(cache_->allocate(kSizeInPages, 0, allocation));
  // One 4 page entry below the max size of 4K 4 page entries in 16MB of
  // capacity.
  EXPECT_EQ(4092, cache_->incrementCachedPages(0));
  EXPECT_EQ(4092, cache_->incrementPrefetchPages(0));
  pins.clear();

  // We allocate the full capacity and expect the cache entries to go.
  for (;;) {
    MappedMemory::Allocation(cache_.get());
    if (!cache_->allocate(kSizeInPages, 0, allocation)) {
      break;
    }
    allocations.push_back(std::move(allocation));
  }
  EXPECT_EQ(0, cache_->incrementCachedPages(0));
  EXPECT_EQ(0, cache_->incrementPrefetchPages(0));
  EXPECT_EQ(4092, cache_->numAllocated());
}
