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

TEST(StoreTest, CompleteResearchErasesItem) {
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

TEST(StoreTest, EvictsOldestWhenCapExceeded) {
  Store store(3);
  std::string oldest_id;

  // Submit 4 items, cap is 3
  for (int i = 0; i < 4; ++i) {
    const json result = store.submit_research({{"idea", "idea"}});
    const std::string id = result["id"].get<std::string>();
    if (i == 0) {
      oldest_id = id;
    }
  }

  // The oldest item should have been evicted, so complete_research returns not_found
  const json evicted = store.complete_research(oldest_id);
  EXPECT_EQ(evicted["error"], "not_found");

  // The other 3 items should still be there
  const json stats = store.get_stats();
  EXPECT_EQ(stats["pending"], 3);
}

TEST(StoreTest, EvictionDecrementsPendingCounter) {
  Store store(2);

  // Submit 3 pending items, cap is 2
  store.submit_research({{"idea", "a"}});
  store.submit_research({{"idea", "b"}});
  store.submit_research({{"idea", "c"}});

  // The oldest item (a) should have been evicted
  const json stats = store.get_stats();
  EXPECT_EQ(stats["pending"], 2);
}

TEST(StoreTest, EvictionPreservesFifoOrder) {
  Store store(2);

  const json a_result = store.submit_research({{"idea", "a"}});
  const std::string a_id = a_result["id"].get<std::string>();

  const json b_result = store.submit_research({{"idea", "b"}});
  const std::string b_id = b_result["id"].get<std::string>();

  const json c_result = store.submit_research({{"idea", "c"}});
  // const std::string c_id = c_result["id"].get<std::string>();

  // Only a (oldest) should be evicted
  EXPECT_EQ(store.complete_research(a_id)["error"], "not_found");

  // b and c should still be completeable
  const json b_completed = store.complete_research(b_id);
  EXPECT_EQ(b_completed["status"], "completed");

  const json c_completed = store.complete_research(c_result["id"].get<std::string>());
  EXPECT_EQ(c_completed["status"], "completed");
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
        const std::string id = submit_result["id"].get<std::string>();
        store.complete_research(id);
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

  // All items should have been erased
  // Verify by attempting to complete a few non-existent ids
  const json check1 = store.complete_research("nonexistent-1");
  EXPECT_EQ(check1["error"], "not_found");

  const json check2 = store.complete_research("nonexistent-2");
  EXPECT_EQ(check2["error"], "not_found");
}

}  // namespace projectnestor::test
