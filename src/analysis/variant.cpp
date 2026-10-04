#include "analysis/variant.h"
#include <algorithm>
#include "shaderlab/reference.h"
namespace sls {
bool isNoKeyword(std::string_view keyword) {
  return !keyword.empty() && keyword.find_first_not_of('_') == std::string_view::npos;
}
bool isVariantPragma(const HlslPragma &pragma) {
  return ref::isKeywordPragma(pragma.name) && pragma.name.rfind("dynamic_branch", 0) != 0 && !pragma.args.empty();
}
bool allowsNone(const HlslPragma &pragma) {
  bool loneFeature = pragma.name.rfind("shader_feature", 0) == 0 && pragma.args.size() == 1;
  return loneFeature || std::any_of(pragma.args.begin(), pragma.args.end(), [](const PragmaArg &arg) { return isNoKeyword(arg.text); });
}
std::vector<std::string> enabledKeywords(const HlslPragma &pragma, const VariantChoice &choice) {
  std::vector<std::string> result;
  if (!isVariantPragma(pragma))
    return result;
  auto chose = [&](const std::string &entry) {
    return std::find(choice.document.begin(), choice.document.end(), entry) != choice.document.end();
  };
  for (const PragmaArg &arg : pragma.args) {
    if (!isNoKeyword(arg.text) && chose(arg.text) && std::find(result.begin(), result.end(), arg.text) == result.end())
      result.push_back(arg.text);
  }
  if (!result.empty())
    return result;
  bool none = allowsNone(pragma) &&
              std::all_of(pragma.args.begin(), pragma.args.end(), [&](const PragmaArg &arg) { return isNoKeyword(arg.text) || chose("!" + arg.text); });
  if (none)
    return result;
  for (const std::vector<std::string> *chosen : {&choice.global}) {
    bool any = false;
    for (const PragmaArg &arg : pragma.args) {
      if (std::find(chosen->begin(), chosen->end(), arg.text) == chosen->end())
        continue;
      any = true;
      if (!isNoKeyword(arg.text) && std::find(result.begin(), result.end(), arg.text) == result.end())
        result.push_back(arg.text);
    }
    if (any)
      return result;
  }
  const std::string &first = pragma.args[0].text;
  bool loneFeature = pragma.name.rfind("shader_feature", 0) == 0 && pragma.args.size() == 1;
  if (!isNoKeyword(first) && !loneFeature)
    result.push_back(first);
  return result;
}
} // namespace sls
