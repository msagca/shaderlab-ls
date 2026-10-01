#pragma once
#include <filesystem>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "analysis/analysis.h"
namespace sls {
struct FeatureContext {
  Encoding encoding = Encoding::Utf16;
  bool snippets = false;
  std::filesystem::path editorOverride;
};
nlohmann::json toRange(std::string_view text, const LineIndex &lines, Span span, Encoding encoding);
nlohmann::json completion(const Analysis &analysis, size_t offset, const FeatureContext &context);
nlohmann::json hover(const Analysis &analysis, size_t offset, const FeatureContext &context);
nlohmann::json definition(const Analysis &analysis, size_t offset, const FeatureContext &context);
nlohmann::json documentSymbols(const Analysis &analysis, const FeatureContext &context);
// A request understood and declined, with the reason for the user. The server answers it as an error but does not
// log it as one: it is the answer.
struct RequestRefused : std::runtime_error {
  using std::runtime_error::runtime_error;
};
// References, highlights and renames all start from the symbol at `offset` and stay within this document: nothing
// indexes the files around it. A rename that can't be done safely throws RequestRefused, with the reason, rather than guessing.
nlohmann::json references(const Analysis &analysis, const std::string &uri, size_t offset, bool includeDeclaration, const FeatureContext &context);
nlohmann::json documentHighlights(const Analysis &analysis, size_t offset, const FeatureContext &context);
nlohmann::json prepareRename(const Analysis &analysis, size_t offset, const FeatureContext &context);
nlohmann::json rename(const Analysis &analysis, const std::string &uri, size_t offset, const std::string &newName, const FeatureContext &context);
} // namespace sls
