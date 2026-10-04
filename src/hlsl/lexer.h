#pragma once
#include <string_view>
#include <vector>
#include "common/text.h"
namespace sls {
enum class HlslTok { Ident,
  Number,
  String,
  Punct,
  Comment,
  Directive };
struct HlslToken {
  HlslTok kind;
  Span span;
};
// Lexes text[range] as HLSL. A directive is one token, from the '#' that starts a line to the end of that line and
// of the lines it continues onto with '\'. Each other punctuation character is a token of its own.
std::vector<HlslToken> lexHlsl(std::string_view text, Span range);
} // namespace sls
