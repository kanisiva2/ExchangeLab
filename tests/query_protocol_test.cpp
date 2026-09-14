#include "exchangelab/query_protocol.hpp"

#include <gtest/gtest.h>

namespace exchangelab {
namespace {

TEST(QueryProtocolTest, ParsesAValidQueryLine) {
  const auto parsed = parse_query_request("QUERY 1234\r", 50'000);
  ASSERT_TRUE(parsed.instrument_id.has_value());
  EXPECT_EQ(*parsed.instrument_id, 1234U);
  EXPECT_EQ(parsed.error, QueryParseError::none);
}

TEST(QueryProtocolTest, RejectsMalformedAndOutOfRangeQueries) {
  EXPECT_EQ(parse_query_request("query 12", 100).error,
            QueryParseError::malformed_command);
  EXPECT_EQ(parse_query_request("QUERY", 100).error,
            QueryParseError::malformed_command);
  EXPECT_EQ(parse_query_request("QUERY twelve", 100).error,
            QueryParseError::malformed_command);
  EXPECT_EQ(parse_query_request("QUERY 12 extra", 100).error,
            QueryParseError::malformed_command);
  EXPECT_EQ(parse_query_request("QUERY 100", 100).error,
            QueryParseError::invalid_instrument);
}

TEST(QueryProtocolTest, FormatsASelfDelimitedSortedResult) {
  const QueryResult result{
      .instrument_id = 5,
      .prices = {{1, 100'250'000, 7, 70}, {0, 101'000'000, 8, 80}},
      .missing_exchanges = {2},
  };

  EXPECT_EQ(format_query_response(result),
            "RESULT 5\n"
            "PRICE 1 $100.250000 sequence=7 source_ns=70\n"
            "PRICE 0 $101.000000 sequence=8 source_ns=80\n"
            "MISSING 2\n"
            "END\n");
  EXPECT_EQ(format_query_error("malformed-command"),
            "ERROR malformed-command\nEND\n");
}

} // namespace
} // namespace exchangelab
