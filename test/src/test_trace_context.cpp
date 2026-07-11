#include "nestor/trace_context.hpp"

#include <regex>
#include <set>

#include "httplib.h"
#include <gtest/gtest.h>

namespace nestor::test {

// ── Parsing Tests ──────────────────────────────────────────────────────────

TEST(TraceContextTest, ParsesValidTraceparent) {
  std::string trace_id, span_id;
  const bool ok = detail::parse_traceparent(
      "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01", trace_id, span_id);
  EXPECT_TRUE(ok);
  EXPECT_EQ(trace_id, "0af7651916cd43dd8448eb211c80319c");
  EXPECT_EQ(span_id, "b7ad6b7169203331");
}

TEST(TraceContextTest, RejectsNonZeroVersionPrefix) {
  std::string trace_id, span_id;
  const bool ok = detail::parse_traceparent(
      "01-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01", trace_id, span_id);
  EXPECT_FALSE(ok);
}

TEST(TraceContextTest, RejectsMalformedLength) {
  std::string trace_id, span_id;
  // Missing closing part
  const bool ok = detail::parse_traceparent("00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331",
                                            trace_id, span_id);
  EXPECT_FALSE(ok);
}

TEST(TraceContextTest, RejectsAllZeroTraceId) {
  std::string trace_id, span_id;
  const bool ok = detail::parse_traceparent(
      "00-00000000000000000000000000000000-b7ad6b7169203331-01", trace_id, span_id);
  EXPECT_FALSE(ok);
}

TEST(TraceContextTest, RejectsAllZeroSpanId) {
  std::string trace_id, span_id;
  const bool ok = detail::parse_traceparent(
      "00-0af7651916cd43dd8448eb211c80319c-0000000000000000-01", trace_id, span_id);
  EXPECT_FALSE(ok);
}

TEST(TraceContextTest, RejectsUppercaseHex) {
  std::string trace_id, span_id;
  const bool ok = detail::parse_traceparent(
      "00-0AF7651916CD43DD8448EB211C80319C-b7ad6b7169203331-01", trace_id, span_id);
  EXPECT_FALSE(ok);
}

TEST(TraceContextTest, RejectsInvalidHexCharacters) {
  std::string trace_id, span_id;
  const bool ok = detail::parse_traceparent(
      "00-0af7651916cd43dd8448eb211c80319g-b7ad6b7169203331-01", trace_id, span_id);
  EXPECT_FALSE(ok);
}

// ── Fallback Chain Tests ───────────────────────────────────────────────────

TEST(TraceContextTest, ExtractTraceparentFromRequest) {
  httplib::Request req;
  req.set_header("traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");

  const auto ctx = extract_or_generate(req);
  EXPECT_EQ(ctx.trace_id, "0af7651916cd43dd8448eb211c80319c");
  EXPECT_EQ(ctx.span_id, "b7ad6b7169203331");
}

TEST(TraceContextTest, FallbackToXRequestIdWhenTraceparentAbsent) {
  httplib::Request req;
  req.set_header("X-Request-ID", "0af7651916cd43dd8448eb211c80319c");

  const auto ctx = extract_or_generate(req);
  EXPECT_EQ(ctx.trace_id, "0af7651916cd43dd8448eb211c80319c");
  EXPECT_EQ(ctx.span_id.length(), 16);
  EXPECT_TRUE(detail::is_lower_hex(ctx.span_id));
}

TEST(TraceContextTest, XRequestIdHyphenatedUuidNormalized) {
  httplib::Request req;
  // Hyphenated UUID v4 format (36 chars with hyphens) -> strip and lowercase -> 32 hex
  req.set_header("X-Request-ID", "0af76519-16cd-43dd-8448-eb211c80319c");

  const auto ctx = extract_or_generate(req);
  EXPECT_EQ(ctx.trace_id, "0af7651916cd43dd8448eb211c80319c");
  EXPECT_EQ(ctx.span_id.length(), 16);
}

TEST(TraceContextTest, XRequestIdUppercaseNormalized) {
  httplib::Request req;
  req.set_header("X-Request-ID", "0AF7651916CD43DD8448EB211C80319C");

  const auto ctx = extract_or_generate(req);
  EXPECT_EQ(ctx.trace_id, "0af7651916cd43dd8448eb211c80319c");
  EXPECT_EQ(ctx.span_id.length(), 16);
}

TEST(TraceContextTest, XRequestIdWrongLengthGeneratesFresh) {
  httplib::Request req;
  req.set_header("X-Request-ID", "toolong");

  const auto ctx = extract_or_generate(req);
  EXPECT_EQ(ctx.trace_id.length(), 32);
  EXPECT_EQ(ctx.span_id.length(), 16);
  EXPECT_TRUE(detail::is_lower_hex(ctx.trace_id));
  EXPECT_TRUE(detail::is_lower_hex(ctx.span_id));
}

TEST(TraceContextTest, XRequestIdNonHexGeneratesFresh) {
  httplib::Request req;
  req.set_header("X-Request-ID", "gggggggggggggggggggggggggggggggg");

  const auto ctx = extract_or_generate(req);
  EXPECT_EQ(ctx.trace_id.length(), 32);
  EXPECT_EQ(ctx.span_id.length(), 16);
  EXPECT_TRUE(detail::is_lower_hex(ctx.trace_id));
  EXPECT_TRUE(detail::is_lower_hex(ctx.span_id));
}

TEST(TraceContextTest, GeneratesFreshWhenAllAbsent) {
  httplib::Request req;

  const auto ctx = extract_or_generate(req);
  EXPECT_EQ(ctx.trace_id.length(), 32);
  EXPECT_EQ(ctx.span_id.length(), 16);
  EXPECT_TRUE(detail::is_lower_hex(ctx.trace_id));
  EXPECT_TRUE(detail::is_lower_hex(ctx.span_id));
}

// ── ID Generation Tests ────────────────────────────────────────────────────

TEST(TraceContextTest, GeneratedTraceIdIs32Hex) {
  const std::string id = detail::generate_trace_id();
  EXPECT_EQ(id.length(), 32);
  EXPECT_TRUE(detail::is_lower_hex(id));
}

TEST(TraceContextTest, GeneratedSpanIdIs16Hex) {
  const std::string id = detail::generate_span_id();
  EXPECT_EQ(id.length(), 16);
  EXPECT_TRUE(detail::is_lower_hex(id));
}

TEST(TraceContextTest, GeneratedIdsAreUnique) {
  std::set<std::string> trace_ids, span_ids;
  for (int i = 0; i < 1000; ++i) {
    trace_ids.insert(detail::generate_trace_id());
    span_ids.insert(detail::generate_span_id());
  }
  EXPECT_EQ(trace_ids.size(), 1000);
  EXPECT_EQ(span_ids.size(), 1000);
}

TEST(TraceContextTest, IsLowerHexAcceptsValidStrings) {
  EXPECT_TRUE(detail::is_lower_hex("0123456789abcdef"));
  EXPECT_TRUE(detail::is_lower_hex("ffffffffffffffff"));
  EXPECT_TRUE(detail::is_lower_hex("0"));
  EXPECT_TRUE(detail::is_lower_hex("a"));
}

TEST(TraceContextTest, IsLowerHexRejectsUppercase) {
  EXPECT_FALSE(detail::is_lower_hex("ABCDEF"));
  EXPECT_FALSE(detail::is_lower_hex("aBcDeF"));
}

TEST(TraceContextTest, IsLowerHexRejectsNonHex) {
  EXPECT_FALSE(detail::is_lower_hex("0123456789abcdefg"));
  EXPECT_FALSE(detail::is_lower_hex("xyz"));
  EXPECT_FALSE(detail::is_lower_hex(""));
}

// ── Traceparent Header Tests ───────────────────────────────────────────────

TEST(TraceContextTest, ToTraceparentHeaderFormat) {
  const TraceContext ctx{
      .trace_id = "0af7651916cd43dd8448eb211c80319c",
      .span_id = "b7ad6b7169203331",
  };

  const std::string header = to_traceparent_header(ctx);
  EXPECT_EQ(header, "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
}

TEST(TraceContextTest, ToTraceparentRoundTrip) {
  const TraceContext original{
      .trace_id = "1234567890abcdef1234567890abcdef",
      .span_id = "fedcba9876543210",
  };

  const std::string header = to_traceparent_header(original);

  std::string extracted_trace_id, extracted_span_id;
  const bool ok = detail::parse_traceparent(header, extracted_trace_id, extracted_span_id);

  EXPECT_TRUE(ok);
  EXPECT_EQ(extracted_trace_id, original.trace_id);
  EXPECT_EQ(extracted_span_id, original.span_id);
}

}  // namespace nestor::test
