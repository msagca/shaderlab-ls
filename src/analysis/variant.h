#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "hlsl/scanner.h"
namespace sls {
// What a variant is chosen by: for each keyword set, the document's own choice first, then the `keywords` setting's,
// then Unity's default variant.
struct VariantChoice {
  // A keyword the document enables, or "!K" for each keyword of a set it chose none of: "_" would name every set
  // that has one.
  std::vector<std::string> document;
  std::vector<std::string> global; // the `keywords` setting: "_" chooses none
};
// "_", "__", ...: the member of a keyword set that stands for none of its keywords.
bool isNoKeyword(std::string_view keyword);
// A #pragma that declares a set of keywords a variant picks from: multi_compile and shader_feature, not dynamic_branch,
// whose keywords are uniforms rather than variants.
bool isVariantPragma(const HlslPragma &pragma);
// Whether a variant may have none of a set's keywords: the set has "_", or is a lone shader_feature keyword, which
// is short for "_ K".
bool allowsNone(const HlslPragma &pragma);
// The keywords of a set the variant enables; empty for none. Without a choice, Unity's default: the first keyword,
// unless it is "_" or the set is a lone shader_feature keyword, which is off.
std::vector<std::string> enabledKeywords(const HlslPragma &pragma, const VariantChoice &choice);
} // namespace sls
