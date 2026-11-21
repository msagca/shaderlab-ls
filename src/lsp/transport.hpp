#pragma once
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
namespace sls {
// JSON-RPC over stdio with LSP "Content-Length" framing.
class Transport {
public:
  Transport();
  // Blocks until a full message body is read. Returns nullopt on EOF or a broken stream.
  std::optional<std::string> read();
  // Thread-safe.
  void write(const nlohmann::json &message);
private:
  std::mutex writeMutex_;
};
} // namespace sls
