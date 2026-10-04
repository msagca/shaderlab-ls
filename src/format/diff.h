#pragma once
#include <cstddef>
#include <string_view>
#include <vector>
namespace sls {
// Lines [fromBegin, fromEnd) of the old text replaced by lines [toBegin, toEnd) of the new. A hunk with an empty old
// range inserts before line fromBegin.
struct LineHunk {
  size_t fromBegin = 0;
  size_t fromEnd = 0;
  size_t toBegin = 0;
  size_t toEnd = 0;
};
// The lines of `text`, each with its line break. A text that does not end with one has a last line without it.
std::vector<std::string_view> splitLines(std::string_view text);
// The hunks that turn `from` into `to`, in order. Lines are matched ignoring their whitespace, so that a line only
// re-indented or re-spaced is its own one-line hunk instead of a part of a larger one.
std::vector<LineHunk> diffLines(const std::vector<std::string_view> &from, const std::vector<std::string_view> &to);
} // namespace sls
