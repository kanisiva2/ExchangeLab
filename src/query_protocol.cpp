#include "exchangelab/query_protocol.hpp"

#include <charconv>
#include <iomanip>
#include <sstream>

namespace exchangelab {
namespace {

void write_price(std::ostream &output, const Price price) {
  output << '$' << price / kPriceScale << '.' << std::setfill('0')
         << std::setw(6) << price % kPriceScale << std::setfill(' ');
}

} // namespace

QueryParseResult parse_query_request(std::string_view line,
                                     const std::uint32_t instrument_count) {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }

  constexpr std::string_view prefix = "QUERY ";
  if (!line.starts_with(prefix)) {
    return {.error = QueryParseError::malformed_command};
  }

  const auto number = line.substr(prefix.size());
  if (number.empty()) {
    return {.error = QueryParseError::malformed_command};
  }

  InstrumentId instrument_id{};
  const auto [position, error] = std::from_chars(
      number.data(), number.data() + number.size(), instrument_id);
  if (error != std::errc{} || position != number.data() + number.size()) {
    return {.error = QueryParseError::malformed_command};
  }
  if (instrument_id >= instrument_count) {
    return {.error = QueryParseError::invalid_instrument};
  }
  return {.instrument_id = instrument_id, .error = QueryParseError::none};
}

std::string format_query_response(const QueryResult &result) {
  std::ostringstream output;
  output << "RESULT " << result.instrument_id << '\n';
  for (const auto &price : result.prices) {
    output << "PRICE " << price.exchange_id << ' ';
    write_price(output, price.price);
    output << " sequence=" << price.last_sequence
           << " source_ns=" << price.source_timestamp_ns << '\n';
  }

  output << "MISSING";
  if (result.missing_exchanges.empty()) {
    output << " none";
  } else {
    for (const auto exchange_id : result.missing_exchanges) {
      output << ' ' << exchange_id;
    }
  }
  output << "\nEND\n";
  return output.str();
}

std::string format_query_error(const std::string_view reason) {
  return "ERROR " + std::string(reason) + "\nEND\n";
}

} // namespace exchangelab
