#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>
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
