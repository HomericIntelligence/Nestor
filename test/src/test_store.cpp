#include "projectnestor/store.hpp"

#include <regex>
#include <set>
#include <string>

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

}  // namespace projectnestor::test
