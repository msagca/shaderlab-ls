#include "common/text.h"
#include <algorithm>
namespace sls {
namespace {
  // Length in bytes of the UTF-8 sequence starting with `lead`.
  size_t sequenceLength(unsigned char lead) {
    if (lead < 0x80)
      return 1;
    if ((lead >> 5) == 0x6)
      return 2;
    if ((lead >> 4) == 0xE)
      return 3;
    if ((lead >> 3) == 0x1E)
      return 4;
    return 1;
  }
} // namespace
LineIndex::LineIndex(std::string_view text) {
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n')
      starts_.push_back(i + 1);
  }
}
size_t LineIndex::lineStart(size_t line) const {
  return line < starts_.size() ? starts_[line] : starts_.back();
}
size_t LineIndex::lineEnd(std::string_view text, size_t line) const {
  size_t end = line + 1 < starts_.size() ? starts_[line + 1] - 1 : text.size();
  if (end > text.size())
    end = text.size();
  if (end > lineStart(line) && end <= text.size() && end > 0 && text[end - 1] == '\r')
    --end;
  return std::max(end, lineStart(line));
}
size_t LineIndex::lineOf(size_t offset) const {
  auto it = std::upper_bound(starts_.begin(), starts_.end(), offset);
  return static_cast<size_t>(it - starts_.begin()) - 1;
}
Position LineIndex::toPosition(std::string_view text, size_t offset, Encoding encoding) const {
  offset = std::min(offset, text.size());
  size_t line = lineOf(offset);
  size_t start = starts_[line];
  if (encoding == Encoding::Utf8) {
    return {static_cast<int>(line), static_cast<int>(offset - start)};
  }
  int units = 0;
  for (size_t i = start; i < offset;) {
    size_t len = sequenceLength(static_cast<unsigned char>(text[i]));
    units += len == 4 ? 2 : 1;
    i += len;
  }
  return {static_cast<int>(line), units};
}
size_t LineIndex::toOffset(std::string_view text, Position position, Encoding encoding) const {
  if (position.line < 0)
    return 0;
  if (static_cast<size_t>(position.line) >= starts_.size())
    return text.size();
  size_t start = starts_[position.line];
  size_t end = lineEnd(text, position.line);
  if (encoding == Encoding::Utf8) {
    return std::min(start + static_cast<size_t>(std::max(position.character, 0)), end);
  }
  int units = 0;
  size_t i = start;
  while (i < end && units < position.character) {
    size_t len = sequenceLength(static_cast<unsigned char>(text[i]));
    units += len == 4 ? 2 : 1;
    i += len;
  }
  return std::min(i, end);
}
} // namespace sls
