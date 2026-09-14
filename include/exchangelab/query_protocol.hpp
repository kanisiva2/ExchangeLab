#pragma once

#include "exchangelab/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace exchangelab {

inline constexpr std::size_t kMaximumQueryLineSize = 128;

// Request:  QUERY <instrument-id>\n
// Success:  RESULT <instrument-id>\n
//           zero or more human-readable PRICE lines
//           MISSING <exchange-ids...>\n
//           END\n
// Error:    ERROR <reason>\n
//           END\n
// END makes a complete response visible even though TCP has no message edges.

enum class QueryParseError {
  none,
  malformed_command,
  invalid_instrument,
};

struct QueryParseResult {
  std::optional<InstrumentId> instrument_id;
  QueryParseError error{QueryParseError::none};
};

[[nodiscard]] QueryParseResult
parse_query_request(std::string_view line, std::uint32_t instrument_count);
[[nodiscard]] std::string format_query_response(const QueryResult &result);
[[nodiscard]] std::string format_query_error(std::string_view reason);

} // namespace exchangelab
