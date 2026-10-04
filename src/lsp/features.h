#pragma once
#include <filesystem>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "analysis/analysis.h"
#include "lsp/workspace.h"
namespace sls {
struct FeatureContext {
  Encoding encoding = Encoding::Utf16;
  bool snippets = false;
  std::filesystem::path editorOverride;
  // For an include file: the files of the shaders that include it, which declare what it uses without including it.
  // Looked in after its own includes.
  std::vector<std::shared_ptr<const CachedFile>> outer;
};
nlohmann::json toRange(std::string_view text, const LineIndex &lines, Span span, Encoding encoding);
nlohmann::json completion(const Analysis &analysis, size_t offset, const FeatureContext &context);
nlohmann::json hover(const Analysis &analysis, size_t offset, const FeatureContext &context);
nlohmann::json definition(const Analysis &analysis, size_t offset, const FeatureContext &context);
nlohmann::json documentSymbols(const Analysis &analysis, const FeatureContext &context);
// The signatures of the function or method whose call `offset` is in: the document's and its includes' functions,
// overloads included, HLSL's intrinsics and the methods of the object before a '.'.
nlohmann::json signatureHelp(const Analysis &analysis, size_t offset, const FeatureContext &context);
// workspace/symbol: the functions, structs, cbuffers, globals and macros of the workspace's shader files, and the
// shaders by name, whose names have the query's letters in order, case aside.
nlohmann::json workspaceSymbols(const std::string &query, const Workspace &workspace, const FeatureContext &context);
// A request understood and declined, with the reason for the user. The server answers it as an error but does not
// log it as one: it is the answer.
struct RequestRefused : std::runtime_error {
  using std::runtime_error::runtime_error;
};
// References, highlights and renames all start from the symbol at `offset`. A symbol declared at file scope in an
// include file reaches every file of the workspace that includes it; any other stays within this document, and
// highlights always do. A rename that can't be done safely throws RequestRefused, with the reason, rather than guessing.
nlohmann::json references(const Analysis &analysis, const std::string &uri, size_t offset, bool includeDeclaration, const FeatureContext &context, const Workspace &workspace);
nlohmann::json documentHighlights(const Analysis &analysis, size_t offset, const FeatureContext &context);
nlohmann::json prepareRename(const Analysis &analysis, size_t offset, const FeatureContext &context, const Workspace &workspace);
nlohmann::json rename(const Analysis &analysis, const std::string &uri, size_t offset, const std::string &newName, const FeatureContext &context, const Workspace &workspace);
} // namespace sls
