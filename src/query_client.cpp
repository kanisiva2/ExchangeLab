#include "exchangelab/query_client.hpp"

#include <asio.hpp>

#include <istream>
#include <sstream>
#include <utility>

namespace exchangelab {

using asio::ip::tcp;

struct QueryClient::Impl {
  Impl(std::string server_host, const std::uint16_t server_port)
      : host(std::move(server_host)), port(server_port), resolver(event_loop),
        socket(event_loop) {}

  std::string host;
  std::uint16_t port;
  asio::io_context event_loop;
  tcp::resolver resolver;
  tcp::socket socket;
  asio::streambuf response_buffer;
};

QueryClient::QueryClient(std::string host, const std::uint16_t port)
    : impl_(std::make_unique<Impl>(std::move(host), port)) {}

QueryClient::~QueryClient() = default;

bool QueryClient::connect(std::string &error_message) {
  close();
  asio::error_code error;
  const auto endpoints =
      impl_->resolver.resolve(impl_->host, std::to_string(impl_->port), error);
  if (error) {
    error_message = error.message();
    return false;
  }

  asio::connect(impl_->socket, endpoints, error);
  if (error) {
    error_message = error.message();
    close();
    return false;
  }
  error_message.clear();
  return true;
}

void QueryClient::close() {
  asio::error_code ignored;
  impl_->socket.shutdown(tcp::socket::shutdown_both, ignored);
  impl_->socket.close(ignored);
  impl_->response_buffer.consume(impl_->response_buffer.size());
}

bool QueryClient::connected() const { return impl_->socket.is_open(); }

std::optional<std::string> QueryClient::query(const InstrumentId instrument_id,
                                              std::string &error_message) {
  if (!connected()) {
    error_message = "not connected";
    return std::nullopt;
  }

  const auto request = "QUERY " + std::to_string(instrument_id) + "\n";
  asio::error_code error;
  asio::write(impl_->socket, asio::buffer(request), error);
  if (error) {
    error_message = error.message();
    close();
    return std::nullopt;
  }

  std::string response;
  for (;;) {
    asio::read_until(impl_->socket, impl_->response_buffer, '\n', error);
    if (error) {
      error_message = error.message();
      close();
      return std::nullopt;
    }

    std::istream input(&impl_->response_buffer);
    std::string line;
    std::getline(input, line);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    response += line + '\n';
    if (line == "END") {
      error_message.clear();
      return response;
    }
  }
}

} // namespace exchangelab
