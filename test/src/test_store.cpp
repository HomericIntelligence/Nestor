#include "projectnestor/store.hpp"

#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace projectnestor::test {

TEST(GenerateUuidTest, ReturnsValidV4Format) {
  const std::string uuid = detail::generate_uuid();
  const std::regex pattern(
      "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-"
      "[0-9a-f]{12}$");
  EXPECT_TRUE(std::regex_match(uuid, pattern)) << "uuid=" << uuid;
}

TEST(GenerateUuidTest, GeneratesUniqueValues) {
  std::set<std::string> ids;
  for (int i = 0; i < 100; ++i) {
    ids.insert(detail::generate_uuid());
  }
  EXPECT_EQ(ids.size(), 100u);
}

TEST(NowIso8601Test, ReturnsValidTimestamp) {
  const std::string ts = detail::now_iso8601();
  const std::regex pattern(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$)");
  EXPECT_TRUE(std::regex_match(ts, pattern)) << "ts=" << ts;
}

TEST(NowIso8601Test, ReturnsCurrentYear) {
  const std::string ts = detail::now_iso8601();
  const std::string year = ts.substr(0, 4);
  EXPECT_TRUE(year == "2025" || year == "2026" || year == "2027") << "year=" << year;
}

TEST(StoreTest, InitialStatsAreZero) {
  Store store;
  const json stats = store.get_stats();
  EXPECT_FALSE(stats.contains("active"));
  EXPECT_EQ(stats["completed"], 0);
  EXPECT_EQ(stats["pending"], 0);
}

TEST(StoreTest, SubmitResearchReturnsPendingWithId) {
  Store store;
  const json body = {{"idea", "test idea"}, {"context", "test context"}};
  const json result = store.submit_research(body);

  EXPECT_FALSE(result["id"].get<std::string>().empty());
  EXPECT_EQ(result["status"], "pending");
}

TEST(StoreTest, SubmitResearchIncrementsPending) {
  Store store;
  store.submit_research({{"idea", "a"}});
  const json stats = store.get_stats();
  EXPECT_EQ(stats["pending"], 1);
  EXPECT_FALSE(stats.contains("active"));
  EXPECT_EQ(stats["completed"], 0);
}

TEST(StoreTest, SubmitResearchHandlesMissingFields) {
  Store store;
  const json result = store.submit_research(json::object());
  EXPECT_FALSE(result["id"].get<std::string>().empty());
  EXPECT_EQ(result["status"], "pending");
}

TEST(StoreTest, SubmitResearchMultipleItems) {
  Store store;
  std::set<std::string> ids;
  for (int i = 0; i < 3; ++i) {
    const json result = store.submit_research({{"idea", "idea"}});
    ids.insert(result["id"].get<std::string>());
  }
  EXPECT_EQ(ids.size(), 3u);
  EXPECT_EQ(store.get_stats()["pending"], 3);
}

TEST(StoreTest, CompleteResearchTransitionsStatus) {
  Store store;
  const json submit_result = store.submit_research({{"idea", "test idea"}});
  const std::string id = submit_result["id"].get<std::string>();

  const json completed = store.complete_research(id);
  EXPECT_EQ(completed["id"], id);
  EXPECT_EQ(completed["status"], "completed");
  EXPECT_TRUE(completed.contains("completed_at"));

  const json stats = store.get_stats();
  EXPECT_EQ(stats["pending"], 0);
  EXPECT_EQ(stats["completed"], 1);
}

TEST(StoreTest, CompleteResearchUnknownIdReturnsError) {
  Store store;
  const json result = store.complete_research("nonexistent-id");
  EXPECT_EQ(result["error"], "not_found");
}

TEST(StoreTest, CompleteResearchMergesAllowedFields) {
  Store store;
  const json submit_result = store.submit_research({{"idea", "test idea"}});
  const std::string id = submit_result["id"].get<std::string>();

  const json metadata = {{"summary", "test summary"},
                         {"results", {{"count", 42}}},
                         {"references", json::array({"ref1", "ref2"})}};
  const json completed = store.complete_research(id, metadata);

  EXPECT_EQ(completed["summary"], "test summary");
  EXPECT_EQ(completed["results"]["count"], 42);
  EXPECT_EQ(completed["references"], json::array({"ref1", "ref2"}));
  EXPECT_EQ(completed["status"], "completed");
}

TEST(StoreTest, CompleteResearchDefaultMetadataArgWorks) {
  Store store;
  const json submit_result = store.submit_research({{"idea", "test idea"}});
  const std::string id = submit_result["id"].get<std::string>();

  const json completed = store.complete_research(id);
  EXPECT_EQ(completed["status"], "completed");
  EXPECT_TRUE(completed.contains("completed_at"));
  EXPECT_FALSE(completed.contains("summary"));
  EXPECT_FALSE(completed.contains("results"));
  EXPECT_FALSE(completed.contains("references"));
}

TEST(StoreTest, CompleteResearchIgnoresUnknownAndReservedFields) {
  Store store;
  const json submit_result =
      store.submit_research({{"idea", "original idea"}, {"context", "original context"}});
  const std::string id = submit_result["id"].get<std::string>();

  const json metadata = {{"id", "hacked-id"},
                         {"status", "hacked-status"},
                         {"idea", "evil idea"},
                         {"context", "evil context"},
                         {"submitted_at", "hacked-submitted"},
                         {"completed_at", "hacked-completed"},
                         {"random_field", "should be ignored"},
                         {"summary", "legitimate summary"}};
  const json completed = store.complete_research(id, metadata);

  // Reserved fields should be unchanged
  EXPECT_EQ(completed["id"], id);
  EXPECT_EQ(completed["status"], "completed");
  EXPECT_EQ(completed["idea"], "original idea");
  EXPECT_EQ(completed["context"], "original context");
  // Unknown fields should not appear
  EXPECT_FALSE(completed.contains("random_field"));
  // Allowed field should be present
  EXPECT_EQ(completed["summary"], "legitimate summary");
}

// Verify eager erase: completing an item removes it from the map so a second
// complete on the same id returns not_found.
TEST(StoreTest, CompleteResearchErasesItemFromMap) {
  Store store;
  const json submit_result = store.submit_research({{"idea", "test idea"}});
  const std::string id = submit_result["id"].get<std::string>();

  // First complete should succeed
  const json completed = store.complete_research(id);
  EXPECT_EQ(completed["status"], "completed");

  // Second complete on the same id should return not_found (item was erased)
  const json not_found = store.complete_research(id);
  EXPECT_EQ(not_found["error"], "not_found");
}

// Verify hard capacity rejection: submit returns {"error":"capacity"} when full.
TEST(StoreTest, SubmitRejectsWhenAtCapacity) {
  Store store(2);
  const json r1 = store.submit_research({{"idea", "a"}});
  const json r2 = store.submit_research({{"idea", "b"}});
  EXPECT_EQ(r1["status"], "pending");
  EXPECT_EQ(r2["status"], "pending");

  const json r3 = store.submit_research({{"idea", "c"}});
  EXPECT_TRUE(r3.contains("error"));
  EXPECT_EQ(r3["error"], "capacity");

  // Counter unchanged: still 2 pending, 0 completed
  const json stats = store.get_stats();
  EXPECT_EQ(stats["pending"], 2);
}

// Verify capacity slot is freed after completion: fill to cap, complete one,
// then the next submit succeeds.
TEST(StoreTest, CapacityRespectedAfterCompletion) {
  Store store(2);
  const json r1 = store.submit_research({{"idea", "a"}});
  store.submit_research({{"idea", "b"}});

  // At cap — next submit would fail
  const json full = store.submit_research({{"idea", "c"}});
  EXPECT_EQ(full["error"], "capacity");

  // Complete one item to free a slot
  const std::string id = r1["id"].get<std::string>();
  store.complete_research(id);

  // Now submit should succeed
  const json r3 = store.submit_research({{"idea", "c"}});
  EXPECT_FALSE(r3.contains("error"));
  EXPECT_EQ(r3["status"], "pending");
}

// Verify TTL-based eviction fires on the next submit_research call.
// Uses seconds(-1) as the TTL so any item is immediately eligible for eviction
// without relying on sub-second clock resolution (avoids same-tick flakiness).
TEST(StoreTest, PendingItemsEvictedOnNextSubmit) {
  Store store(10, std::chrono::seconds(-1));

  const json r1 = store.submit_research({{"idea", "first"}});
  EXPECT_EQ(r1["status"], "pending");

  // Second submit triggers the sweep; the first item is past TTL and evicted
  const json r2 = store.submit_research({{"idea", "second"}});
  EXPECT_EQ(r2["status"], "pending");

  const json stats = store.get_stats();
  EXPECT_EQ(stats["expired"], 1);
  EXPECT_EQ(stats["pending"], 1);
}

// Verify the R3 fix: complete_research does NOT trigger TTL eviction.
// A pending item at/past TTL must still be completeable — the caller's intent
// is honoured. The sweep runs only in submit_research.
TEST(StoreTest, CompleteDoesNotEvictTtlExpiredItem) {
  // Use negative TTL so any item is immediately eligible for TTL eviction
  Store store(10, std::chrono::seconds(-1));

  const json r1 = store.submit_research({{"idea", "test"}});
  EXPECT_EQ(r1["status"], "pending");
  const std::string id = r1["id"].get<std::string>();

  // complete_research must succeed even though the item is past TTL.
  // If it incorrectly ran a TTL sweep, it would evict the item and return
  // not_found — the test would catch that regression.
  const json completed = store.complete_research(id);
  EXPECT_FALSE(completed.contains("error")) << "Got error: " << completed.dump();
  EXPECT_EQ(completed["status"], "completed");
  EXPECT_EQ(completed["id"], id);
}

// Verify the expired counter is exposed in stats and starts at zero.
TEST(StoreTest, StatsExposesExpiredCounter) {
  Store store;
  const json stats = store.get_stats();
  EXPECT_TRUE(stats.contains("expired"));
  EXPECT_EQ(stats["expired"], 0);
}

// Verify pending_ count consistency under concurrent submit-only load.
// 8 threads × 100 submits, no completes, cap = 10000.
// Validates that ++pending_ happens inside the mutex (no lost increments).
TEST(StoreTest, PendingCountIsConsistentUnderInterleaving) {
  constexpr int kThreads = 8;
  constexpr int kSubmitsPerThread = 100;
  Store store(10000);

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&store]() {
      for (int i = 0; i < kSubmitsPerThread; ++i) {
        store.submit_research({{"idea", "test"}});
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }

  const json stats = store.get_stats();
  EXPECT_EQ(stats["pending"], kThreads * kSubmitsPerThread);
}

TEST(StoreTest, ConcurrentSubmitAndCompleteCounterConsistency) {
  Store store(1000);
  const int num_threads = 4;
  const int ops_per_thread = 250;

  std::vector<std::thread> threads;
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&store, ops_per_thread]() {
      for (int i = 0; i < ops_per_thread; ++i) {
        const json submit_result = store.submit_research({{"idea", "test"}});
        if (!submit_result.contains("error")) {
          const std::string id = submit_result["id"].get<std::string>();
          store.complete_research(id);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // After all operations, pending should be 0, completed should be total ops
  const json stats = store.get_stats();
  EXPECT_EQ(stats["pending"], 0);
  EXPECT_EQ(stats["completed"], num_threads * ops_per_thread);
}

TEST(StoreTest, GetResearchReturnsSubmittedItem) {
  Store store;
  const json body = {{"idea", "test idea"}, {"context", "test context"}};
  const json submit_result = store.submit_research(body);
  const std::string id = submit_result["id"].get<std::string>();

  const json item = store.get_research(id);
  EXPECT_EQ(item["id"], id);
  EXPECT_EQ(item["idea"], "test idea");
  EXPECT_EQ(item["context"], "test context");
  EXPECT_EQ(item["status"], "pending");
}

TEST(StoreTest, GetResearchUnknownIdReturnsError) {
  Store store;
  const json result = store.get_research("nonexistent-id");
  EXPECT_EQ(result["error"], "not_found");
}

TEST(StoreTest, ListResearchInitiallyEmpty) {
  Store store;
  const json result = store.list_research();
  EXPECT_EQ(result["count"], 0);
  EXPECT_TRUE(result["items"].is_array());
  EXPECT_TRUE(result["items"].empty());
}

TEST(StoreTest, ListResearchAfterSubmissions) {
  Store store;
  const json r1 = store.submit_research({{"idea", "idea one"}});
  const json r2 = store.submit_research({{"idea", "idea two"}});
  const std::string id1 = r1["id"].get<std::string>();
  const std::string id2 = r2["id"].get<std::string>();

  const json result = store.list_research();
  EXPECT_EQ(result["count"], 2);
  ASSERT_TRUE(result["items"].is_array());

  // Both ids must appear in the items list (order unspecified for unordered_map).
  bool found1 = false;
  bool found2 = false;
  for (const auto& item : result["items"]) {
    if (item["id"] == id1) {
      found1 = true;
    }
    if (item["id"] == id2) {
      found2 = true;
    }
  }
  EXPECT_TRUE(found1);
  EXPECT_TRUE(found2);
}

TEST(StoreTest, SubmitPersistsTraceId) {
  Store store;
  const std::string trace_id = "0123456789abcdef0123456789abcdef";
  const json body = {{"idea", "test"}};

  const json submit_result = store.submit_research(body, trace_id);
  const std::string id = submit_result["id"].get<std::string>();

  const json completed = store.complete_research(id);
  EXPECT_EQ(completed["trace_id"], trace_id);
}

TEST(StoreTest, SubmitWithoutTraceIdStoresEmpty) {
  Store store;
  const json body = {{"idea", "test"}};

  const json submit_result = store.submit_research(body);
  const std::string id = submit_result["id"].get<std::string>();

  const json completed = store.complete_research(id);
  EXPECT_EQ(completed["trace_id"], "");
}

}  // namespace projectnestor::test
