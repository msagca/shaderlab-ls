#include "hlsl/lexer.h"
#include <algorithm>
#include <cctype>
#include "common/util.h"
namespace sls {
std::vector<HlslToken> lexHlsl(std::string_view text, Span range) {
  std::vector<HlslToken> result;
  size_t i = range.begin;
  size_t end = std::min(range.end, text.size());
  bool lineStart = true;
  while (i < end) {
    char c = text[i];
    if (c == '\n') {
      lineStart = true;
      ++i;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    size_t begin = i;
    HlslTok kind = HlslTok::Punct;
    if (c == '/' && i + 1 < end && text[i + 1] == '/') {
      while (i < end && text[i] != '\n')
        ++i;
      kind = HlslTok::Comment;
    } else if (c == '/' && i + 1 < end && text[i + 1] == '*') {
      size_t close = text.find("*/", i + 2);
      i = close == std::string_view::npos || close + 2 > end ? end : close + 2;
      kind = HlslTok::Comment;
    } else if (c == '#' && lineStart) {
      while (i < end && text[i] != '\n') {
        if (text[i] == '\\' && i + 1 < end && (text[i + 1] == '\n' || (text[i + 1] == '\r' && i + 2 < end && text[i + 2] == '\n')))
          i += text[i + 1] == '\r' ? 2 : 1;
        ++i;
      }
      while (i > begin && (text[i - 1] == '\r' || text[i - 1] == ' ' || text[i - 1] == '\t'))
        --i;
      kind = HlslTok::Directive;
    } else if (c == '"' || c == '\'') {
      ++i;
      while (i < end && text[i] != c && text[i] != '\n')
        i += text[i] == '\\' ? 2 : 1;
      i = std::min(i + 1, end);
      kind = HlslTok::String;
    } else if (isIdentStart(c)) {
      while (i < end && isIdentChar(text[i]))
        ++i;
      kind = HlslTok::Ident;
    } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < end && std::isdigit(static_cast<unsigned char>(text[i + 1])))) {
      while (i < end && (isIdentChar(text[i]) || text[i] == '.' ||
                          ((text[i] == '+' || text[i] == '-') && (text[i - 1] == 'e' || text[i - 1] == 'E') && !text.substr(begin, 2).starts_with("0x"))))
        ++i;
      kind = HlslTok::Number;
    } else {
      ++i;
    }
    // A comment leaves the line where it was; anything else means a '#' after it starts no directive.
    lineStart = lineStart && kind == HlslTok::Comment;
    if (kind == HlslTok::Directive)
      lineStart = false;
    result.push_back({kind, {begin, i}});
  }
  return result;
}
} // namespace sls
