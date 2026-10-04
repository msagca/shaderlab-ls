#pragma once
#include <nlohmann/json.hpp>
#include "analysis/analysis.h"
#include "lsp/features.h"
namespace sls {
// The token types and modifiers semanticTokens() numbers its tokens by, for the server's capabilities.
nlohmann::json semanticTokensLegend();
// textDocument/semanticTokens/full: the tokens of the whole document, ShaderLab and HLSL alike, for an editor with
// nothing else to highlight them with. GLSL - a GLSLPROGRAM block or a .glsl file - gets its comments, strings,
// numbers and directives, and its names are left to a GLSL server.
nlohmann::json semanticTokens(const Analysis &analysis, const FeatureContext &context);
} // namespace sls
