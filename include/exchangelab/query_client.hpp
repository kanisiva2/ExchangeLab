#pragma once

#include "exchangelab/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace exchangelab {

class QueryClient {
public:
  QueryClient(std::string host, std::uint16_t port);
  ~QueryClient();

  QueryClient(const QueryClient &) = delete;
  QueryClient &operator=(const QueryClient &) = delete;

  [[nodiscard]] bool connect(std::string &error_message);
  void close();
  [[nodiscard]] bool connected() const;
  [[nodiscard]] std::optional<std::string> query(InstrumentId instrument_id,
                                                 std::string &error_message);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace exchangelab
