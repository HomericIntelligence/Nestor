// test_store_concurrency.cpp — Concurrency tests for Store.
// Issue #28: verify mutex-protected Store is safe under concurrent access.

#include "projectnestor/store.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace projectnestor::test {

// Issue #28: 50-thread concurrent submit — no data races.
// Run with TSAN to catch any missing locks.
TEST(StoreConcurrencyTest, ConcurrentSubmitNoRaces) {
  Store store;
  constexpr int kThreads = 50;
  constexpr int kPerThread = 20;

  std::atomic<int> success_count{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&store, &success_count, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        const json body = {{"idea", "idea-" + std::to_string(t) + "-" + std::to_string(i)}};
        const json result = store.submit_research(body);
        if (!result["id"].get<std::string>().empty()) {
          ++success_count;
        }
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  EXPECT_EQ(success_count.load(), kThreads * kPerThread);
}

// Issue #28: concurrent submit + complete — no deadlocks or data races.
TEST(StoreConcurrencyTest, ConcurrentSubmitAndComplete) {
  Store store;
  constexpr int kSubmitters = 10;
  constexpr int kPerThread = 10;

  // Pre-submit some items to complete.
  std::vector<std::string> ids;
  ids.reserve(kSubmitters * kPerThread);
  for (int i = 0; i < kSubmitters * kPerThread; ++i) {
    const json result = store.submit_research({{"idea", "pre-submitted-" + std::to_string(i)}});
    ids.push_back(result["id"].get<std::string>());
  }

  std::atomic<int> complete_count{0};
  std::vector<std::thread> threads;
  threads.reserve(kSubmitters * 2);

  // Half the threads submit new items.
  for (int t = 0; t < kSubmitters; ++t) {
    threads.emplace_back([&store, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        store.submit_research(
            {{"idea", "concurrent-" + std::to_string(t) + "-" + std::to_string(i)}});
      }
    });
  }

  // Other half complete pre-submitted items.
  for (int t = 0; t < kSubmitters; ++t) {
    threads.emplace_back([&store, &ids, &complete_count, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        const int idx = t * kPerThread + i;
        const json result = store.complete_research(ids[static_cast<std::size_t>(idx)]);
        if (!result.contains("error")) {
          ++complete_count;
        }
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  // All pre-submitted items should be completable.
  EXPECT_EQ(complete_count.load(), kSubmitters * kPerThread);
}

// Issue #21/#48: Store respects max_items cap — no unbounded growth.
TEST(StoreBoundedTest, EvictsOldestWhenFull) {
  constexpr std::size_t kMaxItems = 5;
  Store store{kMaxItems};

  std::vector<std::string> ids;
  for (int i = 0; i < 10; ++i) {
    const json result = store.submit_research({{"idea", "idea-" + std::to_string(i)}});
    ids.push_back(result["id"].get<std::string>());
  }

  // Only the last kMaxItems should still be in the store.
  const json stats = store.get_stats();
  EXPECT_LE(stats["items_count"].get<int>(), static_cast<int>(kMaxItems));

  // The first kMaxItems entries should have been evicted.
  for (std::size_t i = 0; i < kMaxItems; ++i) {
    const json item = store.get(ids[i]);
    EXPECT_TRUE(item.contains("error")) << "Item " << i << " should have been evicted";
  }

  // The last kMaxItems entries should be present.
  for (std::size_t i = kMaxItems; i < ids.size(); ++i) {
    const json item = store.get(ids[i]);
    EXPECT_FALSE(item.contains("error")) << "Item " << i << " should still be present";
  }
}

// Issue #64: Store::list returns paginated results.
TEST(StoreListTest, ListPaginatesCorrectly) {
  Store store;
  for (int i = 0; i < 10; ++i) {
    store.submit_research({{"idea", "idea-" + std::to_string(i)}});
  }

  const json page1 = store.list(0, 3);
  EXPECT_EQ(page1["items"].size(), 3u);
  EXPECT_EQ(page1["total"].get<int>(), 10);
  EXPECT_EQ(page1["offset"].get<int>(), 0);

  const json page2 = store.list(3, 3);
  EXPECT_EQ(page2["items"].size(), 3u);
  EXPECT_EQ(page2["offset"].get<int>(), 3);

  const json page4 = store.list(9, 3);
  EXPECT_EQ(page4["items"].size(), 1u);  // Only 1 item left.
}

// Issue #64: Store::get returns item for known ID.
TEST(StoreGetTest, GetKnownIdReturnsItem) {
  Store store;
  const json submit_result = store.submit_research({{"idea", "findable idea"}});
  const std::string id = submit_result["id"].get<std::string>();

  const json item = store.get(id);
  EXPECT_FALSE(item.contains("error"));
  EXPECT_EQ(item["id"].get<std::string>(), id);
  EXPECT_EQ(item["status"], "pending");
}

// Issue #64: Store::get returns error for unknown ID.
TEST(StoreGetTest, GetUnknownIdReturnsError) {
  Store store;
  const json item = store.get("nonexistent-id");
  EXPECT_TRUE(item.contains("error"));
  EXPECT_EQ(item["error"], "not_found");
}

}  // namespace projectnestor::test
