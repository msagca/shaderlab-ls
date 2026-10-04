#include "format/diff.h"
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>
namespace sls {
std::vector<std::string_view> splitLines(std::string_view text) {
  std::vector<std::string_view> lines;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    end = end == std::string_view::npos ? text.size() : end + 1;
    lines.push_back(text.substr(start, end - start));
    start = end;
  }
  return lines;
}
namespace {
  std::string withoutSpaces(std::string_view line) {
    std::string key;
    for (char c : line) {
      if (!std::isspace(static_cast<unsigned char>(c)))
        key.push_back(c);
    }
    return key;
  }
  // Myers' algorithm: the pairs of lines the shortest edit script keeps, in order. Nothing when it would take more
  // than `limit` edits, which only a file rewritten from end to end comes near.
  std::optional<std::vector<std::pair<size_t, size_t>>> commonLines(const std::vector<std::string> &a, const std::vector<std::string> &b, int limit) {
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    const int max = n + m;
    std::vector<int> v(2 * static_cast<size_t>(max) + 3, 0);
    auto at = [&](int k) -> int & { return v[static_cast<size_t>(k + max + 1)]; };
    std::vector<std::vector<int>> trace; // v[-d..d] after each step d
    int found = -1;
    for (int d = 0; d <= std::min(max, limit) && found < 0; ++d) {
      for (int k = -d; k <= d; k += 2) {
        int x = k == -d || (k != d && at(k - 1) < at(k + 1)) ? at(k + 1) : at(k - 1) + 1;
        int y = x - k;
        while (x < n && y < m && a[static_cast<size_t>(x)] == b[static_cast<size_t>(y)]) {
          ++x;
          ++y;
        }
        at(k) = x;
        if (x >= n && y >= m) {
          found = d;
          break;
        }
      }
      trace.emplace_back(v.begin() + (max + 1 - d), v.begin() + (max + 1 + d + 1));
    }
    if (found < 0)
      return std::nullopt;
    std::vector<std::pair<size_t, size_t>> pairs;
    int x = n, y = m;
    for (int d = found; d >= 0; --d) {
      int k = x - y;
      int previousX = 0, previousY = 0;
      if (d > 0) {
        const std::vector<int> &before = trace[static_cast<size_t>(d - 1)];
        auto was = [&](int kk) { return before[static_cast<size_t>(kk + d - 1)]; };
        int previousK = k == -d || (k != d && was(k - 1) < was(k + 1)) ? k + 1 : k - 1;
        previousX = was(previousK);
        previousY = previousX - previousK;
        // The snake after the edit of step d starts where that edit ended.
        int startX = previousK == k + 1 ? previousX : previousX + 1;
        while (x > startX) {
          --x;
          --y;
          pairs.emplace_back(static_cast<size_t>(x), static_cast<size_t>(y));
        }
        x = previousX;
        y = previousY;
      } else {
        while (x > 0) {
          --x;
          --y;
          pairs.emplace_back(static_cast<size_t>(x), static_cast<size_t>(y));
        }
      }
    }
    std::reverse(pairs.begin(), pairs.end());
    return pairs;
  }
} // namespace
std::vector<LineHunk> diffLines(const std::vector<std::string_view> &from, const std::vector<std::string_view> &to) {
  size_t prefix = 0;
  while (prefix < from.size() && prefix < to.size() && from[prefix] == to[prefix])
    ++prefix;
  size_t suffix = 0;
  while (suffix < from.size() - prefix && suffix < to.size() - prefix && from[from.size() - 1 - suffix] == to[to.size() - 1 - suffix])
    ++suffix;
  std::vector<std::string> a, b;
  for (size_t i = prefix; i < from.size() - suffix; ++i)
    a.push_back(withoutSpaces(from[i]));
  for (size_t j = prefix; j < to.size() - suffix; ++j)
    b.push_back(withoutSpaces(to[j]));
  std::vector<LineHunk> hunks;
  // Hunks that only touch stay apart, so that a range takes in no more lines than it has to; an insertion is joined
  // to the hunk it touches, since two edits at one position have no order an editor agrees on.
  auto add = [&](LineHunk hunk) {
    auto inserts = [](const LineHunk &h) { return h.fromBegin == h.fromEnd; };
    if (!hunks.empty() && hunks.back().fromEnd == hunk.fromBegin && (inserts(hunks.back()) || inserts(hunk))) {
      hunks.back().fromEnd = hunk.fromEnd;
      hunks.back().toEnd = hunk.toEnd;
    } else {
      hunks.push_back(hunk);
    }
  };
  auto pairs = commonLines(a, b, 4000);
  if (!pairs) {
    if (!a.empty() || !b.empty())
      add({prefix, from.size() - suffix, prefix, to.size() - suffix});
    return hunks;
  }
  size_t i = 0, j = 0;
  pairs->emplace_back(a.size(), b.size()); // the end, so the last gap is a hunk too
  for (auto [x, y] : *pairs) {
    if (x > i || y > j)
      add({prefix + i, prefix + x, prefix + j, prefix + y});
    if (x < a.size() && from[prefix + x] != to[prefix + y])
      add({prefix + x, prefix + x + 1, prefix + y, prefix + y + 1});
    i = x + 1;
    j = y + 1;
  }
  return hunks;
}
} // namespace sls
