#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace sls {
// Byte range [begin, end) into a document's UTF-8 text.
struct Span {
  size_t begin = 0;
  size_t end = 0;
  bool containsInclusive(size_t offset) const {
    return offset >= begin && offset <= end;
  }
  bool empty() const {
    return begin >= end;
  }
};
enum class Severity { Error = 1,
  Warning = 2,
  Information = 3,
  Hint = 4 };
struct RelatedLocation {
  std::string path;
  int line = 0; // 0-based
  int column = 0; // 0-based, bytes
  std::string message;
};
struct Diagnostic {
  Span span;
  Severity severity = Severity::Error;
  std::string message;
  std::string code;
  std::string source = "shaderlab";
  std::optional<RelatedLocation> related;
};
// Position encodings negotiated with the client (LSP 3.17 positionEncoding).
enum class Encoding { Utf8,
  Utf16 };
struct Position {
  int line = 0;
  int character = 0;
};
class LineIndex {
public:
  LineIndex() = default;
  explicit LineIndex(std::string_view text);
  size_t lineCount() const {
    return starts_.size();
  }
  size_t lineStart(size_t line) const;
  // End of the line's content, excluding "\r\n" / "\n".
  size_t lineEnd(std::string_view text, size_t line) const;
  size_t lineOf(size_t offset) const;
  Position toPosition(std::string_view text, size_t offset, Encoding encoding) const;
  size_t toOffset(std::string_view text, Position position, Encoding encoding) const;
private:
  std::vector<size_t> starts_{0};
};
} // namespace sls
