// test_request_limits.cpp — Unit tests for request validation helpers.
// Issues #41/#67: body size, required fields, type enforcement, completion validation.

#include "projectnestor/request_limits.hpp"

#include "nlohmann/json.hpp"
#include <gtest/gtest.h>

namespace projectnestor::test {

using json = nlohmann::json;

// ── validate_research_submission ─────────────────────────────────────────────

TEST(RequestLimitsTest, ValidSubmissionPasses) {
  const json body = {{"idea", "a valid idea"}, {"context", "some context"}};
  const auto err = validate_research_submission(body);
  EXPECT_FALSE(err.has_value());
}

TEST(RequestLimitsTest, SubmissionWithOnlyIdeaPasses) {
  const json body = {{"idea", "just an idea"}};
  const auto err = validate_research_submission(body);
  EXPECT_FALSE(err.has_value());
}

TEST(RequestLimitsTest, MissingIdeaReturns400) {
  const json body = {{"context", "some context"}};
  const auto err = validate_research_submission(body);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->status, 400);
  EXPECT_NE(err->detail.find("idea"), std::string::npos);
}

TEST(RequestLimitsTest, EmptyIdeaReturns400) {
  const json body = {{"idea", ""}};
  const auto err = validate_research_submission(body);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->status, 400);
}

TEST(RequestLimitsTest, IdeaWrongTypeIntReturns400) {
  const json body = {{"idea", 42}};
  const auto err = validate_research_submission(body);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->status, 400);
}

TEST(RequestLimitsTest, IdeaWrongTypeBoolReturns400) {
  const json body = {{"idea", true}};
  const auto err = validate_research_submission(body);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->status, 400);
}

TEST(RequestLimitsTest, ContextWrongTypeReturns400) {
  const json body = {{"idea", "valid idea"}, {"context", 99}};
  const auto err = validate_research_submission(body);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->status, 400);
}

TEST(RequestLimitsTest, MaxBodyBytesIs64KiB) {
  // kMaxBodyBytes should be 64 * 1024.
  EXPECT_EQ(kMaxBodyBytes, 64u * 1024u);
}

// ── validate_completion ───────────────────────────────────────────────────────

TEST(CompletionLimitsTest, ValidCompletionPasses) {
  const json body = {{"summary", "Research complete"}};
  const auto err = validate_completion(body);
  EXPECT_FALSE(err.has_value());
}

TEST(CompletionLimitsTest, MissingSummaryReturns400) {
  const json body = json::object();
  const auto err = validate_completion(body);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->status, 400);
  EXPECT_NE(err->detail.find("summary"), std::string::npos);
}

TEST(CompletionLimitsTest, EmptySummaryReturns400) {
  const json body = {{"summary", ""}};
  const auto err = validate_completion(body);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->status, 400);
}

TEST(CompletionLimitsTest, SummaryWrongTypeReturns400) {
  const json body = {{"summary", 42}};
  const auto err = validate_completion(body);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->status, 400);
}

TEST(CompletionLimitsTest, ExtraFieldsAreIgnored) {
  const json body = {{"summary", "done"}, {"extra_field", "ignored"}};
  const auto err = validate_completion(body);
  EXPECT_FALSE(err.has_value());
}

}  // namespace projectnestor::test
