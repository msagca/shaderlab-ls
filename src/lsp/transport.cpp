#include "lsp/transport.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <cstdio>
#include <cstdlib>

#include "common/util.h"

namespace sls {

Transport::Transport() {
#ifdef _WIN32
  // Elsewhere the streams are binary already.
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
}

std::optional<std::string> Transport::read() {
  size_t contentLength = 0;
  bool haveLength = false;
  std::string line;
  while (true) {
    line.clear();
    int c;
    while ((c = std::fgetc(stdin)) != EOF && c != '\n') line.push_back(static_cast<char>(c));
    if (c == EOF) return std::nullopt;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) {
      if (haveLength) break;
      continue;  // Tolerate stray blank lines between messages.
    }
    if (istartsWith(line, "Content-Length:")) {
      contentLength = std::strtoull(line.c_str() + 15, nullptr, 10);
      haveLength = true;
    }
  }
  std::string body(contentLength, '\0');
  size_t read = contentLength == 0 ? 0 : std::fread(body.data(), 1, contentLength, stdin);
  if (read != contentLength) return std::nullopt;
  return body;
}

void Transport::write(const nlohmann::json& message) {
  std::string body = message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  std::lock_guard lock(writeMutex_);
  std::fprintf(stdout, "Content-Length: %zu\r\n\r\n", body.size());
  std::fwrite(body.data(), 1, body.size(), stdout);
  std::fflush(stdout);
}

}  // namespace sls
