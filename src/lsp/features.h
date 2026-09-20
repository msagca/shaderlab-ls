#pragma once
#include <filesystem>
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
} // namespace sls
